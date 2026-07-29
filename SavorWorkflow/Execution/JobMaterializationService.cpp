#include "JobMaterializationService.h"

#include "Runner/Script/PSContextCodec.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace savor::runner::parallel::savordb {
namespace {
constexpr std::string_view kWorkflowJobMaterializerToken = "workflow_job_materializer";
std::atomic<std::uint64_t> g_claim_run_sequence{ 0 };

std::size_t EstimateEncodedInputBytes(const ClaimedJobRecord& record) {
    // Deterministic coordinator lookahead estimate only. The phase adapter
    // later supplies the exact encoded envelope size and the worker validates
    // that exact value against its negotiated limit.
    constexpr std::size_t kEnvelopeOverhead = 256;
    std::size_t bytes = kEnvelopeOverhead
        + record.runtime_init.savestate_ref_kind.size()
        + record.runtime_init.bootstrap_profile.size()
        + record.workset_execution_key.size();
    if (record.payload.has_value()) {
        bytes += record.payload->payload.size();
        std::vector<std::uint8_t> encoded_context;
        if (savor::psctx::encode_numeric(
                record.payload->ctx,
                encoded_context)) {
            bytes += encoded_context.size();
        }
    }
    return std::max<std::size_t>(1, bytes);
}

std::string MakeClaimTokenPrefix() {
    std::ostringstream token;
    token << kWorkflowJobMaterializerToken << ":run:"
          << std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::steady_clock::now().time_since_epoch())
                 .count()
          << ":" << (g_claim_run_sequence.fetch_add(1) + 1);
    return token.str();
}

bool IsMaterializerActiveClaimState(ClaimedJobLifecycleState state) {
    return state == ClaimedJobLifecycleState::Claimed
        || state == ClaimedJobLifecycleState::Materializing
        || state == ClaimedJobLifecycleState::Materialized
        || state == ClaimedJobLifecycleState::Dispatching
        || state == ClaimedJobLifecycleState::Dispatched;
}
}

std::size_t CoordinatorItemCapacitySnapshot::ConsumedCredits() const noexcept {
    const auto add_saturated = [](std::size_t lhs, std::size_t rhs) {
        const auto max = std::numeric_limits<std::size_t>::max();
        return rhs > max - lhs ? max : lhs + rhs;
    };
    auto consumed = add_saturated(coordinator_buffered, worker_resident);
    consumed = add_saturated(consumed, active_invocations);
    consumed = add_saturated(consumed, pending_finalizers);
    return add_saturated(consumed, unacknowledged_terminals);
}

std::size_t CoordinatorItemCapacitySnapshot::AvailableCredits() const noexcept {
    const auto consumed = ConsumedCredits();
    return consumed >= total_credits ? 0 : total_credits - consumed;
}

JobMaterializationService::JobMaterializationService(
    savor::db::IExecutionDb* execution_db,
    const savor::db::execution::programdb::ProgramKindRegistry* program_kind_registry)
    : execution_db(std::move(execution_db))
    , program_kind_registry(program_kind_registry) {
}

void JobMaterializationService::ResetForStart() {
    std::lock_guard<std::mutex> lock(mutex_);
    claimed_jobs_q_.reset();
    claimed_jobs_.clear();
    materialized_jobs_.clear();
    claim_sequence_counter_ = 0;
    claim_batch_sequence_counter_ = 0;
    claim_token_prefix_ = MakeClaimTokenPrefix();
}

void JobMaterializationService::StopMaterializationLoop() {
    claimed_jobs_q_.close();
}

void JobMaterializationService::SetEventCallback(EventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_callback_ = std::move(callback);
}

ClaimJobsResult JobMaterializationService::ClaimJobsDetailed(
    std::size_t max_claims,
    std::chrono::steady_clock::time_point now) {
    ClaimJobsResult result{};
    result.requested = max_claims;
    if (execution_db == nullptr || max_claims == 0) {
        return result;
    }
    result.attempted = true;
    std::vector<ClaimedJobSeed> claims;

    std::string error;
    std::string claim_token_prefix;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (claim_token_prefix_.empty()) {
            claim_token_prefix_ = MakeClaimTokenPrefix();
        }
        claim_token_prefix = claim_token_prefix_
            + ":claim:"
            + std::to_string(++claim_batch_sequence_counter_);
    }
    const auto claimed_jobs = execution_db->ClaimBatchReadyExecutionJobs(
        claim_token_prefix,
        static_cast<int>(max_claims),
        30000,
        &error);
    if (!error.empty()) {
        result.error = true;
        result.error_message = error;
    }
    claims.reserve(claimed_jobs.size());
    for (const auto& claimed : claimed_jobs) {
        claims.push_back(ClaimedJobSeed{
            .step = WorkflowReadyStep{
                .workflow_instance_id = claimed.workflow_instance_id,
                .workflow_step_id = claimed.workflow_step_id,
                .step_key = claimed.workflow_step_key,
                .step_kind = claimed.workflow_step_kind,
                .priority = claimed.workflow_step_priority,
            },
            .job_set_id = claimed.job_set_id,
            .job_id = claimed.job_id,
            .affinity = ClaimedJobAffinity{
                .savestate_affinity_key = claimed.savestate_affinity_key,
                .program_runtime_affinity_key = claimed.program_runtime_affinity_key,
            },
            });
    }

    std::vector<std::string> event_lines;
    EventCallback callback;
    std::size_t claimed = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = event_callback_;
        for (std::size_t index = 0; index < claims.size(); ++index) {
            const auto& seed = claims[index];
            const auto& claimed_job = claimed_jobs[index];
            const auto job_id = seed.job_id;
            if (job_id <= 0) {
                continue;
            }
            const auto key = std::to_string(job_id);
            auto [it, inserted] = claimed_jobs_.try_emplace(key);
            if (!inserted) {
                continue;
            }

            auto& record = it->second;
            record.step = seed.step;
            record.job_set_id = seed.job_set_id;
            record.job_id = seed.job_id;
            record.affinity = seed.affinity;
            record.claimed_by_token = claimed_job.claimed_by_token.empty()
                ? claim_token_prefix + ":"
                    + std::to_string(seed.job_id)
                : claimed_job.claimed_by_token;
            record.lease_expires_at_utc =
                claimed_job.lease_expires_at_utc;
            record.durable_attempt_id =
                claimed_job.durable_attempt_id;
            record.durable_priority = claimed_job.priority;
            record.queued_at_utc = claimed_job.queued_at_utc;
            record.claim_sequence = ++claim_sequence_counter_;
            record.claimed_at = now;
            record.state = ClaimedJobLifecycleState::Claimed;
            if (!claimed_jobs_q_.push(record)) {
                claimed_jobs_.erase(it);
                continue;
            }
            ++claimed;

            std::ostringstream line;
            line << "[seedprobe-claim] job=" << claimed_job.job_id
                 << " job_set=" << claimed_job.job_set_id
                 << " step=" << claimed_job.workflow_step_key
                 << " kind=" << claimed_job.workflow_step_kind
                 << " workflow_step_id=" << claimed_job.workflow_step_id
                 << " claim_sequence=" << record.claim_sequence
                 << " previous_claimed_by=";
            if (claimed_job.previous_claimed_by_token.has_value() && !claimed_job.previous_claimed_by_token->empty()) {
                line << *claimed_job.previous_claimed_by_token;
            } else {
                line << "none";
            }
            line << " previous_lease_expires_at=";
            if (claimed_job.previous_lease_expires_at_utc.has_value()) {
                line << *claimed_job.previous_lease_expires_at_utc;
            } else {
                line << "none";
            }
            event_lines.push_back(line.str());
        }
    }
    result.claimed = claimed;
    if (callback) {
        for (const auto& line : event_lines) {
            callback(line);
        }
    }
    return result;
}

std::size_t JobMaterializationService::ClaimJobs(std::size_t max_claims, std::chrono::steady_clock::time_point now) {
    return ClaimJobsDetailed(max_claims, now).claimed;
}

bool JobMaterializationService::MaterializeClaimedJobPayload(std::chrono::steady_clock::time_point now) {
    bool materialized_any = false;
    ClaimedJobRecord queued_record;
    while (claimed_jobs_q_.try_pop(queued_record)) {
        materialized_any = MaterializeClaimedJobRecord(queued_record, now) || materialized_any;
    }
    return materialized_any;
}

bool JobMaterializationService::MaterializeClaimedJobRecord(
    const ClaimedJobRecord& queued_record,
    std::chrono::steady_clock::time_point now) {
    (void)now;
    ClaimedJobRecord materialized_record{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = claimed_jobs_.find(std::to_string(queued_record.job_id));
        if (it == claimed_jobs_.end()) {
            return false;
        }

        auto& record = it->second;
        if (record.state != ClaimedJobLifecycleState::Claimed) {
            return false;
        }

        record.state = ClaimedJobLifecycleState::Materializing;
        materialized_record = record;
    }

    if (execution_db == nullptr || program_kind_registry == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = claimed_jobs_.find(std::to_string(queued_record.job_id));
        if (it == claimed_jobs_.end()) {
            return false;
        }
        auto& record = it->second;
        record.state = ClaimedJobLifecycleState::MaterializationFailed;
        ++payload_materialization_failure_count_;
        return false;
    }

    const auto* descriptor = program_kind_registry->FindForStepKind(materialized_record.step.step_kind);
    if (descriptor == nullptr || descriptor->runtime_init == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = claimed_jobs_.find(std::to_string(queued_record.job_id));
        if (it == claimed_jobs_.end()) {
            return false;
        }
        auto& record = it->second;
        record.state = ClaimedJobLifecycleState::MaterializationFailed;
        ++payload_materialization_failure_count_;
        return false;
    }

    const auto init_request = descriptor->runtime_init->BuildRuntimeInit(materialized_record.job_id);
    materialized_record.program_kind = descriptor->program_kind;
    materialized_record.runtime_init = init_request;
    materialized_record.payload = descriptor->runtime_init->MaterializePsJob(materialized_record.job_id, init_request);
    materialized_record.affinity = ClaimedJobAffinity{
        .savestate_affinity_key = std::to_string(init_request.savestate_ref_id),
        .program_runtime_affinity_key = init_request.bootstrap_profile,
    };
    // A RuntimeInitRequest does not yet describe the complete composite
    // baseline (movie continuation and adapter-declared derived state are
    // intentionally phase-owned). Do not infer multi-item compatibility from
    // a savestate ID alone. Phase adapters opt into a shared canonical key as
    // they migrate; until then the safe workset is a singleton.
    materialized_record.workset_execution_key =
        init_request.workset_execution_key.has_value()
            && !init_request.workset_execution_key->empty()
        ? *init_request.workset_execution_key
        : "singleton-job:" + std::to_string(materialized_record.job_id);
    if (!materialized_record.payload.has_value()) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = claimed_jobs_.find(std::to_string(queued_record.job_id));
        if (it == claimed_jobs_.end()) {
            return false;
        }
        auto& record = it->second;
        record.state = ClaimedJobLifecycleState::MaterializationFailed;
        ++payload_materialization_failure_count_;
        return false;
    }
    materialized_record.encoded_input_bytes =
        EstimateEncodedInputBytes(materialized_record);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = claimed_jobs_.find(std::to_string(queued_record.job_id));
        if (it == claimed_jobs_.end()) {
            return false;
        }
        auto& record = it->second;
        if (record.state != ClaimedJobLifecycleState::Materializing) {
            return false;
        }
        record = materialized_record;
        record.state = ClaimedJobLifecycleState::Materialized;
        materialized_jobs_[std::to_string(record.job_id)] = record;
    }
    return true;
}

void JobMaterializationService::MaterializeClaimedJobPayloadLoop(const std::atomic<bool>& stop_requested) {
    ClaimedJobRecord job;
    while (!stop_requested.load() && claimed_jobs_q_.pop_wait(job)) {
        (void)MaterializeClaimedJobRecord(job, std::chrono::steady_clock::now());
    }
}

bool JobMaterializationService::TrySelectMaterializedJobForWorker(
    const MaterializedJobSelectionAffinity& worker_affinity,
    ClaimedJobRecord* job_out) {
    if (job_out == nullptr) {
        return false;
    }
    std::vector<ClaimedJobRecord> jobs;
    if (!TrySelectMaterializedWorksetForWorker(
            worker_affinity,
            MaterializedWorksetSelectionLimits{
                .max_items = 1,
                .lookahead_items = 64,
                .max_selected_bytes =
                    std::numeric_limits<std::size_t>::max(),
                .lookahead_bytes =
                    std::numeric_limits<std::size_t>::max(),
            },
            &jobs)
        || jobs.empty()) {
        return false;
    }
    *job_out = std::move(jobs.front());
    return true;
}

bool JobMaterializationService::TrySelectMaterializedWorksetForWorker(
    const MaterializedJobSelectionAffinity& worker_affinity,
    MaterializedWorksetSelectionLimits limits,
    std::vector<ClaimedJobRecord>* jobs_out) {
    (void)worker_affinity;
    if (jobs_out == nullptr || limits.max_items == 0
        || limits.lookahead_items == 0
        || limits.max_selected_bytes == 0
        || limits.lookahead_bytes == 0) {
        return false;
    }
    jobs_out->clear();
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ClaimedJobRecord*> candidates;
    candidates.reserve(materialized_jobs_.size());
    for (auto it = materialized_jobs_.begin(); it != materialized_jobs_.end();) {
        const auto claimed_it = claimed_jobs_.find(it->first);
        if (claimed_it == claimed_jobs_.end()
            || claimed_it->second.state != ClaimedJobLifecycleState::Materialized) {
            it = materialized_jobs_.erase(it);
            continue;
        }
        candidates.push_back(&claimed_it->second);
        ++it;
    }
    if (candidates.empty()) {
        return false;
    }
    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const auto* lhs, const auto* rhs) {
            return DurableMaterializedOrder(*lhs, *rhs);
        });

    const auto& anchor = *candidates.front();
    const auto anchor_key = anchor.workset_execution_key;
    const auto anchor_priority = anchor.durable_priority;
    const auto lookahead = std::min(limits.lookahead_items, candidates.size());
    jobs_out->reserve(std::min(limits.max_items, lookahead));
    std::size_t inspected_bytes = 0;
    std::size_t selected_bytes = 0;
    for (std::size_t i = 0;
         i < lookahead && jobs_out->size() < limits.max_items;
         ++i) {
        auto& candidate = *candidates[i];
        if (candidate.durable_priority != anchor_priority) {
            break;
        }
        const auto candidate_bytes =
            std::max<std::size_t>(1, candidate.encoded_input_bytes);
        if (inspected_bytes >= limits.lookahead_bytes
            || candidate_bytes
                > limits.lookahead_bytes - inspected_bytes) {
            break;
        }
        inspected_bytes += candidate_bytes;
        if (candidate.workset_execution_key != anchor_key) {
            continue;
        }
        if (selected_bytes >= limits.max_selected_bytes
            || candidate_bytes
                > limits.max_selected_bytes - selected_bytes) {
            break;
        }
        selected_bytes += candidate_bytes;
        candidate.state = ClaimedJobLifecycleState::Dispatching;
        jobs_out->push_back(candidate);
        materialized_jobs_.erase(std::to_string(candidate.job_id));
    }
    return !jobs_out->empty();
}

bool JobMaterializationService::PeekMaterializedAnchor(
    ClaimedJobRecord* job_out) const {
    if (job_out == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const ClaimedJobRecord* anchor = nullptr;
    for (const auto& [_, materialized] : materialized_jobs_) {
        const auto claimed_it = claimed_jobs_.find(
            std::to_string(materialized.job_id));
        if (claimed_it == claimed_jobs_.end()
            || claimed_it->second.state
                != ClaimedJobLifecycleState::Materialized) {
            continue;
        }
        if (anchor == nullptr
            || DurableMaterializedOrder(
                claimed_it->second,
                *anchor)) {
            anchor = &claimed_it->second;
        }
    }
    if (anchor == nullptr) {
        return false;
    }
    *job_out = *anchor;
    return true;
}

bool JobMaterializationService::MaterializeJobForDebugReplay(
    std::int64_t job_id,
    ClaimedJobRecord* job_out,
    std::string* error_out) const {
    if (job_out == nullptr) {
        if (error_out) *error_out = "job_out is null";
        return false;
    }
    *job_out = ClaimedJobRecord{};
    if (execution_db == nullptr) {
        if (error_out) *error_out = "execution db is unavailable";
        return false;
    }
    if (program_kind_registry == nullptr) {
        if (error_out) *error_out = "program registry is unavailable";
        return false;
    }

    const auto job = execution_db->GetJob(job_id);
    if (!job.has_value()) {
        if (error_out) *error_out = "job not found";
        return false;
    }

    const std::int32_t program_kind = job->program_kind;
    if (program_kind <= 0) {
        if (error_out) *error_out = "job program_kind is not usable";
        return false;
    }

    const auto* descriptor = program_kind_registry->Find(program_kind);
    if (descriptor == nullptr || descriptor->runtime_init == nullptr) {
        if (error_out) *error_out = "program descriptor does not support runtime initialization";
        return false;
    }

    auto runtime_init = descriptor->runtime_init->BuildRuntimeInit(job_id);
    auto payload = descriptor->runtime_init->MaterializePsJob(job_id, runtime_init);
    if (!payload.has_value()) {
        if (error_out) *error_out = "job payload materialization failed";
        return false;
    }

    ClaimedJobRecord record{};
    record.job_id = job_id;
    record.job_set_id = job->job_set_id;
    record.program_kind = program_kind;
    record.runtime_init = std::move(runtime_init);
    record.affinity = ClaimedJobAffinity{
        .savestate_affinity_key = std::to_string(record.runtime_init.savestate_ref_id),
        .program_runtime_affinity_key = record.runtime_init.bootstrap_profile,
    };
    record.claimed_at = std::chrono::steady_clock::now();
    record.state = ClaimedJobLifecycleState::Materialized;
    record.payload = std::move(payload);
    *job_out = std::move(record);
    if (error_out) error_out->clear();
    return true;
}

bool JobMaterializationService::RequeueMaterializedJob(std::int64_t job_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = claimed_jobs_.find(std::to_string(job_id));
    if (it == claimed_jobs_.end()) {
        return false;
    }
    if (it->second.state != ClaimedJobLifecycleState::Materialized
        && it->second.state != ClaimedJobLifecycleState::Dispatching) {
        return false;
    }
    it->second.state = ClaimedJobLifecycleState::Materialized;
    materialized_jobs_[std::to_string(job_id)] = it->second;
    return true;
}

ClaimLeaseMaintenanceResult JobMaterializationService::RenewActiveClaimLeases(std::chrono::milliseconds lease_duration) {
    ClaimLeaseMaintenanceResult result{};
    if (execution_db == nullptr || lease_duration.count() <= 0) {
        return result;
    }

    std::vector<savor::db::ExecutionJobLeaseRequest> requests;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        requests.reserve(claimed_jobs_.size());
        for (const auto& [_, record] : claimed_jobs_) {
            if (IsMaterializerActiveClaimState(record.state)) {
                requests.push_back(savor::db::ExecutionJobLeaseRequest{
                    .job_id = record.job_id,
                    .claimed_by_token = record.claimed_by_token,
                });
            }
        }
    }

    result.attempted = requests.size();
    std::string error;
    const auto receipts = execution_db->RenewExecutionJobLeases(
        requests,
        lease_duration.count(),
        &error);
    std::unordered_map<std::int64_t, std::string> token_by_job;
    token_by_job.reserve(requests.size());
    for (const auto& request : requests) {
        token_by_job.emplace(
            request.job_id,
            request.claimed_by_token);
    }
    std::unordered_set<std::int64_t> seen_receipts;
    seen_receipts.reserve(receipts.size());
    for (const auto& receipt : receipts) {
        const auto request = token_by_job.find(receipt.job_id);
        if (request == token_by_job.end()
            || !seen_receipts.emplace(receipt.job_id).second) {
            ++result.failed;
            continue;
        }
        if (receipt.disposition
            == savor::db::ExecutionJobLeaseRenewalDisposition::Renewed) {
            ++result.renewed;
        } else {
            ++result.failed;
            result.lost_authority.push_back(
                ClaimLeaseMaintenanceResult::LostAuthority{
                    .job_id = receipt.job_id,
                    .claimed_by_token = request->second,
                    .disposition = receipt.disposition,
                });
        }
    }
    for (const auto& request : requests) {
        if (!seen_receipts.contains(request.job_id)) {
            ++result.failed;
            result.lost_authority.push_back(
                ClaimLeaseMaintenanceResult::LostAuthority{
                    .job_id = request.job_id,
                    .claimed_by_token =
                        request.claimed_by_token,
                    .disposition =
                        savor::db::
                            ExecutionJobLeaseRenewalDisposition::
                                BackendError,
                });
        }
    }
    return result;
}

std::vector<ClaimedJobRecord> JobMaterializationService::ListByState(ClaimedJobLifecycleState state) const {
    std::vector<ClaimedJobRecord> records;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, record] : claimed_jobs_) {
        if (record.state == state) {
            records.push_back(record);
        }
    }
    return records;
}

bool JobMaterializationService::MarkDispatched(std::int64_t job_id, std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = claimed_jobs_.find(std::to_string(job_id));
    if (it == claimed_jobs_.end()) {
        return false;
    }
    if (it->second.state != ClaimedJobLifecycleState::Materialized
        && it->second.state != ClaimedJobLifecycleState::Dispatching) {
        return false;
    }
    materialized_jobs_.erase(std::to_string(job_id));
    it->second.state = ClaimedJobLifecycleState::Dispatched;
    it->second.dispatched_at = now;
    return true;
}

bool JobMaterializationService::CleanupDispatchedOrExpired(std::int64_t job_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = claimed_jobs_.find(std::to_string(job_id));
    if (it == claimed_jobs_.end()) {
        return false;
    }
    if (it->second.state != ClaimedJobLifecycleState::Dispatching
        && it->second.state != ClaimedJobLifecycleState::Dispatched
        && it->second.state != ClaimedJobLifecycleState::Expired) {
        return false;
    }
    materialized_jobs_.erase(std::to_string(job_id));
    claimed_jobs_.erase(it);
    return true;
}

bool JobMaterializationService::AbandonClaim(std::int64_t job_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = claimed_jobs_.find(std::to_string(job_id));
    if (it == claimed_jobs_.end()) {
        return false;
    }
    materialized_jobs_.erase(std::to_string(job_id));
    claimed_jobs_.erase(it);
    return true;
}

std::size_t JobMaterializationService::ExpireClaimsOlderThan(std::chrono::milliseconds max_age, std::chrono::steady_clock::time_point now) {
    std::size_t expired = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, record] : claimed_jobs_) {
        if (record.state == ClaimedJobLifecycleState::Dispatched || record.state == ClaimedJobLifecycleState::Expired) {
            continue;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - record.claimed_at) > max_age) {
            record.state = ClaimedJobLifecycleState::Expired;
            materialized_jobs_.erase(std::to_string(record.job_id));
            ++expired;
        }
    }
    return expired;
}

std::size_t JobMaterializationService::CountBufferedJobs() const {
    std::size_t count = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, record] : claimed_jobs_) {
        if (record.state == ClaimedJobLifecycleState::Claimed
            || record.state == ClaimedJobLifecycleState::Materializing
            || record.state == ClaimedJobLifecycleState::Materialized
            || record.state == ClaimedJobLifecycleState::MaterializationFailed) {
            ++count;
        }
    }
    return count;
}

std::size_t JobMaterializationService::CountMaterializedJobs() const {
    std::size_t count = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, record] : materialized_jobs_) {
        if (record.state == ClaimedJobLifecycleState::Materialized) {
            ++count;
        }
    }
    return count;
}

namespace {
bool MatchesStringAffinity(const std::optional<std::string>& worker_value, const std::optional<std::string>& job_value) {
    return worker_value.has_value()
        && !worker_value->empty()
        && job_value.has_value()
        && *worker_value == *job_value;
}
}

bool JobMaterializationService::BetterMaterializedDispatchCandidate(
    const ClaimedJobRecord& lhs,
    const ClaimedJobRecord& rhs,
    const MaterializedJobSelectionAffinity& worker_affinity) {
    const bool lhs_savestate = MatchesStringAffinity(worker_affinity.savestate_affinity_key, lhs.affinity.savestate_affinity_key);
    const bool rhs_savestate = MatchesStringAffinity(worker_affinity.savestate_affinity_key, rhs.affinity.savestate_affinity_key);
    if (lhs_savestate != rhs_savestate) {
        return lhs_savestate;
    }

    const bool lhs_program = worker_affinity.program_kind.has_value()
        && lhs.program_kind == *worker_affinity.program_kind;
    const bool rhs_program = worker_affinity.program_kind.has_value()
        && rhs.program_kind == *worker_affinity.program_kind;
    if (lhs_program != rhs_program) {
        return lhs_program;
    }

    const bool lhs_runtime = MatchesStringAffinity(
        worker_affinity.program_runtime_affinity_key,
        lhs.affinity.program_runtime_affinity_key);
    const bool rhs_runtime = MatchesStringAffinity(
        worker_affinity.program_runtime_affinity_key,
        rhs.affinity.program_runtime_affinity_key);
    if (lhs_runtime != rhs_runtime) {
        return lhs_runtime;
    }

    return lhs.claim_sequence < rhs.claim_sequence;
}

std::string JobMaterializationService::CurrentClaimTokenPrefix() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return claim_token_prefix_;
}

bool JobMaterializationService::DurableMaterializedOrder(
    const ClaimedJobRecord& lhs,
    const ClaimedJobRecord& rhs) {
    if (lhs.durable_priority != rhs.durable_priority) {
        return lhs.durable_priority > rhs.durable_priority;
    }
    if (lhs.queued_at_utc != rhs.queued_at_utc) {
        return lhs.queued_at_utc < rhs.queued_at_utc;
    }
    return lhs.job_id < rhs.job_id;
}

} // namespace savor::runner::parallel::savordb
