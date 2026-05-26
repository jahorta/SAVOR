#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../PRTypes.h"
#include "../TSQueue.h"
#include "WorkflowSchedulerAdapter.h"
#include "../../../../SimCoreDB/Execution/IExecutionDb.h"
#include "../../../../SimCoreDB/Execution/ProgramDB/ProgramKindRegistry.h"

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

class JobMaterializationService {
public:
    using BuildJobPayloadFn = std::function<std::optional<simcore::PSJob>(std::int64_t job_id, const WorkflowReadyStep&)>;
    using ClaimJobsFn = std::function<std::vector<ClaimedJobSeed>(std::size_t max_claims)>;
    using ResolveAffinityFn = std::function<ClaimedJobAffinity(const ClaimedJobRecord&)>;

    JobMaterializationService(
        simcore::db::IExecutionDb* execution_db,
        const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry
    );

    std::size_t ClaimJobs(std::size_t max_claims, std::chrono::steady_clock::time_point now);
    bool MaterializeClaimedJobPayload(std::chrono::steady_clock::time_point now);
    void MaterializeClaimedJobPayloadLoop();

    std::vector<ClaimedJobRecord> ListByState(ClaimedJobLifecycleState state) const;
    bool MarkDispatched(std::int64_t job_id, std::chrono::steady_clock::time_point now);
    bool CleanupDispatchedOrExpired(std::int64_t job_id);
    bool AbandonClaim(std::int64_t job_id);
    std::size_t ExpireClaimsOlderThan(std::chrono::milliseconds max_age, std::chrono::steady_clock::time_point now);
    std::size_t CountBufferedJobs() const;

private:
    static bool BetterClaimPriority(const ClaimedJobRecord& lhs, const ClaimedJobRecord& rhs);

    
    std::atomic<std::int64_t> payload_materialization_failure_count_{ 0 };    
    mutable std::mutex mutex_;
    TSQueue<ClaimedJobRecord> claimed_jobs_q_;
    std::unordered_map<std::string, ClaimedJobRecord> claimed_jobs_;
    std::uint64_t claim_sequence_counter_ = 0;
    simcore::db::IExecutionDb* execution_db;
    const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry;
};

} // namespace simcore::runner::parallel::simcoredb
