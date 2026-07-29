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
    std::string workset_execution_key;
    // Deterministic host-envelope estimate used only to bound coordinator
    // lookahead before the phase adapter builds the canonical encoded
    // workset. The worker still validates the exact encoded byte count.
    std::size_t encoded_input_bytes = 0;
    std::string claimed_by_token;
    std::int64_t lease_expires_at_utc = 0;
    std::uint64_t durable_attempt_id = 0;
    int durable_priority = 0;
    std::int64_t queued_at_utc = 0;
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

struct MaterializedWorksetSelectionLimits {
    std::size_t max_items = 16;
    std::size_t lookahead_items = 64;
    std::size_t max_selected_bytes = 32ull * 1024ull * 1024ull;
    std::size_t lookahead_bytes = 64ull * 1024ull * 1024ull;
};

struct CoordinatorItemCapacitySnapshot {
    std::size_t total_credits = 0;
    std::size_t coordinator_buffered = 0;
    std::size_t worker_resident = 0;
    std::size_t active_invocations = 0;
    std::size_t pending_finalizers = 0;
    std::size_t unacknowledged_terminals = 0;

    [[nodiscard]] std::size_t ConsumedCredits() const noexcept;
    [[nodiscard]] std::size_t AvailableCredits() const noexcept;
};

struct ClaimJobsResult {
    bool attempted = false;
    std::size_t requested = 0;
    std::size_t claimed = 0;
    bool error = false;
    std::string error_message;
};

struct ClaimLeaseMaintenanceResult {
    struct LostAuthority {
        std::int64_t job_id = 0;
        std::string claimed_by_token;
        savor::db::ExecutionJobLeaseRenewalDisposition disposition =
            savor::db::ExecutionJobLeaseRenewalDisposition::BackendError;
    };
    std::size_t attempted = 0;
    std::size_t renewed = 0;
    std::size_t failed = 0;
    std::vector<LostAuthority> lost_authority;
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
    ClaimJobsResult ClaimJobsDetailed(std::size_t max_claims, std::chrono::steady_clock::time_point now);
    std::size_t ClaimJobs(std::size_t max_claims, std::chrono::steady_clock::time_point now);
    bool MaterializeClaimedJobPayload(std::chrono::steady_clock::time_point now);
    void MaterializeClaimedJobPayloadLoop(const std::atomic<bool>& stop_requested);
    bool TrySelectMaterializedJobForWorker(
        const MaterializedJobSelectionAffinity& worker_affinity,
        ClaimedJobRecord* job_out);
    bool TrySelectMaterializedWorksetForWorker(
        const MaterializedJobSelectionAffinity& worker_affinity,
        MaterializedWorksetSelectionLimits limits,
        std::vector<ClaimedJobRecord>* jobs_out);
    bool PeekMaterializedAnchor(
        ClaimedJobRecord* job_out) const;
    bool MaterializeJobForDebugReplay(
        std::int64_t job_id,
        ClaimedJobRecord* job_out,
        std::string* error_out = nullptr) const;
    bool RequeueMaterializedJob(std::int64_t job_id);
    ClaimLeaseMaintenanceResult RenewActiveClaimLeases(std::chrono::milliseconds lease_duration);

    std::vector<ClaimedJobRecord> ListByState(ClaimedJobLifecycleState state) const;
    bool MarkDispatched(std::int64_t job_id, std::chrono::steady_clock::time_point now);
    bool CleanupDispatchedOrExpired(std::int64_t job_id);
    bool AbandonClaim(std::int64_t job_id);
    std::size_t ExpireClaimsOlderThan(std::chrono::milliseconds max_age, std::chrono::steady_clock::time_point now);
    std::size_t CountBufferedJobs() const;
    std::size_t CountMaterializedJobs() const;
    [[nodiscard]] std::string CurrentClaimTokenPrefix() const;

private:
    static bool BetterMaterializedDispatchCandidate(
        const ClaimedJobRecord& lhs,
        const ClaimedJobRecord& rhs,
        const MaterializedJobSelectionAffinity& worker_affinity);
    static bool DurableMaterializedOrder(
        const ClaimedJobRecord& lhs,
        const ClaimedJobRecord& rhs);
    bool MaterializeClaimedJobRecord(const ClaimedJobRecord& queued_record, std::chrono::steady_clock::time_point now);

    
    std::atomic<std::int64_t> payload_materialization_failure_count_{ 0 };    
    mutable std::mutex mutex_;
    TSQueue<ClaimedJobRecord> claimed_jobs_q_;
    std::unordered_map<std::string, ClaimedJobRecord> claimed_jobs_;
    std::unordered_map<std::string, ClaimedJobRecord> materialized_jobs_;
    std::uint64_t claim_sequence_counter_ = 0;
    std::uint64_t claim_batch_sequence_counter_ = 0;
    std::string claim_token_prefix_;
    savor::db::IExecutionDb* execution_db;
    const savor::db::execution::programdb::ProgramKindRegistry* program_kind_registry;
    EventCallback event_callback_;
};

} // namespace savor::runner::parallel::savordb
