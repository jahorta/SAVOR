#include "JobMaterializationService.h"

#include <charconv>
#include <sstream>
#include <utility>

namespace savor::runner::parallel::savordb {

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
}

void JobMaterializationService::StopMaterializationLoop() {
    claimed_jobs_q_.close();
}

void JobMaterializationService::SetEventCallback(EventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_callback_ = std::move(callback);
}

std::size_t JobMaterializationService::ClaimJobs(std::size_t max_claims, std::chrono::steady_clock::time_point now) {
    if (execution_db == nullptr || max_claims == 0) {
        return 0;
    }
    std::vector<ClaimedJobSeed> claims;

    std::string error;
    const auto claimed_jobs = execution_db->ClaimBatchReadyExecutionJobs(
        "workflow_job_materializer",
        static_cast<int>(max_claims),
        30000,
        &error);
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
            record.claim_sequence = ++claim_sequence_counter_;
            record.claimed_at = now;
            record.state = ClaimedJobLifecycleState::Claimed;
            claimed_jobs_q_.push(record);
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
    if (callback) {
        for (const auto& line : event_lines) {
            callback(line);
        }
    }
    return claimed;
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

    std::lock_guard<std::mutex> lock(mutex_);
    auto best_it = materialized_jobs_.end();
    for (auto it = materialized_jobs_.begin(); it != materialized_jobs_.end();) {
        const auto claimed_it = claimed_jobs_.find(it->first);
        if (claimed_it == claimed_jobs_.end()
            || claimed_it->second.state != ClaimedJobLifecycleState::Materialized) {
            it = materialized_jobs_.erase(it);
            continue;
        }
        if (execution_db != nullptr) {
            const auto job_row = execution_db->GetJob(claimed_it->second.job_id);
            if (job_row.has_value() && job_row->state != "QUEUED") {
                claimed_jobs_.erase(claimed_it);
                it = materialized_jobs_.erase(it);
                continue;
            }
        }

        if (best_it == materialized_jobs_.end()) {
            best_it = it;
        } else {
            const auto best_claimed_it = claimed_jobs_.find(best_it->first);
            if (best_claimed_it == claimed_jobs_.end()
                || BetterMaterializedDispatchCandidate(claimed_it->second, best_claimed_it->second, worker_affinity)) {
                best_it = it;
            }
        }
        ++it;
    }

    if (best_it == materialized_jobs_.end()) {
        return false;
    }

    const auto claimed_it = claimed_jobs_.find(best_it->first);
    if (claimed_it == claimed_jobs_.end()) {
        materialized_jobs_.erase(best_it);
        return false;
    }
    *job_out = claimed_it->second;
    claimed_it->second.state = ClaimedJobLifecycleState::Dispatching;
    materialized_jobs_.erase(best_it);
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

    std::int32_t program_kind = 0;
    const auto* first = job->program_kind.data();
    const auto* last = first + job->program_kind.size();
    const auto parse_result = std::from_chars(first, last, program_kind);
    if (parse_result.ec != std::errc{} || parse_result.ptr != last) {
        if (job->program_kind.size() == 1) {
            program_kind = static_cast<unsigned char>(job->program_kind.front());
        }
    }
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

} // namespace savor::runner::parallel::savordb
