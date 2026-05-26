#include "JobMaterializationService.h"

#include <utility>

namespace simcore::runner::parallel::simcoredb {

JobMaterializationService::JobMaterializationService(
    simcore::db::IExecutionDb* execution_db,
    const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry)
    : execution_db(std::move(execution_db))
    , program_kind_registry(program_kind_registry) {
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

    std::size_t claimed = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& seed : claims) {
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
    }
    return claimed;
}

bool JobMaterializationService::MaterializeClaimedJobPayload(std::chrono::steady_clock::time_point now) {
    (void)now;

    std::lock_guard<std::mutex> lock(mutex_);
    bool materialized = false;

    for (auto& [_, record] : claimed_jobs_) {
        if (record.state != ClaimedJobLifecycleState::Claimed) {
            continue;
        }

        if (execution_db == nullptr || program_kind_registry == nullptr) {
            record.state = ClaimedJobLifecycleState::PayloadMaterialized;
            ++payload_materialization_failure_count_;
            continue;
        }

        const auto* descriptor = program_kind_registry->FindForStepKind(record.step.step_kind);
        if (descriptor == nullptr || descriptor->runtime_init == nullptr) {
            record.state = ClaimedJobLifecycleState::PayloadMaterialized;
            ++payload_materialization_failure_count_;
            continue;
        }

        const auto init_request = descriptor->runtime_init->BuildRuntimeInit(record.job_id);
        record.payload = descriptor->runtime_init->MaterializePsJob(record.job_id, init_request);
        record.affinity = ClaimedJobAffinity{
            .savestate_affinity_key = std::to_string(init_request.savestate_ref_id),
            .program_runtime_affinity_key = init_request.bootstrap_profile,
        };
        record.state = record.payload.has_value()
            ? ClaimedJobLifecycleState::EligibleForDispatch
            : ClaimedJobLifecycleState::PayloadMaterialized;
        if (!record.payload.has_value()) {
            ++payload_materialization_failure_count_;
        }
        materialized = true;
    }

    return materialized;
}

void JobMaterializationService::MaterializeClaimedJobPayloadLoop() {
    ClaimedJobRecord job;
    while (claimed_jobs_q_.pop_wait(job)) {
        (void)job;
        (void)MaterializeClaimedJobPayload(std::chrono::steady_clock::now());
    }
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
    if (it->second.state != ClaimedJobLifecycleState::Dispatched
        && it->second.state != ClaimedJobLifecycleState::Expired) {
        return false;
    }
    claimed_jobs_.erase(it);
    return true;
}

bool JobMaterializationService::AbandonClaim(std::int64_t job_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = claimed_jobs_.find(std::to_string(job_id));
    if (it == claimed_jobs_.end()) {
        return false;
    }
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
            || record.state == ClaimedJobLifecycleState::PayloadMaterialized
            || record.state == ClaimedJobLifecycleState::EligibleForDispatch) {
            ++count;
        }
    }
    return count;
}

bool JobMaterializationService::BetterClaimPriority(const ClaimedJobRecord& lhs, const ClaimedJobRecord& rhs) {
    const bool lhs_savestate = lhs.affinity.savestate_affinity_key.has_value() && !lhs.affinity.savestate_affinity_key->empty();
    const bool rhs_savestate = rhs.affinity.savestate_affinity_key.has_value() && !rhs.affinity.savestate_affinity_key->empty();
    if (lhs_savestate != rhs_savestate) {
        return lhs_savestate;
    }

    const bool lhs_runtime = lhs.affinity.program_runtime_affinity_key.has_value() && !lhs.affinity.program_runtime_affinity_key->empty();
    const bool rhs_runtime = rhs.affinity.program_runtime_affinity_key.has_value() && !rhs.affinity.program_runtime_affinity_key->empty();
    if (lhs_runtime != rhs_runtime) {
        return lhs_runtime;
    }

    return lhs.claim_sequence < rhs.claim_sequence;
}

} // namespace simcore::runner::parallel::simcoredb
