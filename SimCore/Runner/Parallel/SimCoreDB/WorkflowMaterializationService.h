#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../PRTypes.h"
#include "WorkflowSchedulerAdapter.h"

namespace simcore::runner::parallel::simcoredb {

enum class ClaimedJobLifecycleState {
    Claimed = 0,
    PayloadMaterialized = 1,
    EligibleForDispatch = 2,
    Dispatched = 3,
    Expired = 4,
};

struct ClaimedJobAffinity {
    std::optional<std::string> savestate_affinity_key;
    std::optional<std::string> program_runtime_affinity_key;
};

struct ClaimedJobRecord {
    WorkflowReadyStep step{};
    std::int64_t job_set_id = 0;
    std::int64_t job_id = 0;
    ClaimedJobAffinity affinity{};
    std::uint64_t claim_sequence = 0;
    std::chrono::steady_clock::time_point claimed_at{};
    std::optional<std::chrono::steady_clock::time_point> dispatched_at;
    ClaimedJobLifecycleState state = ClaimedJobLifecycleState::Claimed;
    std::optional<simcore::PSJob> payload;
};

struct ClaimedJobSeed {
    WorkflowReadyStep step{};
    std::int64_t job_set_id = 0;
    std::int64_t job_id = 0;
    ClaimedJobAffinity affinity{};
};

class WorkflowMaterializationService {
public:
    using BuildJobPayloadFn = std::function<std::optional<simcore::PSJob>(std::int64_t job_id, const WorkflowReadyStep&)>;
    using ClaimJobsFn = std::function<std::vector<ClaimedJobSeed>()>;
    using ReadyStepPersistFn = std::function<void(const WorkflowReadyStep&, const ScheduledJobSet&)>;
    using MarkMaterializedFn = std::function<void(const WorkflowReadyStep&, const ScheduledJobSet&)>;
    using ResolveAffinityFn = std::function<ClaimedJobAffinity(const ClaimedJobRecord&)>;

    WorkflowMaterializationService(
        WorkflowSchedulerAdapter* scheduler,
        ClaimJobsFn claim_jobs,
        BuildJobPayloadFn build_payload,
        ReadyStepPersistFn persist_materialization,
        MarkMaterializedFn mark_materialized,
        ResolveAffinityFn resolve_affinity = {});

    std::optional<ScheduledJobSet> MaterializeWorkflowStep(const WorkflowReadyStep& step);
    std::size_t ClaimJobs(std::chrono::steady_clock::time_point now);
    bool MaterializeClaimedJobPayload(std::chrono::steady_clock::time_point now);

    std::vector<ClaimedJobRecord> ListByState(ClaimedJobLifecycleState state) const;
    bool MarkDispatched(std::int64_t job_id, std::chrono::steady_clock::time_point now);
    bool CleanupDispatchedOrExpired(std::int64_t job_id);
    bool AbandonClaim(std::int64_t job_id);
    std::size_t ExpireClaimsOlderThan(std::chrono::milliseconds max_age, std::chrono::steady_clock::time_point now);

private:
    static bool BetterClaimPriority(const ClaimedJobRecord& lhs, const ClaimedJobRecord& rhs);
    static std::string JobKey(std::int64_t job_id);

    WorkflowSchedulerAdapter* scheduler_ = nullptr;
    ClaimJobsFn claim_jobs_;
    BuildJobPayloadFn build_payload_;
    ReadyStepPersistFn persist_materialization_;
    MarkMaterializedFn mark_materialized_;
    ResolveAffinityFn resolve_affinity_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ClaimedJobRecord> claimed_jobs_;
    std::uint64_t claim_sequence_counter_ = 0;
};

} // namespace simcore::runner::parallel::simcoredb
