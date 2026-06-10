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

#include "Runner/Parallel/PRTypes.h"
#include "../Worker/TSQueue.h"
#include "WorkflowSchedulerAdapter.h"
#include "Execution/IExecutionDb.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"

namespace savor::runner::parallel::savordb {

enum class ClaimedJobLifecycleState {
    Claimed = 0,
    Materializing = 1,
    Materialized = 2,
    MaterializationFailed = 3,
    Dispatching = 4,
    Dispatched = 5,
    Expired = 6,
};

struct ClaimedJobAffinity {
    std::optional<std::string> savestate_affinity_key;
    std::optional<std::string> program_runtime_affinity_key;
};

struct ClaimedJobRecord {
    WorkflowReadyStep step{};
    std::int64_t job_set_id = 0;
    std::int64_t job_id = 0;
    std::int32_t program_kind = 0;
    savor::db::execution::programdb::RuntimeInitRequest runtime_init{};
    ClaimedJobAffinity affinity{};
    std::uint64_t claim_sequence = 0;
    std::chrono::steady_clock::time_point claimed_at{};
    std::optional<std::chrono::steady_clock::time_point> dispatched_at;
    ClaimedJobLifecycleState state = ClaimedJobLifecycleState::Claimed;
    std::optional<savor::PSJob> payload;
};

struct ClaimedJobSeed {
    WorkflowReadyStep step{};
    std::int64_t job_set_id = 0;
    std::int64_t job_id = 0;
    ClaimedJobAffinity affinity{};
};

struct MaterializedJobSelectionAffinity {
    std::optional<std::string> savestate_affinity_key;
    std::optional<std::int32_t> program_kind;
    std::optional<std::string> program_runtime_affinity_key;
};

class JobMaterializationService {
public:
    using BuildJobPayloadFn = std::function<std::optional<savor::PSJob>(std::int64_t job_id, const WorkflowReadyStep&)>;
    using ClaimJobsFn = std::function<std::vector<ClaimedJobSeed>(std::size_t max_claims)>;
    using ResolveAffinityFn = std::function<ClaimedJobAffinity(const ClaimedJobRecord&)>;
    using EventCallback = std::function<void(const std::string&)>;

    JobMaterializationService(
        savor::db::IExecutionDb* execution_db,
        const savor::db::execution::programdb::ProgramKindRegistry* program_kind_registry
    );

    void ResetForStart();
    void StopMaterializationLoop();
    void SetEventCallback(EventCallback callback);
    std::size_t ClaimJobs(std::size_t max_claims, std::chrono::steady_clock::time_point now);
    bool MaterializeClaimedJobPayload(std::chrono::steady_clock::time_point now);
    void MaterializeClaimedJobPayloadLoop(const std::atomic<bool>& stop_requested);
    bool TrySelectMaterializedJobForWorker(
        const MaterializedJobSelectionAffinity& worker_affinity,
        ClaimedJobRecord* job_out);
    bool MaterializeJobForDebugReplay(
        std::int64_t job_id,
        ClaimedJobRecord* job_out,
        std::string* error_out = nullptr) const;
    bool RequeueMaterializedJob(std::int64_t job_id);

    std::vector<ClaimedJobRecord> ListByState(ClaimedJobLifecycleState state) const;
    bool MarkDispatched(std::int64_t job_id, std::chrono::steady_clock::time_point now);
    bool CleanupDispatchedOrExpired(std::int64_t job_id);
    bool AbandonClaim(std::int64_t job_id);
    std::size_t ExpireClaimsOlderThan(std::chrono::milliseconds max_age, std::chrono::steady_clock::time_point now);
    std::size_t CountBufferedJobs() const;

private:
    static bool BetterMaterializedDispatchCandidate(
        const ClaimedJobRecord& lhs,
        const ClaimedJobRecord& rhs,
        const MaterializedJobSelectionAffinity& worker_affinity);
    bool MaterializeClaimedJobRecord(const ClaimedJobRecord& queued_record, std::chrono::steady_clock::time_point now);

    
    std::atomic<std::int64_t> payload_materialization_failure_count_{ 0 };    
    mutable std::mutex mutex_;
    TSQueue<ClaimedJobRecord> claimed_jobs_q_;
    std::unordered_map<std::string, ClaimedJobRecord> claimed_jobs_;
    std::unordered_map<std::string, ClaimedJobRecord> materialized_jobs_;
    std::uint64_t claim_sequence_counter_ = 0;
    savor::db::IExecutionDb* execution_db;
    const savor::db::execution::programdb::ProgramKindRegistry* program_kind_registry;
    EventCallback event_callback_;
};

} // namespace savor::runner::parallel::savordb
