#include "WorkflowMaterializationService.h"

#include <algorithm>

namespace simcore::runner::parallel::simcoredb {

WorkflowMaterializationService::WorkflowMaterializationService(
    WorkflowSchedulerAdapter* scheduler,
    ClaimJobsFn claim_jobs,
    BuildJobPayloadFn build_payload,
    ReadyStepPersistFn persist_materialization,
    MarkMaterializedFn mark_materialized,
    ResolveAffinityFn resolve_affinity)
    : scheduler_(scheduler)
    , claim_jobs_(std::move(claim_jobs))
    , build_payload_(std::move(build_payload))
    , persist_materialization_(std::move(persist_materialization))
    , mark_materialized_(std::move(mark_materialized))
    , resolve_affinity_(std::move(resolve_affinity)) {
}

std::optional<ScheduledJobSet> WorkflowMaterializationService::MaterializeWorkflowStep(const WorkflowReadyStep& step) {
    if (scheduler_ == nullptr) {
        return std::nullopt;
    }

    const auto scheduled = scheduler_->MaterializeReadyStep(step);
    if (mark_materialized_) {
        mark_materialized_(step, scheduled);
    }
    if (persist_materialization_) {
        persist_materialization_(step, scheduled);
    }
    return scheduled;
}

std::size_t WorkflowMaterializationService::ClaimJobs(std::chrono::steady_clock::time_point now) {
    const auto claimed_seeds = claim_jobs_ ? claim_jobs_() : std::vector<ClaimedJobSeed>{};
    std::size_t claimed = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& seed : claimed_seeds) {
        const auto job_id = seed.job_id;
        if (job_id <= 0) {
            continue;
        }
        const auto key = JobKey(job_id);
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
        ++claimed;
    }
    return claimed;
}

bool WorkflowMaterializationService::MaterializeClaimedJobPayload(std::chrono::steady_clock::time_point now) {
    std::string selected_key;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [key, record] : claimed_jobs_) {
            if (record.state != ClaimedJobLifecycleState::Claimed) {
                continue;
            }
            if (selected_key.empty() || BetterClaimPriority(record, claimed_jobs_.at(selected_key))) {
                selected_key = key;
            }
        }
    }

    if (selected_key.empty() || !build_payload_) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = claimed_jobs_.find(selected_key);
    if (it == claimed_jobs_.end() || it->second.state != ClaimedJobLifecycleState::Claimed) {
        return false;
    }

    auto& record = it->second;
    record.payload = build_payload_(record.job_id, record.step);
    record.state = ClaimedJobLifecycleState::PayloadMaterialized;
    if (record.payload.has_value()) {
        if (resolve_affinity_) {
            record.affinity = resolve_affinity_(record);
        }
        record.state = ClaimedJobLifecycleState::EligibleForDispatch;
    }

    (void)now;
    return true;
}

std::vector<ClaimedJobRecord> WorkflowMaterializationService::ListByState(ClaimedJobLifecycleState state) const {
    std::vector<ClaimedJobRecord> records;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, record] : claimed_jobs_) {
        if (record.state == state) {
            records.push_back(record);
        }
    }
    return records;
}

bool WorkflowMaterializationService::MarkDispatched(std::int64_t job_id, std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = claimed_jobs_.find(JobKey(job_id));
    if (it == claimed_jobs_.end()) {
        return false;
    }
    it->second.state = ClaimedJobLifecycleState::Dispatched;
    it->second.dispatched_at = now;
    return true;
}

bool WorkflowMaterializationService::CleanupDispatchedOrExpired(std::int64_t job_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = claimed_jobs_.find(JobKey(job_id));
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

std::size_t WorkflowMaterializationService::ExpireClaimsOlderThan(std::chrono::milliseconds max_age, std::chrono::steady_clock::time_point now) {
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

bool WorkflowMaterializationService::BetterClaimPriority(const ClaimedJobRecord& lhs, const ClaimedJobRecord& rhs) {
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

std::string WorkflowMaterializationService::JobKey(std::int64_t job_id) {
    return std::to_string(job_id);
}

} // namespace simcore::runner::parallel::simcoredb
