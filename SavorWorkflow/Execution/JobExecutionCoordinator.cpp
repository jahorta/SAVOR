#include "JobExecutionCoordinator.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

#include "Runner/IPC/DurableWorkerTerminalEnvelope.h"
#include "Runner/Runtime/Worksets/WorksetTypes.h"

namespace savor::runner::parallel::savordb {
namespace {

std::atomic<std::uint64_t> g_coordinator_sequence{1};

std::int64_t NowMonoNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const char* TerminalStatusName(
    savor::wrms::InvocationTerminalStatus status) {
    switch (status) {
    case savor::wrms::InvocationTerminalStatus::Succeeded:
        return "SUCCEEDED";
    case savor::wrms::InvocationTerminalStatus::Failed:
        return "FAILED";
    case savor::wrms::InvocationTerminalStatus::Cancelled:
        return "CANCELLED";
    case savor::wrms::InvocationTerminalStatus::InfrastructureFailure:
        return "INFRASTRUCTURE_FAILURE";
    case savor::wrms::InvocationTerminalStatus::CleanupFailure:
        return "CLEANUP_FAILURE";
    case savor::wrms::InvocationTerminalStatus::TimedOut:
        return "TIMED_OUT";
    default:
        return "UNKNOWN";
    }
}

bool Applied(savor::db::ExecutionDbOperationDisposition disposition) {
    return disposition
            == savor::db::ExecutionDbOperationDisposition::Applied
        || disposition
            == savor::db::ExecutionDbOperationDisposition::AlreadyApplied;
}

const char* ExecutionDbDispositionName(
    savor::db::ExecutionDbOperationDisposition disposition) {
    using Disposition = savor::db::ExecutionDbOperationDisposition;
    switch (disposition) {
    case Disposition::Applied: return "APPLIED";
    case Disposition::AlreadyApplied: return "ALREADY_APPLIED";
    case Disposition::Missing: return "MISSING";
    case Disposition::WrongState: return "WRONG_STATE";
    case Disposition::TokenMismatch: return "TOKEN_MISMATCH";
    case Disposition::AttemptMismatch: return "ATTEMPT_MISMATCH";
    case Disposition::LeaseExpired: return "LEASE_EXPIRED";
    case Disposition::Conflict: return "CONFLICT";
    case Disposition::InvalidRequest: return "INVALID_REQUEST";
    case Disposition::BackendError: return "BACKEND_ERROR";
    default: return "UNKNOWN";
    }
}

std::chrono::milliseconds CappedExponentialDelay(
    std::chrono::milliseconds base,
    std::chrono::milliseconds maximum,
    std::uint32_t attempt) {
    auto delay = std::max(base, std::chrono::milliseconds(1));
    const auto cap = std::max(maximum, delay);
    for (std::uint32_t index = 1;
         index < attempt && delay < cap;
         ++index) {
        if (delay >= cap / 2) {
            return cap;
        }
        delay *= 2;
    }
    return std::min(delay, cap);
}

std::chrono::steady_clock::time_point LeaseRenewalDeadline(
    std::int64_t lease_expires_at_utc,
    std::chrono::milliseconds renewal_lead_time) {
    const auto now_steady = std::chrono::steady_clock::now();
    const auto now_utc = std::chrono::duration_cast<
        std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                             .count();
    const auto remaining = std::chrono::milliseconds(
        std::max<std::int64_t>(0, lease_expires_at_utc - now_utc));
    return remaining <= renewal_lead_time
        ? now_steady
        : now_steady + (remaining - renewal_lead_time);
}

std::string WorkerStreamKey(
    std::size_t worker_id,
    std::uint64_t generation) {
    return std::to_string(worker_id) + ":" + std::to_string(generation);
}

void StoreMaximum(
    std::atomic<std::uint64_t>& target,
    std::uint64_t value) {
    auto current = target.load(std::memory_order_relaxed);
    while (value > current
        && !target.compare_exchange_weak(
            current,
            value,
            std::memory_order_relaxed)) {
    }
}

} // namespace

class JobExecutionCoordinator::Impl {
public:
    using Clock = std::chrono::steady_clock;

    Impl(
        savor::db::IExecutionDb* execution_db,
        const savor::db::execution::programdb::ProgramKindRegistry*
            program_kind_registry,
        WorkerCoordinator* worker_coordinator,
        savor::db::execution::WorkerResultBlobStore* blob_store,
        JobExecutionCoordinatorConfig config)
        : execution_db_(execution_db)
        , program_kind_registry_(program_kind_registry)
        , worker_coordinator_(worker_coordinator)
        , blob_store_(blob_store)
        , config_(std::move(config)) {
        std::ostringstream token;
        token << "job-execution-coordinator-"
              << Clock::now().time_since_epoch().count()
              << '-' << reinterpret_cast<std::uintptr_t>(this)
              << '-' << g_coordinator_sequence.fetch_add(1);
        coordinator_token_ = token.str();
    }

    ~Impl() {
        Stop();
    }

    bool Start(std::string* error_out);
    void Quiesce();
    bool ReleaseBufferedClaims(std::string* error_out);
    bool RecoverAfterWorkersStopped(std::string* error_out);
    void Stop();
    void SetPaused(bool paused);
    bool IsPaused() const noexcept;
    bool IsRunning() const noexcept;
    bool ClearInvariantPause();
    void RegisterCancellationCommitPending(
        std::uint64_t hold_id,
        const std::vector<savor::db::ExecutionCancellationRequestSpec>&
            cancellations);
    void RegisterCommittedCancellations(
        std::uint64_t hold_id,
        const std::vector<savor::db::CommittedJobCancellation>&
            cancellations);
    void OpenCancellationAdmission();
    JobExecutionCoordinatorTelemetry SnapshotTelemetry() const;
    std::vector<JobExecutionWorkerLaneSnapshot>
        SnapshotWorkerLanes() const;
    std::vector<JobExecutionCoordinatorWarning>
        SnapshotWarnings() const;

private:
    enum class DispatchPhase : std::uint8_t {
        Claimed = 0,
        Reconstructing,
        Waiting,
        Submitting,
        Active,
        Draining,
        ReleasedDraining,
        ReleaseRequested,
        Released,
        Retired,
    };

    struct DeferredDispatchRelease {
        std::string reason_code;
        std::string reason_text;
        bool keep_draining = false;
    };

    struct PendingSubmissionCancellation {
        savor::db::CommittedJobCancellation cancellation;
        std::optional<savor::runtime::WorkerWorksetItemId> item_id;
        Clock::time_point enqueued_at{};
    };

    struct DispatchRecord;
    using DispatchPtr = std::shared_ptr<DispatchRecord>;

    struct PendingCancellationMutation {
        savor::db::JobCancellationOutcomeCommand mutation;
        std::string requested_resolution_code;
        bool delivery = false;
        DispatchPtr dispatch;
        bool initial_suppression = false;
        Clock::time_point enqueued_at{};
    };

    struct DispatchRecord {
        mutable std::mutex mutex;
        savor::db::ClaimedPublishedWorkset claimed;
        savor::runtime::WorkerWorksetDefinition definition;
        WorkerExecutionTarget target;
        DispatchPhase phase = DispatchPhase::Claimed;
        bool submitted = false;
        bool dispatch_marked = false;
        bool terminal_workset_state_observed = false;
        bool draining_transition_persisted = false;
        Clock::time_point draining_retry_at{};
        bool summary_observed = false;
        bool workset_cancellation_applied = false;
        std::optional<DeferredDispatchRelease> deferred_release;
        std::deque<PendingSubmissionCancellation>
            pending_submission_cancellations;
        std::unordered_map<std::uint64_t, std::size_t> item_index_by_id;
        std::unordered_set<std::int64_t> staged_jobs;
        std::unordered_set<std::int64_t> acknowledged_jobs;
        std::unordered_set<std::int64_t> initially_suppressed_jobs;
        std::unordered_set<std::int64_t> sidecar_persisted_jobs;
        bool sidecar_receipt_accounted = false;
        std::string cancellation_sidecar_sha256;
        std::optional<
            savor::runtime::InitialWorksetCancellationSidecarV1>
            frozen_submission_sidecar;
        std::vector<savor::db::CommittedJobCancellation>
            frozen_submission_cancellations;
        std::unordered_map<
            std::int64_t,
            savor::runtime::WorkerItemTerminalCorrelation>
            terminal_by_job;
    };

    struct ReconstructionItem {
        DispatchPtr dispatch;
        Clock::time_point enqueued_at{};
    };

    struct ItemStartedEvent {
        WorkerCoordinatorEventContext source;
        savor::wrms::WorksetItemStartedPayload payload;
    };
    struct PreparedTerminalPersistence {
        savor::db::execution::WorkerResultBlobReference blob;
        savor::db::StageWorkerTerminalCommand command;
        bool pinned = true;
    };
    struct TerminalEvent {
        savor::runtime::DurableWorkerTerminalEnvelope envelope;
        std::uint32_t retry_attempt = 0;
        std::shared_ptr<PreparedTerminalPersistence> prepared;
        Clock::time_point retry_at{};
    };
    struct SummaryEvent {
        WorkerCoordinatorEventContext source;
        savor::wrms::WorksetSummaryPayload payload;
    };
    using PersistencePayload =
        std::variant<ItemStartedEvent, TerminalEvent, SummaryEvent>;
    struct PersistenceEvent {
        std::size_t worker_id = 0;
        std::uint64_t generation = 0;
        Clock::time_point enqueued_at{};
        PersistencePayload payload;
    };
    struct PersistenceStream {
        std::deque<PersistenceEvent> events;
        bool active = false;
    };

    struct PreparedWorkerEventMutation {
        savor::db::WorkerExecutionEventMutation mutation;
        Clock::time_point enqueued_at{};
        std::mutex mutex;
        std::condition_variable cv;
        bool completed = false;
        bool succeeded = false;
        savor::db::WorkerExecutionEventMutationReceipt receipt;
        std::string error;
    };
    using PreparedWorkerEventMutationPtr =
        std::shared_ptr<PreparedWorkerEventMutation>;

    enum class WorkerControlKind : std::uint8_t {
        Acknowledge = 0,
        CancelItem,
        CancelWorkset,
    };
    struct WorkerControlCommand {
        WorkerControlKind kind = WorkerControlKind::Acknowledge;
        std::int64_t dispatch_attempt_id = 0;
        std::int64_t job_id = 0;
        savor::runtime::WorkerItemTerminalCorrelation terminal;
        std::optional<savor::runtime::WorkerWorksetItemId> item_id;
        std::string reason;
        std::optional<savor::db::CommittedJobCancellation> cancellation;
        Clock::time_point enqueued_at{};
        Clock::time_point retry_at{};
    };

    struct WorksetAffinity {
        std::string module_canonical_id;
        std::optional<std::string> execution_key;
        std::optional<std::string> baseline_key;
    };

    struct WorkerLane {
        mutable std::mutex mutex;
        std::condition_variable cv;
        std::thread thread;
        std::size_t worker_id = 0;
        std::uint64_t generation = 0;
        bool stop = false;
        std::size_t reservations = 0;
        std::size_t reconstructing = 0;
        std::deque<DispatchPtr> waiting;
        std::optional<std::int64_t> submitting;
        DispatchPtr retry_submission;
        std::optional<std::int64_t> active;
        std::deque<WorkerControlCommand> controls;
        bool has_submitted_workset = false;
        std::optional<std::string> actual_module_canonical_id;
        std::optional<std::string> actual_execution_key;
        std::optional<std::string> actual_baseline_key;
        std::map<std::int64_t, WorksetAffinity> projected_affinities;
    };
    using WorkerLanePtr = std::shared_ptr<WorkerLane>;

    struct LeaseHeartbeatEntry {
        std::int64_t dispatch_attempt_id = 0;
        std::string claim_token;
        std::int64_t lease_expires_at_utc = 0;
        Clock::time_point renew_at{};
    };

    enum class LifecycleRequestKind : std::uint8_t {
        ReleaseUnsubmitted = 0,
        RecoverAfterWorkersStopped,
    };
    struct LifecycleRequest {
        LifecycleRequestKind kind =
            LifecycleRequestKind::ReleaseUnsubmitted;
        bool pending = false;
        bool completed = false;
        std::string error;
    };

    void ConfigureWorkerCallbacks();
    void SchedulerLoop();
    void ReconstructionLoop();
    void PersistenceLoop();
    void WorkerEventBatchLoop();
    void CancellationMutationLoop();
    void AuthorityLoop();
    void LeaseHeartbeatLoop();
    void WorkerLaneLoop(const WorkerLanePtr& lane);

    WorkerLanePtr EnsureWorkerLane(
        const ReadyWorkerCompatibilitySnapshot& worker);
    WorkerLanePtr FindWorkerLane(
        std::size_t worker_id,
        std::uint64_t generation) const;
    void StopWorkerLanes();
    static WorksetAffinity AffinityOf(
        const savor::db::ExecutionWorksetCompatibility& compatibility);
    static int AffinityScore(
        const WorkerLane& lane,
        const savor::db::ExecutionWorksetCompatibility& compatibility);
    static void RefreshActualAffinity(
        WorkerLane& lane,
        const ReadyWorkerCompatibilitySnapshot& worker);
    static void RemoveProjectedAffinity(
        WorkerLane& lane,
        std::int64_t dispatch_attempt_id);
    void WakeScheduler(std::string reason, bool reset_backoff);
    bool Reconstruct(const DispatchPtr& dispatch);
    bool ValidateReconstruction(
        const savor::db::ClaimedPublishedWorkset& claimed,
        const savor::db::execution::programdb::
            WorksetReconstructionResult& reconstruction,
        std::string* error_out) const;

    void EnqueuePersistence(PersistenceEvent event);
    bool ProcessPersistenceEvent(PersistenceEvent& event);
    bool HandleItemStarted(const ItemStartedEvent& event);
    bool HandleTerminal(TerminalEvent& event);
    bool HandleWorksetSummary(const SummaryEvent& event);
    bool PersistPreparedWorkerEvent(
        savor::db::WorkerExecutionEventMutation mutation,
        savor::db::WorkerExecutionEventMutationReceipt* receipt_out,
        std::string* error_out);

    void EnqueueAuthority(std::function<void()> action);
    void EnqueueWorkerControl(
        const WorkerExecutionTarget& target,
        WorkerControlCommand command);
    void ExecuteWorkerControl(
        const WorkerLanePtr& lane,
        WorkerControlCommand command);
    void QueueCancellationMutation(PendingCancellationMutation mutation);
    bool ResolveClaimedCancellation(
        const savor::db::CommittedJobCancellation& cancellation,
        std::string resolution_code,
        bool finalize_job_canceled,
        std::optional<std::int64_t> dispatch_attempt_id = {},
        std::optional<std::string> claim_token = {});
    void CompleteCancellationDelivery(
        savor::db::CommittedJobCancellation cancellation,
        const WorkerCommandResult& result,
        bool whole_workset,
        Clock::time_point enqueued_at);
    void HandleWorkerUnavailable(WorkerUnavailableEvent event);
    void HandleWorksetState(
        const WorkerCoordinatorEventContext& source,
        const savor::wrms::WorksetStatePayload& payload);
    bool MarkDispatchDraining(const DispatchPtr& dispatch);
    bool ProcessPendingDrainingTransitions();
    void RefreshStorageReadiness();
    void ProcessLifecycleRequest();

    DispatchPtr FindDispatch(std::int64_t dispatch_attempt_id) const;
    void InsertDispatch(const DispatchPtr& dispatch);
    void RemoveDispatch(
        std::int64_t dispatch_attempt_id,
        const DispatchPtr& expected);
    bool ReleaseDispatch(
        const DispatchPtr& dispatch,
        std::string reason_code,
        std::string reason_text,
        bool keep_draining = false,
        bool* deferred_out = nullptr,
        bool retain_release_after_submission = true);
    void PauseForInvariant(
        const DispatchPtr& dispatch,
        std::string message,
        std::string detail,
        std::int64_t job_id = 0);
    void MaybeRetire(const DispatchPtr& dispatch);
    void ClearLaneIdentity(
        const WorkerExecutionTarget& target,
        std::int64_t dispatch_attempt_id);
    bool IsDispatchAdmissionPaused() const noexcept;
    static bool IsInvariantSubmissionRejection(
        const WorkerSubmitResult& result) noexcept;

    void RegisterLeaseHeartbeat(
        const DispatchPtr& dispatch,
        std::int64_t lease_expires_at_utc);
    void UnregisterLeaseHeartbeat(
        std::int64_t dispatch_attempt_id,
        std::string_view claim_token);
    void HandleLeaseHeartbeatResult(
        std::int64_t dispatch_attempt_id,
        std::string claim_token,
        bool call_succeeded,
        savor::db::WorksetDispatchLeaseReceipt receipt,
        std::string error);

    bool WaitForLifecycleRequest(
        LifecycleRequestKind kind,
        std::string* error_out);
    void RecordWarning(
        std::string message,
        std::string detail = {},
        std::int64_t worker_id = 0,
        std::int64_t job_id = 0);
    void RecordError(std::string error);
    void RecordCancellationResolution(std::string code);
    std::string NextToken(std::string_view purpose);

    savor::db::IExecutionDb* execution_db_ = nullptr;
    const savor::db::execution::programdb::ProgramKindRegistry*
        program_kind_registry_ = nullptr;
    WorkerCoordinator* worker_coordinator_ = nullptr;
    savor::db::execution::WorkerResultBlobStore* blob_store_ = nullptr;
    JobExecutionCoordinatorConfig config_{};
    std::string coordinator_token_;
    std::atomic<std::uint64_t> token_sequence_{1};

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> quiescing_{false};
    std::atomic<bool> user_paused_{false};
    std::atomic<bool> invariant_paused_{false};
    std::atomic<bool> storage_paused_{false};
    std::atomic<bool> cancellation_storage_paused_{false};
    std::atomic<bool> cancellation_admission_open_{false};
    std::atomic<bool> blob_store_ready_{false};

    std::thread scheduler_thread_;
    std::thread reconstruction_thread_;
    std::thread authority_thread_;
    std::thread lease_heartbeat_thread_;
    std::thread worker_event_batch_thread_;
    std::thread cancellation_mutation_thread_;
    std::vector<std::thread> persistence_threads_;

    mutable std::mutex scheduler_mutex_;
    std::condition_variable scheduler_cv_;
    struct ClaimBackoffState {
        std::uint32_t empty_attempts = 0;
        std::uint64_t delay_ms = 0;
        Clock::time_point next_attempt{};
    };
    ClaimBackoffState claim_backoff_;
    std::size_t last_assigned_worker_ =
        std::numeric_limits<std::size_t>::max();
    Clock::time_point next_full_claim_reconciliation_{};
    savor::db::ExecutionWorkAvailabilitySubscription
        execution_work_subscription_ = 0;
    savor::db::ExecutionWorkAvailabilitySnapshot
        execution_work_availability_{};
    std::uint64_t scheduler_wakeup_generation_ = 0;
    std::string last_scheduler_wake_reason_;

    mutable std::mutex registry_mutex_;
    std::unordered_map<std::int64_t, DispatchPtr> dispatches_;

    mutable std::mutex lanes_mutex_;
    std::unordered_map<std::size_t, WorkerLanePtr> lanes_;

    mutable std::mutex reconstruction_mutex_;
    std::condition_variable reconstruction_cv_;
    std::deque<ReconstructionItem> reconstruction_queue_;
    bool reconstruction_active_ = false;
    std::size_t reconstruction_high_water_ = 0;
    std::uint64_t reconstruction_total_duration_ms_ = 0;
    std::uint64_t reconstruction_max_duration_ms_ = 0;

    mutable std::mutex persistence_mutex_;
    std::condition_variable persistence_cv_;
    std::map<std::string, PersistenceStream> persistence_streams_;
    std::size_t persistence_high_water_ = 0;

    mutable std::mutex worker_event_batch_mutex_;
    std::condition_variable worker_event_batch_cv_;
    std::deque<PreparedWorkerEventMutationPtr>
        worker_event_batch_queue_;
    std::size_t worker_event_batch_high_water_ = 0;

    mutable std::mutex authority_mutex_;
    std::condition_variable authority_cv_;
    std::deque<std::function<void()>> authority_actions_;
    struct CancellationIndexEntry {
        savor::db::CommittedJobCancellation cancellation;
    };
    mutable std::mutex cancellation_index_mutex_;
    std::unordered_map<std::int64_t, CancellationIndexEntry>
        committed_cancellations_by_job_;
    std::vector<savor::db::CommittedJobCancellation>
        startup_unresolved_cancellations_;
    std::unordered_map<std::uint64_t, std::vector<std::int64_t>>
        pending_cancellation_holds_;
    std::unordered_map<std::int64_t, std::size_t>
        pending_cancellation_hold_counts_;
    mutable std::mutex cancellation_mutation_mutex_;
    std::condition_variable cancellation_mutation_cv_;
    std::vector<PendingCancellationMutation> cancellation_mutations_;
    std::size_t cancellation_mutation_high_water_ = 0;
    std::atomic<bool> cancellation_mutation_stop_{false};
    Clock::time_point next_blob_readiness_retry_at_{};
    std::uint32_t blob_readiness_retry_attempt_ = 0;

    mutable std::mutex lease_heartbeat_mutex_;
    std::condition_variable lease_heartbeat_cv_;
    std::unordered_map<std::int64_t, LeaseHeartbeatEntry>
        lease_heartbeat_entries_;

    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_cv_;
    LifecycleRequest lifecycle_request_;

    std::atomic<std::uint64_t> claim_batches_{0};
    std::atomic<std::uint64_t> successful_claim_batches_{0};
    std::atomic<std::uint64_t> empty_claim_batches_{0};
    std::atomic<std::uint64_t> worksets_claimed_{0};
    std::atomic<std::uint64_t> worksets_reconstructed_{0};
    std::atomic<std::uint64_t> worksets_submitted_{0};
    std::atomic<std::uint64_t> submission_calls_started_{0};
    std::atomic<std::uint64_t> submission_accepted_{0};
    std::atomic<std::uint64_t> submission_temporary_unavailable_{0};
    std::atomic<std::uint64_t> submission_stale_generation_{0};
    std::atomic<std::uint64_t> submission_incompatible_{0};
    std::atomic<std::uint64_t> submission_deterministic_rejection_{0};
    std::atomic<std::uint64_t> submission_ambiguous_after_write_{0};
    std::atomic<std::uint64_t>
        submission_transport_canceled_before_write_{0};
    std::atomic<std::uint64_t> reconstruction_invariant_failures_{0};
    std::atomic<std::uint64_t> jobs_started_{0};
    std::atomic<std::uint64_t> worker_terminals_observed_{0};
    std::atomic<std::uint64_t> worker_terminals_staged_{0};
    std::atomic<std::uint64_t>
        worker_terminals_discarded_after_authority_release_{0};
    std::atomic<std::uint64_t> worker_terminal_acks_{0};
    std::atomic<std::uint64_t>
        worker_terminal_ack_abandoned_generation_loss_{0};
    std::atomic<std::uint64_t> worker_terminal_staging_failures_{0};
    std::atomic<std::uint64_t> worker_terminal_retry_attempts_{0};
    std::atomic<std::uint64_t> worker_event_batches_{0};
    std::atomic<std::uint64_t> worker_event_batch_items_{0};
    std::atomic<std::uint64_t> worker_event_batch_rollbacks_{0};
    std::atomic<std::uint64_t> worker_event_batch_retries_{0};
    std::atomic<std::uint64_t> worker_event_full_flushes_{0};
    std::atomic<std::uint64_t> worker_event_deadline_flushes_{0};
    std::atomic<std::uint64_t> worker_event_barrier_flushes_{0};
    std::atomic<std::uint64_t> worker_event_batch_max_size_{0};
    std::atomic<std::uint64_t> worker_event_batch_total_size_{0};
    std::atomic<std::uint64_t>
        worker_event_batch_max_collection_age_ms_{0};
    std::atomic<std::uint64_t> blob_readiness_failures_{0};
    std::atomic<std::uint64_t> cancellations_delivered_{0};
    std::atomic<std::uint64_t>
        cancellation_precommit_holds_registered_{0};
    std::atomic<std::uint64_t>
        cancellation_precommit_holds_promoted_{0};
    std::atomic<std::uint64_t> committed_cancellations_indexed_{0};
    std::atomic<std::uint64_t> waiting_jobs_suppressed_by_sidecar_{0};
    std::atomic<std::uint64_t> fully_canceled_worksets_avoided_{0};
    std::atomic<std::uint64_t> sidecar_items_submitted_{0};
    std::atomic<std::uint64_t> sidecar_submit_receipts_accepted_{0};
    std::atomic<std::uint64_t> sidecar_submit_receipts_repeated_{0};
    std::atomic<std::uint64_t> sidecar_submit_receipts_mismatched_{0};
    std::atomic<std::uint64_t> post_fence_cancellation_commands_{0};
    std::atomic<std::uint64_t> cancellation_mutation_batches_{0};
    std::atomic<std::uint64_t> cancellation_mutation_batch_items_{0};
    std::atomic<std::uint64_t> cancellation_mutation_full_flushes_{0};
    std::atomic<std::uint64_t> cancellation_mutation_deadline_flushes_{0};
    std::atomic<std::uint64_t> cancellation_mutation_barrier_flushes_{0};
    std::atomic<std::uint64_t> cancellation_mutation_rollbacks_{0};
    std::atomic<std::uint64_t> cancellation_mutation_retries_{0};
    std::atomic<std::uint64_t> cancellation_mutation_max_size_{0};
    std::atomic<std::uint64_t>
        cancellation_mutation_max_collection_age_ms_{0};
    std::atomic<std::size_t> pending_cancellation_mutations_{0};
    std::atomic<std::uint64_t> cancellations_already_applied_{0};
    std::atomic<std::uint64_t> worker_losses_{0};
    std::atomic<std::uint64_t> startup_recovered_dispatches_{0};
    std::atomic<std::uint64_t> startup_requeued_jobs_{0};
    std::atomic<std::uint64_t> startup_recovery_attempts_granted_{0};
    std::atomic<std::uint64_t> active_residence_probes_{0};
    std::atomic<std::uint64_t> active_residence_matches_{0};
    std::atomic<std::uint64_t> active_residence_failures_{0};
    std::atomic<std::uint64_t> active_lease_renewal_batches_{0};
    std::atomic<std::uint64_t> active_lease_renewal_retries_{0};
    std::atomic<std::uint64_t> draining_transitions_{0};
    std::atomic<std::uint64_t> scheduler_wakeups_{0};
    std::atomic<std::uint64_t> availability_signal_wakeups_{0};
    std::atomic<std::uint64_t> reconciliation_claims_{0};
    std::atomic<std::uint64_t> worker_control_commands_queued_{0};
    std::atomic<std::uint64_t> worker_control_commands_attempted_{0};
    std::atomic<std::uint64_t> worker_control_commands_applied_{0};
    std::atomic<std::uint64_t> worker_control_commands_failed_{0};
    std::atomic<std::uint64_t> worker_control_commands_abandoned_{0};
    std::atomic<std::uint64_t> worker_control_queue_high_water_{0};
    std::atomic<std::uint64_t> cancellation_delivery_total_latency_ms_{0};
    std::atomic<std::uint64_t> cancellation_delivery_max_latency_ms_{0};

    mutable std::mutex warning_mutex_;
    std::deque<JobExecutionCoordinatorWarning> warnings_;
    std::uint64_t warning_sequence_ = 0;
    mutable std::mutex error_mutex_;
    std::string last_error_;
    mutable std::mutex cancellation_resolution_mutex_;
    std::map<std::string, std::uint64_t>
        cancellation_resolution_counts_;
    mutable std::mutex dispatch_release_mutex_;
    std::map<std::string, std::uint64_t>
        dispatch_release_reason_counts_;
    std::map<std::string, std::uint64_t>
        dispatch_release_phase_counts_;
};

bool JobExecutionCoordinator::Impl::Start(std::string* error_out) {
    if (running_.load()) {
        return true;
    }
    if (execution_db_ == nullptr || program_kind_registry_ == nullptr
        || worker_coordinator_ == nullptr || blob_store_ == nullptr) {
        if (error_out) {
            *error_out =
                "job execution coordinator requires execution DB, program "
                "registry, worker coordinator, and result blob store";
        }
        return false;
    }
    if (!worker_coordinator_->IsStarted()) {
        if (error_out) {
            *error_out =
                "worker coordinator must start before job execution "
                "coordinator";
        }
        return false;
    }
    const auto enabled_program_kinds =
        worker_coordinator_->EnabledProgramKinds();
    const auto descriptor_program_kinds =
        program_kind_registry_->RegisteredProgramKinds();
    if (enabled_program_kinds != descriptor_program_kinds) {
        if (error_out) {
            *error_out =
                "enabled FullPhase set and execution descriptor registry do not agree";
        }
        return false;
    }
    for (const auto program_kind : enabled_program_kinds) {
        const auto* phase = savor::runtime::fullphase::
            ProductionRegistry().Find(program_kind);
        const auto* descriptor =
            program_kind_registry_->Find(program_kind);
        if (phase == nullptr || descriptor == nullptr
            || !descriptor->full_phase_identity.has_value()
            || *descriptor->full_phase_identity != phase->identity()) {
            if (error_out) {
                *error_out =
                    "execution descriptor does not identify the enabled immutable FullPhase: "
                    + std::to_string(program_kind);
            }
            return false;
        }
    }
    const auto pool_limits =
        worker_coordinator_->RequiredWorksetLimits();
    if (pool_limits.maximum_items_per_workset
            < config_.maximum_items_per_workset
        || pool_limits.maximum_encoded_workset_bytes
            < config_.maximum_encoded_workset_bytes) {
        if (error_out) {
            *error_out =
                "job execution limits exceed the homogeneous worker pool contract";
        }
        return false;
    }
    if (config_.workset_lease_duration <= std::chrono::milliseconds::zero()
        || config_.workset_lease_renewal_point
            <= std::chrono::milliseconds::zero()
        || config_.workset_lease_renewal_point
            >= config_.workset_lease_duration
        || config_.workset_lease_retry_interval
            <= std::chrono::milliseconds::zero()
        || config_.terminal_retry_interval
            <= std::chrono::milliseconds::zero()
        || config_.terminal_retry_max_interval
            < config_.terminal_retry_interval
        || config_.blob_readiness_retry_interval
            <= std::chrono::milliseconds::zero()
        || config_.blob_readiness_retry_max_interval
            < config_.blob_readiness_retry_interval
        || config_.worker_queue_capacity == 0
        || config_.terminal_persistence_threads == 0
        || config_.worker_event_batch_size == 0
        || config_.worker_event_collection_delay
            < std::chrono::milliseconds::zero()
        || config_.cancellation_batch_size == 0
        || config_.cancellation_mutation_delay
            < std::chrono::milliseconds::zero()
        || config_.maximum_items_per_workset == 0
        || config_.maximum_encoded_workset_bytes == 0
        || !config_.state_compatibility.Complete()) {
        if (error_out) {
            *error_out =
                "job execution coordinator limits or state compatibility "
                "identity are invalid";
        }
        return false;
    }

    std::string readiness_error;
    if (!blob_store_->ValidateReady(&readiness_error)) {
        if (error_out) {
            *error_out = readiness_error.empty()
                ? "worker result blob store is not ready"
                : std::move(readiness_error);
        }
        return false;
    }

    {
        std::lock_guard lock(registry_mutex_);
        dispatches_.clear();
    }
    {
        std::lock_guard lock(reconstruction_mutex_);
        reconstruction_queue_.clear();
        reconstruction_active_ = false;
        reconstruction_high_water_ = 0;
        reconstruction_total_duration_ms_ = 0;
        reconstruction_max_duration_ms_ = 0;
    }
    {
        std::lock_guard lock(persistence_mutex_);
        persistence_streams_.clear();
        persistence_high_water_ = 0;
    }
    {
        std::lock_guard lock(worker_event_batch_mutex_);
        worker_event_batch_queue_.clear();
        worker_event_batch_high_water_ = 0;
    }
    {
        std::lock_guard lock(authority_mutex_);
        authority_actions_.clear();
    }
    {
        std::lock_guard lock(cancellation_index_mutex_);
        committed_cancellations_by_job_.clear();
        startup_unresolved_cancellations_.clear();
        pending_cancellation_holds_.clear();
        pending_cancellation_hold_counts_.clear();
    }
    {
        std::lock_guard lock(cancellation_mutation_mutex_);
        cancellation_mutations_.clear();
        cancellation_mutation_high_water_ = 0;
        cancellation_mutation_stop_ = false;
        pending_cancellation_mutations_.store(0);
    }
    {
        std::lock_guard lock(lease_heartbeat_mutex_);
        lease_heartbeat_entries_.clear();
    }
    {
        std::lock_guard lock(lifecycle_mutex_);
        lifecycle_request_ = {};
    }
    {
        std::lock_guard lock(cancellation_resolution_mutex_);
        cancellation_resolution_counts_.clear();
    }
    {
        std::lock_guard lock(dispatch_release_mutex_);
        dispatch_release_reason_counts_.clear();
        dispatch_release_phase_counts_.clear();
    }
    {
        std::lock_guard lock(scheduler_mutex_);
        claim_backoff_ = {};
        last_assigned_worker_ =
            std::numeric_limits<std::size_t>::max();
        next_full_claim_reconciliation_ =
            Clock::now() + std::chrono::seconds(30);
        scheduler_wakeup_generation_ = 0;
        last_scheduler_wake_reason_.clear();
    }

    savor::db::RecoverInterruptedWorksetDispatchesReceipt
        dispatch_recovery{};
    std::string dispatch_recovery_error;
    if (!execution_db_->RecoverInterruptedWorksetDispatches(
            &dispatch_recovery,
            &dispatch_recovery_error)) {
        if (error_out != nullptr) {
            *error_out = dispatch_recovery_error.empty()
                ? "failed reconciling interrupted workset dispatches"
                : std::move(dispatch_recovery_error);
        }
        return false;
    }
    startup_recovered_dispatches_.fetch_add(
        static_cast<std::uint64_t>(dispatch_recovery.dispatches_closed));
    startup_requeued_jobs_.fetch_add(
        static_cast<std::uint64_t>(dispatch_recovery.jobs_requeued));
    startup_recovery_attempts_granted_.fetch_add(
        static_cast<std::uint64_t>(
            dispatch_recovery.recovery_attempts_granted));

    std::string cancellation_recovery_error;
    const auto unresolved_cancellations =
        execution_db_->ListUnresolvedJobCancellations(
            &cancellation_recovery_error);
    if (!cancellation_recovery_error.empty()) {
        if (error_out != nullptr) {
            *error_out = std::move(cancellation_recovery_error);
        }
        return false;
    }
    {
        std::lock_guard lock(cancellation_index_mutex_);
        for (const auto& cancellation : unresolved_cancellations) {
            committed_cancellations_by_job_.insert_or_assign(
                cancellation.job_id,
                CancellationIndexEntry{cancellation});
        }
        startup_unresolved_cancellations_ = unresolved_cancellations;
    }

    std::string availability_error;
    const auto availability =
        execution_db_->GetExecutionWorkAvailability(&availability_error);
    if (!availability.has_value()) {
        if (error_out) {
            *error_out = availability_error.empty()
                ? "execution DB did not provide execution-work availability"
                : std::move(availability_error);
        }
        return false;
    }
    {
        std::lock_guard lock(scheduler_mutex_);
        execution_work_availability_ = *availability;
    }
    execution_work_subscription_ =
        execution_db_->SubscribeExecutionWorkAvailability(
            [this](
                const savor::db::ExecutionWorkAvailabilitySnapshot& snapshot) {
                bool generation_changed = false;
                bool ready_changed = false;
                {
                    std::lock_guard lock(scheduler_mutex_);
                    generation_changed = snapshot.generation
                        != execution_work_availability_.generation;
                    ready_changed = snapshot.has_ready_worksets
                        != execution_work_availability_.has_ready_worksets;
                    execution_work_availability_ = snapshot;
                }
                if (generation_changed) {
                    ++availability_signal_wakeups_;
                }
                if (ready_changed) {
                    WakeScheduler(
                        "ready-workset-generation",
                        snapshot.has_ready_worksets);
                }
            });

    stop_.store(false);
    quiescing_.store(false);
    invariant_paused_.store(false);
    storage_paused_.store(false);
    cancellation_storage_paused_.store(false);
    cancellation_admission_open_.store(false);
    blob_store_ready_.store(true);
    blob_readiness_retry_attempt_ = 0;
    next_blob_readiness_retry_at_ = {};
    ConfigureWorkerCallbacks();
    running_.store(true);

    persistence_threads_.reserve(config_.terminal_persistence_threads);
    for (std::size_t index = 0;
         index < config_.terminal_persistence_threads;
         ++index) {
        persistence_threads_.emplace_back(
            [this]() { PersistenceLoop(); });
    }
    worker_event_batch_thread_ =
        std::thread([this]() { WorkerEventBatchLoop(); });
    cancellation_mutation_thread_ =
        std::thread([this]() { CancellationMutationLoop(); });
    reconstruction_thread_ =
        std::thread([this]() { ReconstructionLoop(); });
    authority_thread_ = std::thread([this]() { AuthorityLoop(); });
    lease_heartbeat_thread_ =
        std::thread([this]() { LeaseHeartbeatLoop(); });
    scheduler_thread_ = std::thread([this]() { SchedulerLoop(); });
    if (error_out) error_out->clear();
    return true;
}

void JobExecutionCoordinator::Impl::Quiesce() {
    quiescing_.store(true);
    WakeScheduler("quiesce", false);
    {
        std::lock_guard lock(lanes_mutex_);
        for (const auto& [worker_id, lane] : lanes_) {
            (void)worker_id;
            lane->cv.notify_all();
        }
    }
}

bool JobExecutionCoordinator::Impl::WaitForLifecycleRequest(
    LifecycleRequestKind kind,
    std::string* error_out) {
    if (!running_.load()) {
        if (error_out) {
            *error_out = "job execution coordinator is not running";
        }
        return false;
    }
    {
        std::lock_guard lock(lifecycle_mutex_);
        lifecycle_request_.kind = kind;
        lifecycle_request_.pending = true;
        lifecycle_request_.completed = false;
        lifecycle_request_.error.clear();
    }
    authority_cv_.notify_all();
    std::unique_lock lock(lifecycle_mutex_);
    lifecycle_cv_.wait(lock, [this]() {
        return lifecycle_request_.completed || !running_.load();
    });
    if (error_out) {
        *error_out = lifecycle_request_.error;
    }
    return lifecycle_request_.completed
        && lifecycle_request_.error.empty();
}

bool JobExecutionCoordinator::Impl::ReleaseBufferedClaims(
    std::string* error_out) {
    Quiesce();
    return WaitForLifecycleRequest(
        LifecycleRequestKind::ReleaseUnsubmitted,
        error_out);
}

bool JobExecutionCoordinator::Impl::RecoverAfterWorkersStopped(
    std::string* error_out) {
    Quiesce();
    return WaitForLifecycleRequest(
        LifecycleRequestKind::RecoverAfterWorkersStopped,
        error_out);
}

void JobExecutionCoordinator::Impl::Stop() {
    if (!running_.load() && !scheduler_thread_.joinable()
        && !authority_thread_.joinable()) {
        return;
    }
    Quiesce();
    if (execution_work_subscription_ != 0) {
        execution_db_->UnsubscribeExecutionWorkAvailability(
            execution_work_subscription_);
        execution_work_subscription_ = 0;
    }
    if (running_.load() && authority_thread_.joinable()) {
        std::string ignored;
        (void)WaitForLifecycleRequest(
            LifecycleRequestKind::ReleaseUnsubmitted,
            &ignored);
    }

    stop_.store(true);
    scheduler_cv_.notify_all();
    reconstruction_cv_.notify_all();
    persistence_cv_.notify_all();
    worker_event_batch_cv_.notify_all();
    authority_cv_.notify_all();
    lease_heartbeat_cv_.notify_all();
    lifecycle_cv_.notify_all();

    // The scheduler is the only thread allowed to create worker lanes. Stop
    // and join it before draining the lane set so it cannot publish a final
    // joinable lane after StopWorkerLanes() has taken its snapshot.
    if (scheduler_thread_.joinable()) scheduler_thread_.join();
    StopWorkerLanes();

    if (reconstruction_thread_.joinable()) {
        reconstruction_thread_.join();
    }
    if (lease_heartbeat_thread_.joinable()) {
        lease_heartbeat_thread_.join();
    }
    if (authority_thread_.joinable()) authority_thread_.join();
    {
        std::lock_guard lock(cancellation_mutation_mutex_);
        cancellation_mutation_stop_ = true;
    }
    cancellation_mutation_cv_.notify_all();
    if (cancellation_mutation_thread_.joinable()) {
        cancellation_mutation_thread_.join();
    }
    for (auto& thread : persistence_threads_) {
        if (thread.joinable()) thread.join();
    }
    persistence_threads_.clear();
    if (worker_event_batch_thread_.joinable()) {
        worker_event_batch_thread_.join();
    }
    worker_coordinator_->SetCallbacks({});
    blob_store_->UnpinAll();
    {
        std::lock_guard lock(lease_heartbeat_mutex_);
        lease_heartbeat_entries_.clear();
    }
    blob_store_ready_.store(false);
    running_.store(false);
    lifecycle_cv_.notify_all();
}

void JobExecutionCoordinator::Impl::SetPaused(bool paused) {
    user_paused_.store(paused);
    WakeScheduler(paused ? "user-pause" : "user-resume", false);
    std::lock_guard lock(lanes_mutex_);
    for (const auto& [worker_id, lane] : lanes_) {
        (void)worker_id;
        lane->cv.notify_all();
    }
}

bool JobExecutionCoordinator::Impl::IsPaused() const noexcept {
    return user_paused_.load() || invariant_paused_.load();
}

bool JobExecutionCoordinator::Impl::IsRunning() const noexcept {
    return running_.load();
}

bool JobExecutionCoordinator::Impl::ClearInvariantPause() {
    const bool changed = invariant_paused_.exchange(false);
    if (changed) {
        WakeScheduler("invariant-resume", false);
        std::lock_guard lock(lanes_mutex_);
        for (const auto& [worker_id, lane] : lanes_) {
            (void)worker_id;
            lane->cv.notify_all();
        }
    }
    return changed;
}

void JobExecutionCoordinator::Impl::RegisterCancellationCommitPending(
    std::uint64_t hold_id,
    const std::vector<savor::db::ExecutionCancellationRequestSpec>&
        cancellations) {
    if (hold_id == 0 || cancellations.empty()) return;
    std::vector<std::int64_t> jobs;
    jobs.reserve(cancellations.size());
    for (const auto& cancellation : cancellations) {
        if (cancellation.job_id <= 0) continue;
        jobs.push_back(cancellation.job_id);
    }
    std::ranges::sort(jobs);
    jobs.erase(std::unique(jobs.begin(), jobs.end()), jobs.end());
    if (jobs.empty()) return;
    {
        std::lock_guard lock(cancellation_index_mutex_);
        const auto [_, inserted] = pending_cancellation_holds_.emplace(
            hold_id, jobs);
        if (!inserted) return;
        for (const auto job_id : jobs)
            ++pending_cancellation_hold_counts_[job_id];
    }
    ++cancellation_precommit_holds_registered_;
    std::lock_guard lock(lanes_mutex_);
    for (const auto& [_, lane] : lanes_) lane->cv.notify_all();
}

void JobExecutionCoordinator::Impl::RegisterCommittedCancellations(
    std::uint64_t hold_id,
    const std::vector<savor::db::CommittedJobCancellation>&
        cancellations) {
    std::vector<std::int64_t> held_jobs;
    bool mismatch = false;
    {
        std::lock_guard lock(cancellation_index_mutex_);
        const auto hold = pending_cancellation_holds_.find(hold_id);
        if (hold == pending_cancellation_holds_.end()) {
            mismatch = hold_id != 0 && !cancellations.empty();
        } else {
            held_jobs = hold->second;
            std::vector<std::int64_t> committed_jobs;
            committed_jobs.reserve(cancellations.size());
            for (const auto& cancellation : cancellations)
                committed_jobs.push_back(cancellation.job_id);
            std::ranges::sort(committed_jobs);
            committed_jobs.erase(
                std::unique(committed_jobs.begin(), committed_jobs.end()),
                committed_jobs.end());
            mismatch = committed_jobs != held_jobs;
            if (!mismatch) {
                for (const auto& cancellation : cancellations) {
                    committed_cancellations_by_job_.insert_or_assign(
                        cancellation.job_id,
                        CancellationIndexEntry{cancellation});
                }
                for (const auto job_id : held_jobs) {
                    const auto count =
                        pending_cancellation_hold_counts_.find(job_id);
                    if (count != pending_cancellation_hold_counts_.end()) {
                        if (count->second <= 1)
                            pending_cancellation_hold_counts_.erase(count);
                        else
                            --count->second;
                    }
                }
                pending_cancellation_holds_.erase(hold);
            }
        }
    }
    if (mismatch) {
        invariant_paused_.store(true);
        RecordError(
            "committed cancellation receipts do not match their precommit hold");
        WakeScheduler("cancellation-receipt-invariant", false);
        return;
    }
    if (hold_id != 0) {
        ++cancellation_precommit_holds_promoted_;
    }
    committed_cancellations_indexed_.fetch_add(cancellations.size());

    for (const auto& cancellation : cancellations) {
        DispatchPtr dispatch;
        std::vector<DispatchPtr> candidates;
        {
            std::lock_guard registry_lock(registry_mutex_);
            candidates.reserve(dispatches_.size());
            for (const auto& [_, candidate] : dispatches_)
                candidates.push_back(candidate);
        }
        for (const auto& candidate : candidates) {
            std::lock_guard record_lock(candidate->mutex);
            const bool contains = std::ranges::any_of(
                candidate->claimed.items,
                [&](const auto& item) {
                    return item.job_id == cancellation.job_id;
                });
            if (contains) {
                dispatch = candidate;
                break;
            }
        }
        if (!dispatch) {
            if (cancellation.durable_job_state == "EXECUTION_FINISHED"
                || cancellation.durable_job_state == "SUCCEEDED"
                || cancellation.durable_job_state == "SUCCEEDED_WINNER"
                || cancellation.durable_job_state == "SUCCEEDED_DUPLICATE"
                || cancellation.durable_job_state == "FAILED"
                || cancellation.durable_job_state == "CANCELED"
                || cancellation.durable_job_state == "SUPERSEDED") {
                (void)ResolveClaimedCancellation(
                    cancellation, "NO_LONGER_EXECUTABLE", false);
            }
            continue;
        }
        std::optional<savor::runtime::WorkerWorksetItemId> item_id;
        WorkerExecutionTarget target;
        DispatchPhase phase = DispatchPhase::Claimed;
        bool terminal_staged = false;
        {
            std::lock_guard lock(dispatch->mutex);
            phase = dispatch->phase;
            target = dispatch->target;
            terminal_staged =
                dispatch->staged_jobs.contains(cancellation.job_id);
            for (const auto& [runtime_item_id, item_index] :
                 dispatch->item_index_by_id) {
                if (item_index < dispatch->claimed.items.size()
                    && dispatch->claimed.items[item_index].job_id
                        == cancellation.job_id) {
                    item_id = savor::runtime::WorkerWorksetItemId{
                        runtime_item_id};
                    break;
                }
            }
            if (phase == DispatchPhase::Submitting
                && !dispatch->dispatch_marked) {
                const bool already_pending = std::ranges::any_of(
                    dispatch->pending_submission_cancellations,
                    [&](const auto& pending) {
                        return pending.cancellation
                            .cancellation_request_id
                            == cancellation.cancellation_request_id;
                    });
                if (!already_pending) {
                    dispatch->pending_submission_cancellations.push_back({
                        .cancellation = cancellation,
                        .item_id = item_id,
                        .enqueued_at = Clock::now(),
                    });
                    ++post_fence_cancellation_commands_;
                }
            }
        }
        if (terminal_staged) {
            (void)ResolveClaimedCancellation(
                cancellation, "WORKER_TERMINAL", false,
                dispatch->claimed.dispatch_attempt_id,
                dispatch->claimed.claim_token);
        } else if ((phase == DispatchPhase::Active
                || phase == DispatchPhase::Draining)
            && item_id.has_value()) {
            ++post_fence_cancellation_commands_;
            EnqueueWorkerControl(
                target,
                {
                    .kind = WorkerControlKind::CancelItem,
                    .dispatch_attempt_id =
                        dispatch->claimed.dispatch_attempt_id,
                    .job_id = cancellation.job_id,
                    .item_id = item_id,
                    .reason = cancellation.reason_text.value_or(
                        cancellation.reason_code),
                    .cancellation = cancellation,
                });
        }
    }
    WakeScheduler("committed-cancellation", false);
    std::lock_guard lock(lanes_mutex_);
    for (const auto& [_, lane] : lanes_) lane->cv.notify_all();
}

void JobExecutionCoordinator::Impl::OpenCancellationAdmission() {
    std::vector<savor::db::CommittedJobCancellation> startup;
    {
        std::lock_guard lock(cancellation_index_mutex_);
        startup.swap(startup_unresolved_cancellations_);
    }
    if (!startup.empty()) {
        RegisterCommittedCancellations(0, startup);
    }
    cancellation_admission_open_.store(true);
    WakeScheduler("cancellation-admission-open", false);
    std::lock_guard lock(lanes_mutex_);
    for (const auto& [_, lane] : lanes_) lane->cv.notify_all();
}

void JobExecutionCoordinator::Impl::WakeScheduler(
    std::string reason,
    bool reset_backoff) {
    {
        std::lock_guard lock(scheduler_mutex_);
        ++scheduler_wakeup_generation_;
        last_scheduler_wake_reason_ = std::move(reason);
        if (reset_backoff) {
            claim_backoff_ = {};
        }
    }
    ++scheduler_wakeups_;
    scheduler_cv_.notify_all();
}

bool JobExecutionCoordinator::Impl::IsDispatchAdmissionPaused()
    const noexcept {
    return quiescing_.load() || user_paused_.load()
        || invariant_paused_.load() || storage_paused_.load()
        || cancellation_storage_paused_.load()
        || !cancellation_admission_open_.load();
}

JobExecutionCoordinatorTelemetry
JobExecutionCoordinator::Impl::SnapshotTelemetry() const {
    JobExecutionCoordinatorTelemetry telemetry{};
    telemetry.claim_batches = claim_batches_.load();
    telemetry.successful_claim_batches =
        successful_claim_batches_.load();
    telemetry.empty_claim_batches = empty_claim_batches_.load();
    telemetry.worksets_claimed = worksets_claimed_.load();
    telemetry.worksets_reconstructed = worksets_reconstructed_.load();
    telemetry.worksets_submitted = worksets_submitted_.load();
    telemetry.submission_calls_started =
        submission_calls_started_.load();
    telemetry.submission_accepted = submission_accepted_.load();
    telemetry.submission_temporary_unavailable =
        submission_temporary_unavailable_.load();
    telemetry.submission_stale_generation =
        submission_stale_generation_.load();
    telemetry.submission_incompatible =
        submission_incompatible_.load();
    telemetry.submission_deterministic_rejection =
        submission_deterministic_rejection_.load();
    telemetry.submission_ambiguous_after_write =
        submission_ambiguous_after_write_.load();
    telemetry.submission_transport_canceled_before_write =
        submission_transport_canceled_before_write_.load();
    telemetry.worksets_durably_dispatched =
        worksets_submitted_.load();
    telemetry.reconstruction_invariant_failures =
        reconstruction_invariant_failures_.load();
    telemetry.jobs_started = jobs_started_.load();
    telemetry.worker_terminals_observed =
        worker_terminals_observed_.load();
    telemetry.worker_terminals_staged =
        worker_terminals_staged_.load();
    telemetry.worker_terminals_discarded_after_authority_release =
        worker_terminals_discarded_after_authority_release_.load();
    telemetry.worker_terminal_acks = worker_terminal_acks_.load();
    telemetry.worker_terminal_ack_abandoned_generation_loss =
        worker_terminal_ack_abandoned_generation_loss_.load();
    telemetry.worker_terminal_staging_failures =
        worker_terminal_staging_failures_.load();
    telemetry.worker_terminal_retry_attempts =
        worker_terminal_retry_attempts_.load();
    telemetry.worker_event_batches = worker_event_batches_.load();
    telemetry.worker_event_batch_items = worker_event_batch_items_.load();
    telemetry.worker_event_batch_rollbacks =
        worker_event_batch_rollbacks_.load();
    telemetry.worker_event_batch_retries =
        worker_event_batch_retries_.load();
    telemetry.worker_event_full_flushes =
        worker_event_full_flushes_.load();
    telemetry.worker_event_deadline_flushes =
        worker_event_deadline_flushes_.load();
    telemetry.worker_event_barrier_flushes =
        worker_event_barrier_flushes_.load();
    telemetry.worker_event_batch_max_size =
        worker_event_batch_max_size_.load();
    telemetry.worker_event_batch_total_size =
        worker_event_batch_total_size_.load();
    if (telemetry.worker_event_batches != 0) {
        telemetry.worker_event_batch_average_size =
            static_cast<double>(telemetry.worker_event_batch_total_size)
            / static_cast<double>(telemetry.worker_event_batches);
    }
    telemetry.worker_event_batch_max_collection_age_ms =
        worker_event_batch_max_collection_age_ms_.load();
    telemetry.blob_readiness_failures =
        blob_readiness_failures_.load();
    telemetry.cancellations_delivered =
        cancellations_delivered_.load();
    telemetry.cancellation_precommit_holds_registered =
        cancellation_precommit_holds_registered_.load();
    telemetry.cancellation_precommit_holds_promoted =
        cancellation_precommit_holds_promoted_.load();
    telemetry.committed_cancellations_indexed =
        committed_cancellations_indexed_.load();
    telemetry.waiting_jobs_suppressed_by_sidecar =
        waiting_jobs_suppressed_by_sidecar_.load();
    telemetry.fully_canceled_worksets_avoided =
        fully_canceled_worksets_avoided_.load();
    telemetry.sidecar_items_submitted =
        sidecar_items_submitted_.load();
    telemetry.sidecar_submit_receipts_accepted =
        sidecar_submit_receipts_accepted_.load();
    telemetry.sidecar_submit_receipts_repeated =
        sidecar_submit_receipts_repeated_.load();
    telemetry.sidecar_submit_receipts_mismatched =
        sidecar_submit_receipts_mismatched_.load();
    telemetry.post_fence_cancellation_commands =
        post_fence_cancellation_commands_.load();
    telemetry.cancellation_mutation_batches =
        cancellation_mutation_batches_.load();
    telemetry.cancellation_mutation_batch_items =
        cancellation_mutation_batch_items_.load();
    if (telemetry.cancellation_mutation_batches != 0) {
        telemetry.cancellation_mutation_batch_average_size =
            static_cast<double>(telemetry.cancellation_mutation_batch_items)
            / static_cast<double>(telemetry.cancellation_mutation_batches);
    }
    telemetry.cancellation_mutation_full_flushes =
        cancellation_mutation_full_flushes_.load();
    telemetry.cancellation_mutation_deadline_flushes =
        cancellation_mutation_deadline_flushes_.load();
    telemetry.cancellation_mutation_barrier_flushes =
        cancellation_mutation_barrier_flushes_.load();
    telemetry.cancellation_mutation_rollbacks =
        cancellation_mutation_rollbacks_.load();
    telemetry.cancellation_mutation_retries =
        cancellation_mutation_retries_.load();
    telemetry.cancellation_mutation_max_size =
        cancellation_mutation_max_size_.load();
    telemetry.cancellation_mutation_max_collection_age_ms =
        cancellation_mutation_max_collection_age_ms_.load();
    {
        std::lock_guard lock(cancellation_mutation_mutex_);
        telemetry.cancellation_mutation_queue_high_water =
            cancellation_mutation_high_water_;
    }
    telemetry.cancellation_deliveries_already_applied =
        cancellations_already_applied_.load();
    telemetry.worker_losses = worker_losses_.load();
    telemetry.startup_recovered_dispatches =
        startup_recovered_dispatches_.load();
    telemetry.startup_requeued_jobs = startup_requeued_jobs_.load();
    telemetry.startup_recovery_attempts_granted =
        startup_recovery_attempts_granted_.load();
    telemetry.active_residence_probes = active_residence_probes_.load();
    telemetry.active_residence_matches = active_residence_matches_.load();
    telemetry.active_residence_failures = active_residence_failures_.load();
    telemetry.active_lease_renewal_batches =
        active_lease_renewal_batches_.load();
    telemetry.active_lease_renewal_retries =
        active_lease_renewal_retries_.load();
    telemetry.draining_transitions = draining_transitions_.load();
    telemetry.scheduler_wakeups = scheduler_wakeups_.load();
    telemetry.availability_signal_wakeups =
        availability_signal_wakeups_.load();
    telemetry.reconciliation_claims = reconciliation_claims_.load();
    telemetry.worker_control_commands_queued =
        worker_control_commands_queued_.load();
    telemetry.worker_control_commands_attempted =
        worker_control_commands_attempted_.load();
    telemetry.worker_control_commands_applied =
        worker_control_commands_applied_.load();
    telemetry.worker_control_commands_failed =
        worker_control_commands_failed_.load();
    telemetry.worker_control_commands_abandoned =
        worker_control_commands_abandoned_.load();
    telemetry.worker_control_queue_high_water =
        static_cast<std::size_t>(
            worker_control_queue_high_water_.load());
    telemetry.cancellation_delivery_total_latency_ms =
        cancellation_delivery_total_latency_ms_.load();
    telemetry.cancellation_delivery_max_latency_ms =
        cancellation_delivery_max_latency_ms_.load();
    {
        std::lock_guard lock(cancellation_resolution_mutex_);
        telemetry.cancellation_resolution_counts =
            cancellation_resolution_counts_;
    }
    {
        std::lock_guard lock(dispatch_release_mutex_);
        telemetry.dispatch_release_reason_counts =
            dispatch_release_reason_counts_;
        telemetry.dispatch_release_phase_counts =
            dispatch_release_phase_counts_;
    }
    telemetry.blob_store_ready = blob_store_ready_.load();
    telemetry.user_admission_paused = user_paused_.load();
    telemetry.invariant_admission_paused = invariant_paused_.load();
    telemetry.storage_admission_paused = storage_paused_.load()
        || cancellation_storage_paused_.load();
    telemetry.claims_paused_for_terminal_staging =
        telemetry.storage_admission_paused;
    telemetry.invariant_paused =
        telemetry.invariant_admission_paused;
    {
        std::lock_guard lock(scheduler_mutex_);
        telemetry.last_scheduler_wake_reason =
            last_scheduler_wake_reason_;
        telemetry.availability_generation =
            execution_work_availability_.generation;
        telemetry.ready_worksets_present =
            execution_work_availability_.has_ready_worksets;
        telemetry.execution_finished_results_present =
            execution_work_availability_.has_execution_finished_results;
        telemetry.claim_backoff_stage = std::min<std::uint32_t>(
            claim_backoff_.empty_attempts,
            3);
        telemetry.current_claim_backoff_ms = claim_backoff_.delay_ms;
    }
    {
        std::lock_guard lock(cancellation_index_mutex_);
        telemetry.cancellation_precommit_holds_pending =
            pending_cancellation_holds_.size();
        telemetry.unresolved_requested_cancellation_canaries =
            static_cast<std::size_t>(std::count_if(
                committed_cancellations_by_job_.begin(),
                committed_cancellations_by_job_.end(),
                [](const auto& entry) {
                    return entry.second.cancellation.state == "REQUESTED";
                }));
    }

    const auto now = Clock::now();
    {
        std::lock_guard lock(reconstruction_mutex_);
        telemetry.reconstruction_queue_depth =
            reconstruction_queue_.size();
        telemetry.reconstruction_queue_high_water =
            reconstruction_high_water_;
        telemetry.reconstruction_active = reconstruction_active_;
        telemetry.reconstruction_total_duration_ms =
            reconstruction_total_duration_ms_;
        telemetry.reconstruction_max_duration_ms =
            reconstruction_max_duration_ms_;
        if (!reconstruction_queue_.empty()) {
            telemetry.reconstruction_oldest_item_age_ms =
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - reconstruction_queue_.front().enqueued_at)
                        .count());
        }
    }
    {
        std::lock_guard lock(persistence_mutex_);
        telemetry.persistence_queue_high_water =
            persistence_high_water_;
        std::optional<Clock::time_point> oldest;
        for (const auto& [key, stream] : persistence_streams_) {
            (void)key;
            telemetry.persistence_queue_depth += stream.events.size();
            telemetry.pending_worker_terminals +=
                static_cast<std::size_t>(std::count_if(
                    stream.events.begin(),
                    stream.events.end(),
                    [](const PersistenceEvent& event) {
                        return std::holds_alternative<TerminalEvent>(
                            event.payload);
                    }));
            if (stream.active) ++telemetry.active_worker_streams;
            if (!stream.events.empty()
                && (!oldest.has_value()
                    || stream.events.front().enqueued_at < *oldest)) {
                oldest = stream.events.front().enqueued_at;
            }
        }
        if (oldest.has_value()) {
            telemetry.persistence_oldest_event_age_ms =
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - *oldest)
                        .count());
        }
    }
    {
        std::lock_guard lock(worker_event_batch_mutex_);
        telemetry.worker_event_batch_queue_depth =
            worker_event_batch_queue_.size();
        telemetry.worker_event_batch_queue_high_water =
            worker_event_batch_high_water_;
    }
    telemetry.pending_cancellation_mutations =
        pending_cancellation_mutations_.load();
    {
        std::lock_guard lock(lanes_mutex_);
        for (const auto& [worker_id, lane] : lanes_) {
            (void)worker_id;
            std::lock_guard lane_lock(lane->mutex);
            telemetry.reserved_slots += lane->reservations;
            telemetry.reconstructed_waiting_worksets +=
                lane->waiting.size();
            telemetry.worker_control_queue_depth +=
                lane->controls.size();
            if (lane->submitting.has_value()) {
                ++telemetry.submitting_worksets;
            }
            if (lane->active.has_value()) {
                ++telemetry.active_worksets;
            }
            for (const auto& command : lane->controls) {
                if (command.enqueued_at != Clock::time_point{}) {
                    telemetry.worker_control_oldest_command_age_ms =
                        std::max(
                            telemetry.worker_control_oldest_command_age_ms,
                            static_cast<std::uint64_t>(
                                std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                        now - command.enqueued_at)
                                    .count()));
                }
                if (command.kind == WorkerControlKind::Acknowledge) {
                    ++telemetry.pending_acknowledgements;
                } else {
                    ++telemetry.pending_cancellations;
                }
            }
        }
    }
    {
        std::lock_guard lock(registry_mutex_);
        for (const auto& [dispatch_id, record] : dispatches_) {
            (void)dispatch_id;
            std::lock_guard record_lock(record->mutex);
            if (record->phase == DispatchPhase::Draining) {
                ++telemetry.draining_worksets;
            }
        }
    }
    telemetry.buffered_worksets =
        telemetry.reserved_slots
        + telemetry.reconstructed_waiting_worksets;
    {
        std::lock_guard lock(error_mutex_);
        telemetry.last_error = last_error_;
    }
    return telemetry;
}

std::vector<JobExecutionWorkerLaneSnapshot>
JobExecutionCoordinator::Impl::SnapshotWorkerLanes() const {
    std::vector<JobExecutionWorkerLaneSnapshot> snapshots;
    std::vector<WorkerLanePtr> lanes;
    {
        std::lock_guard lock(lanes_mutex_);
        for (const auto& [worker_id, lane] : lanes_) {
            (void)worker_id;
            lanes.push_back(lane);
        }
    }
    std::sort(
        lanes.begin(),
        lanes.end(),
        [](const WorkerLanePtr& lhs, const WorkerLanePtr& rhs) {
            return lhs->worker_id < rhs->worker_id;
        });
    for (const auto& lane : lanes) {
        JobExecutionWorkerLaneSnapshot snapshot{};
        {
            std::lock_guard lock(lane->mutex);
            snapshot.worker_id = lane->worker_id;
            snapshot.process_generation = lane->generation;
            snapshot.reservations = lane->reservations;
            snapshot.reconstructing = lane->reconstructing;
            snapshot.waiting_queue_depth = lane->waiting.size();
            snapshot.submitting_dispatch_attempt_id =
                lane->submitting;
            snapshot.active_dispatch_attempt_id = lane->active;
            snapshot.control_queue_depth = lane->controls.size();
            for (const auto& command : lane->controls) {
                if (command.kind == WorkerControlKind::Acknowledge) {
                    ++snapshot.pending_acknowledgements;
                } else {
                    ++snapshot.pending_cancellations;
                }
            }
            snapshot.actual_execution_affinity_key =
                lane->actual_execution_key;
            snapshot.actual_baseline_affinity_key =
                lane->actual_baseline_key;
            if (!lane->projected_affinities.empty()) {
                snapshot.affinity_state =
                    WorkerSchedulerAffinityState::Projected;
                const auto& projected =
                    lane->projected_affinities.rbegin()->second;
                snapshot.projected_execution_affinity_key =
                    projected.execution_key;
                snapshot.projected_baseline_affinity_key =
                    projected.baseline_key;
            } else if (lane->has_submitted_workset) {
                snapshot.affinity_state =
                    WorkerSchedulerAffinityState::Actual;
            }
        }
        const auto key =
            WorkerStreamKey(snapshot.worker_id, snapshot.process_generation);
        {
            std::lock_guard lock(persistence_mutex_);
            const auto found = persistence_streams_.find(key);
            if (found != persistence_streams_.end()) {
                snapshot.persistence_queue_depth =
                    found->second.events.size();
            }
        }
        snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
}

std::vector<JobExecutionCoordinatorWarning>
JobExecutionCoordinator::Impl::SnapshotWarnings() const {
    std::lock_guard lock(warning_mutex_);
    return {warnings_.begin(), warnings_.end()};
}

void JobExecutionCoordinator::Impl::ConfigureWorkerCallbacks() {
    worker_coordinator_->SetCallbacks(
        {
            .availability_changed = [this]() {
                WakeScheduler("worker-availability", false);
                std::lock_guard lock(lanes_mutex_);
                for (const auto& [worker_id, lane] : lanes_) {
                    (void)worker_id;
                    lane->cv.notify_all();
                }
            },
            .workset_state =
                [this](
                    const WorkerCoordinatorEventContext& source,
                    const savor::wrms::WorksetStatePayload& payload) {
                    HandleWorksetState(source, payload);
                },
            .item_started =
                [this](
                    const WorkerCoordinatorEventContext& source,
                    const savor::wrms::WorksetItemStartedPayload& payload) {
                    EnqueuePersistence(
                        {
                            .worker_id = source.worker_id,
                            .generation = source.process_generation,
                            .enqueued_at = Clock::now(),
                            .payload = ItemStartedEvent{source, payload},
                        });
                },
            .item_progress =
                [](const WorkerCoordinatorEventContext&,
                   const savor::wrms::InvocationProgressPayload&) {},
            .item_terminal =
                [this](
                    const savor::runtime::
                        DurableWorkerTerminalEnvelope& envelope) {
                    ++worker_terminals_observed_;
                    EnqueuePersistence(
                        {
                            .worker_id = envelope.worker_id,
                            .generation = envelope.process_generation,
                            .enqueued_at = Clock::now(),
                            .payload = TerminalEvent{envelope, 0},
                        });
                },
            .credits =
                [this](
                    const WorkerCoordinatorEventContext& source,
                    const savor::wrms::WorksetCreditsPayload&) {
                    WakeScheduler("worker-credits", false);
                    const auto lane = FindWorkerLane(
                        source.worker_id,
                        source.process_generation);
                    if (lane) lane->cv.notify_all();
                },
            .workset_summary =
                [this](
                    const WorkerCoordinatorEventContext& source,
                    const savor::wrms::WorksetSummaryPayload& payload) {
                    EnqueuePersistence(
                        {
                            .worker_id = source.worker_id,
                            .generation = source.process_generation,
                            .enqueued_at = Clock::now(),
                            .payload = SummaryEvent{source, payload},
                        });
                },
            .worker_unavailable =
                [this](const WorkerUnavailableEvent& event) {
                    EnqueueAuthority(
                        [this, event]() mutable {
                            HandleWorkerUnavailable(std::move(event));
                        });
                },
        });
}

JobExecutionCoordinator::Impl::WorkerLanePtr
JobExecutionCoordinator::Impl::FindWorkerLane(
    std::size_t worker_id,
    std::uint64_t generation) const {
    std::lock_guard lock(lanes_mutex_);
    const auto found = lanes_.find(worker_id);
    if (found == lanes_.end()) return {};
    std::lock_guard lane_lock(found->second->mutex);
    return found->second->generation == generation
        ? found->second
        : WorkerLanePtr{};
}

JobExecutionCoordinator::Impl::WorkerLanePtr
JobExecutionCoordinator::Impl::EnsureWorkerLane(
    const ReadyWorkerCompatibilitySnapshot& worker) {
    WorkerLanePtr old_lane;
    WorkerLanePtr lane;
    {
        std::lock_guard lock(lanes_mutex_);
        const auto found = lanes_.find(worker.worker_id);
        if (found != lanes_.end()) {
            std::lock_guard lane_lock(found->second->mutex);
            if (found->second->generation
                == worker.process_generation) {
                return found->second;
            }
            old_lane = found->second;
            lanes_.erase(found);
        }
        lane = std::make_shared<WorkerLane>();
        lane->worker_id = worker.worker_id;
        lane->generation = worker.process_generation;
        lanes_.emplace(worker.worker_id, lane);
    }
    if (old_lane) {
        {
            std::lock_guard lock(old_lane->mutex);
            old_lane->stop = true;
        }
        old_lane->cv.notify_all();
        if (old_lane->thread.joinable()) old_lane->thread.join();
        std::deque<WorkerControlCommand> abandoned_controls;
        {
            std::lock_guard lock(old_lane->mutex);
            abandoned_controls.swap(old_lane->controls);
        }
        for (auto& command : abandoned_controls) {
            ++worker_control_commands_abandoned_;
            if (command.kind == WorkerControlKind::Acknowledge) {
                ++worker_terminal_ack_abandoned_generation_loss_;
            }
            if (command.cancellation.has_value()) {
                WorkerCommandResult stale{
                    .disposition =
                        WorkerCommandDisposition::StaleRoute,
                    .worker_id = old_lane->worker_id,
                    .diagnostic =
                        "worker generation was replaced before control delivery",
                };
                EnqueueAuthority(
                    [this,
                     cancellation = *command.cancellation,
                     stale,
                     whole = command.kind
                         == WorkerControlKind::CancelWorkset,
                     enqueued_at = command.enqueued_at]() {
                        CompleteCancellationDelivery(
                            cancellation,
                            stale,
                            whole,
                            enqueued_at);
                    });
            }
        }
        const WorkerUnavailableEvent replaced{
            .source =
                {
                    .worker_id = old_lane->worker_id,
                    .process_generation = old_lane->generation,
                },
            .diagnostic =
                "worker process generation was replaced during scheduling",
        };
        EnqueueAuthority(
            [this, replaced]() mutable {
                HandleWorkerUnavailable(std::move(replaced));
            });
    }
    lane->thread =
        std::thread([this, lane]() { WorkerLaneLoop(lane); });
    return lane;
}

void JobExecutionCoordinator::Impl::StopWorkerLanes() {
    std::vector<WorkerLanePtr> lanes;
    {
        std::lock_guard lock(lanes_mutex_);
        for (auto& [worker_id, lane] : lanes_) {
            (void)worker_id;
            lanes.push_back(lane);
        }
        lanes_.clear();
    }
    for (const auto& lane : lanes) {
        {
            std::lock_guard lock(lane->mutex);
            lane->stop = true;
        }
        lane->cv.notify_all();
    }
    for (const auto& lane : lanes) {
        if (lane->thread.joinable()) lane->thread.join();
        std::lock_guard lock(lane->mutex);
        for (const auto& command : lane->controls) {
            ++worker_control_commands_abandoned_;
            if (command.kind == WorkerControlKind::Acknowledge) {
                ++worker_terminal_ack_abandoned_generation_loss_;
            }
        }
        lane->controls.clear();
    }
}

JobExecutionCoordinator::Impl::WorksetAffinity
JobExecutionCoordinator::Impl::AffinityOf(
    const savor::db::ExecutionWorksetCompatibility& compatibility) {
    return {
        .module_canonical_id = compatibility.module_canonical_id,
        .execution_key = compatibility.execution_affinity_key,
        .baseline_key = compatibility.baseline_affinity_key,
    };
}

int JobExecutionCoordinator::Impl::AffinityScore(
    const WorkerLane& lane,
    const savor::db::ExecutionWorksetCompatibility& compatibility) {
    const bool declares_affinity =
        compatibility.execution_affinity_key.has_value()
        || compatibility.baseline_affinity_key.has_value();
    if (!declares_affinity) return 0;

    // Until a first reservation establishes projected state, a worker that
    // has never been sent work is universal rather than cold-penalized.
    if (!lane.has_submitted_workset
        && lane.projected_affinities.empty()) {
        return 7;
    }

    std::optional<std::string> module =
        lane.actual_module_canonical_id;
    std::optional<std::string> execution = lane.actual_execution_key;
    std::optional<std::string> baseline = lane.actual_baseline_key;
    if (!lane.projected_affinities.empty()) {
        const auto& projected =
            lane.projected_affinities.rbegin()->second;
        module = projected.module_canonical_id;
        execution = projected.execution_key;
        baseline = projected.baseline_key;
    }

    int score = 0;
    if (module == std::optional<std::string>(
            compatibility.module_canonical_id)) {
        score += 1;
    }
    if (compatibility.execution_affinity_key.has_value()
        && execution == compatibility.execution_affinity_key) {
        score += 2;
    }
    if (compatibility.baseline_affinity_key.has_value()
        && baseline == compatibility.baseline_affinity_key) {
        score += 4;
    }
    return score;
}

void JobExecutionCoordinator::Impl::RefreshActualAffinity(
    WorkerLane& lane,
    const ReadyWorkerCompatibilitySnapshot& worker) {
    lane.actual_module_canonical_id = worker.warm_program_module_id;
    lane.actual_execution_key = worker.warm_execution_key_sha256;
    lane.actual_baseline_key = worker.warm_baseline_sha256;
}

void JobExecutionCoordinator::Impl::RemoveProjectedAffinity(
    WorkerLane& lane,
    std::int64_t dispatch_attempt_id) {
    lane.projected_affinities.erase(dispatch_attempt_id);
}

void JobExecutionCoordinator::Impl::SchedulerLoop() {
    struct FreeLane {
        WorkerLanePtr lane;
        std::size_t initial_depth = 0;
        std::size_t free = 0;
        std::size_t assigned = 0;
    };
    while (!stop_.load()) {
        if (IsDispatchAdmissionPaused()) {
            std::unique_lock lock(scheduler_mutex_);
            scheduler_cv_.wait_for(lock, std::chrono::seconds(1));
            continue;
        }

        const auto workers = worker_coordinator_->SnapshotReadyWorkers();
        std::vector<FreeLane> lanes;
        lanes.reserve(workers.size());
        std::size_t requested = 0;
        for (const auto& worker : workers) {
            const auto lane = EnsureWorkerLane(worker);
            std::size_t free = 0;
            std::size_t occupied = 0;
            {
                std::lock_guard lock(lane->mutex);
                if (lane->generation != worker.process_generation) continue;
                RefreshActualAffinity(*lane, worker);
                occupied =
                    lane->reservations + lane->waiting.size();
                free = occupied < config_.worker_queue_capacity
                    ? config_.worker_queue_capacity - occupied
                    : 0;
            }
            lanes.push_back({lane, occupied, free, 0});
            requested += free;
        }
        std::sort(
            lanes.begin(),
            lanes.end(),
            [](const FreeLane& lhs, const FreeLane& rhs) {
                return lhs.lane->worker_id < rhs.lane->worker_id;
            });

        const auto now = Clock::now();
        bool ready_signal = false;
        bool reconciliation_due = false;
        Clock::time_point next_attempt = now + std::chrono::seconds(30);
        {
            std::lock_guard lock(scheduler_mutex_);
            ready_signal = execution_work_availability_.has_ready_worksets;
            if (next_full_claim_reconciliation_ == Clock::time_point{}) {
                next_full_claim_reconciliation_ =
                    now + std::chrono::seconds(30);
            }
            reconciliation_due = now >= next_full_claim_reconciliation_;
            next_attempt = std::min(
                claim_backoff_.next_attempt == Clock::time_point{}
                    ? next_full_claim_reconciliation_
                    : claim_backoff_.next_attempt,
                next_full_claim_reconciliation_);
        }

        const bool backoff_elapsed = [&]() {
            std::lock_guard lock(scheduler_mutex_);
            return claim_backoff_.next_attempt == Clock::time_point{}
                || claim_backoff_.next_attempt <= now;
        }();
        if (requested > 0
            && (ready_signal || reconciliation_due || backoff_elapsed)) {
            std::string error;
            ++claim_batches_;
            auto claimed = execution_db_->ClaimPublishedWorksetBatch(
                {
                    .batch_nonce = NextToken("workset-batch"),
                    .requested_workset_count = requested,
                },
                &error);
            if (!error.empty()) RecordError(std::move(error));
            if (claimed.size() > requested) {
                for (const auto& row : claimed) {
                    savor::db::WorksetDispatchMutationReceipt receipt{};
                    std::string release_error;
                    (void)execution_db_->ReleaseWorksetDispatch(
                        {
                            .dispatch_attempt_id = row.dispatch_attempt_id,
                            .claim_token = row.claim_token,
                            .reason_code = "BATCH_CLAIM_OVERFLOW",
                            .reason_text =
                                "DB returned more claims than reserved slots",
                            .requested_by = "job_execution_coordinator",
                        },
                        &receipt,
                        &release_error);
                }
                claimed.clear();
                invariant_paused_.store(true);
                RecordError(
                    "batch claim returned more worksets than requested");
            }

            if (claimed.empty()) {
                ++empty_claim_batches_;
                std::lock_guard lock(scheduler_mutex_);
                ++claim_backoff_.empty_attempts;
                const std::uint64_t delays[] = {1000, 5000, 30000};
                const auto index = std::min<std::size_t>(
                    claim_backoff_.empty_attempts - 1,
                    std::size(delays) - 1);
                claim_backoff_.delay_ms = delays[index];
                claim_backoff_.next_attempt = Clock::now()
                    + std::chrono::milliseconds(
                        claim_backoff_.delay_ms);
            } else {
                ++successful_claim_batches_;
                worksets_claimed_.fetch_add(claimed.size());
                if (reconciliation_due && !ready_signal) {
                    reconciliation_claims_.fetch_add(claimed.size());
                }
                {
                    std::lock_guard lock(scheduler_mutex_);
                    claim_backoff_ = {};
                }
            }
            if (reconciliation_due) {
                std::lock_guard lock(scheduler_mutex_);
                next_full_claim_reconciliation_ =
                    Clock::now() + std::chrono::seconds(30);
            }

            for (auto& row : claimed) {
                std::size_t minimum_depth =
                    std::numeric_limits<std::size_t>::max();
                for (const auto& entry : lanes) {
                    if (entry.assigned < entry.free) {
                        minimum_depth = std::min(
                            minimum_depth,
                            entry.initial_depth + entry.assigned);
                    }
                }

                int best_affinity = std::numeric_limits<int>::min();
                std::vector<std::size_t> candidates;
                for (std::size_t index = 0; index < lanes.size(); ++index) {
                    auto& entry = lanes[index];
                    if (entry.assigned >= entry.free
                        || entry.initial_depth + entry.assigned
                            != minimum_depth) {
                        continue;
                    }
                    std::lock_guard lane_lock(entry.lane->mutex);
                    if (entry.lane->stop) continue;
                    const auto score = AffinityScore(
                        *entry.lane,
                        row.compatibility);
                    if (score > best_affinity) {
                        best_affinity = score;
                        candidates.clear();
                    }
                    if (score == best_affinity) candidates.push_back(index);
                }

                std::optional<std::size_t> selected_index;
                std::size_t last_worker =
                    std::numeric_limits<std::size_t>::max();
                {
                    std::lock_guard lock(scheduler_mutex_);
                    last_worker = last_assigned_worker_;
                }
                for (const auto index : candidates) {
                    if (last_worker
                            != std::numeric_limits<std::size_t>::max()
                        && lanes[index].lane->worker_id > last_worker) {
                        selected_index = index;
                        break;
                    }
                }
                if (!selected_index.has_value() && !candidates.empty()) {
                    selected_index = candidates.front();
                }

                if (!selected_index.has_value()) {
                    savor::db::WorksetDispatchMutationReceipt receipt{};
                    std::string release_error;
                    (void)execution_db_->ReleaseWorksetDispatch(
                        {
                            .dispatch_attempt_id = row.dispatch_attempt_id,
                            .claim_token = row.claim_token,
                            .reason_code = "NO_RESERVED_WORKER",
                            .reason_text =
                                "ready worker generation was lost during assignment",
                            .requested_by = "job_execution_coordinator",
                        },
                        &receipt,
                        &release_error);
                    if (!release_error.empty()) {
                        RecordError(std::move(release_error));
                    }
                    continue;
                }

                auto& selected = lanes[*selected_index];
                const auto& lane = selected.lane;
                auto dispatch = std::make_shared<DispatchRecord>();
                dispatch->claimed = std::move(row);
                dispatch->target = {
                    .worker_id = lane->worker_id,
                    .process_generation = lane->generation,
                };
                dispatch->phase = DispatchPhase::Reconstructing;
                InsertDispatch(dispatch);
                {
                    std::lock_guard lock(lane->mutex);
                    ++lane->reservations;
                    ++lane->reconstructing;
                    lane->projected_affinities.insert_or_assign(
                        dispatch->claimed.dispatch_attempt_id,
                        AffinityOf(dispatch->claimed.compatibility));
                }
                ++selected.assigned;
                {
                    std::lock_guard lock(scheduler_mutex_);
                    last_assigned_worker_ = lane->worker_id;
                }
                {
                    std::lock_guard lock(reconstruction_mutex_);
                    reconstruction_queue_.push_back(
                        {dispatch, Clock::now()});
                    reconstruction_high_water_ = std::max(
                        reconstruction_high_water_,
                        reconstruction_queue_.size());
                }
                reconstruction_cv_.notify_one();
            }
        }

        {
            std::lock_guard lock(scheduler_mutex_);
            next_attempt = std::min(
                claim_backoff_.next_attempt == Clock::time_point{}
                    ? next_full_claim_reconciliation_
                    : claim_backoff_.next_attempt,
                next_full_claim_reconciliation_);
        }
        if (requested == 0 && next_attempt <= Clock::now()) {
            next_attempt = Clock::now() + std::chrono::seconds(1);
        }
        std::unique_lock lock(scheduler_mutex_);
        const auto observed_generation = scheduler_wakeup_generation_;
        scheduler_cv_.wait_until(lock, next_attempt, [this, observed_generation]() {
            return stop_.load()
                || scheduler_wakeup_generation_ != observed_generation;
        });
    }
}

void JobExecutionCoordinator::Impl::ReconstructionLoop() {
    while (!stop_.load()) {
        ReconstructionItem item;
        {
            std::unique_lock lock(reconstruction_mutex_);
            reconstruction_cv_.wait(lock, [this]() {
                return stop_.load() || !reconstruction_queue_.empty();
            });
            if (stop_.load() && reconstruction_queue_.empty()) return;
            item = std::move(reconstruction_queue_.front());
            reconstruction_queue_.pop_front();
            reconstruction_active_ = true;
        }
        const auto started = Clock::now();
        (void)Reconstruct(item.dispatch);
        const auto duration = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - started)
                .count());
        {
            std::lock_guard lock(reconstruction_mutex_);
            reconstruction_active_ = false;
            reconstruction_total_duration_ms_ += duration;
            reconstruction_max_duration_ms_ =
                std::max(reconstruction_max_duration_ms_, duration);
        }
    }
}

bool JobExecutionCoordinator::Impl::Reconstruct(
    const DispatchPtr& dispatch) {
    if (!dispatch) return false;
    savor::db::ClaimedPublishedWorkset claimed;
    WorkerExecutionTarget target;
    {
        std::lock_guard lock(dispatch->mutex);
        if (dispatch->phase != DispatchPhase::Reconstructing) {
            return false;
        }
        claimed = dispatch->claimed;
        target = dispatch->target;
    }

    const auto* descriptor =
        program_kind_registry_->Find(claimed.program_kind);
    std::optional<
        savor::db::execution::programdb::WorksetReconstructionResult>
        reconstruction;
    std::string error;
    if (descriptor == nullptr
        || descriptor->workset_reconstruction == nullptr) {
        error = "program kind " + std::to_string(claimed.program_kind)
            + " has no registered workset reconstruction adapter";
    } else {
        savor::db::execution::programdb::WorksetReconstructionContext
            context{};
        context.workset_id = claimed.workset_id;
        context.dispatch_attempt_id = claimed.dispatch_attempt_id;
        context.workflow_step_id = claimed.workflow_step_id;
        context.root_job_set_id = claimed.root_job_set_id;
        context.dispatch_token = claimed.claim_token;
        context.compatibility_key =
            claimed.compatibility.compatibility_key;
        context.state_compatibility = config_.state_compatibility;
        context.items.reserve(claimed.items.size());
        for (const auto& item : claimed.items) {
            context.items.push_back(
                {
                    .job_id = item.job_id,
                    .logical_ordinal =
                        static_cast<std::uint32_t>(item.item_ordinal),
                    .reserved_attempt_id = item.reserved_attempt_id,
                    .claim_token = claimed.claim_token,
                    .program_kind = item.program_kind,
                    .program_version = item.program_version,
                    .program_ref_kind = item.program_ref_kind,
                    .program_ref_id = item.program_ref_id,
                    .savestate_id = item.savestate_id,
                    .fingerprint = item.fingerprint,
                    .input_ini = item.input_ini,
                });
        }
        try {
            reconstruction =
                descriptor->workset_reconstruction->Reconstruct(
                    context,
                    &error);
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error =
                "workset reconstruction threw an unknown exception";
        }
    }

    const auto lane = FindWorkerLane(
        target.worker_id,
        target.process_generation);
    if (!reconstruction.has_value()
        || !ValidateReconstruction(
            claimed,
            *reconstruction,
            &error)) {
        PauseForInvariant(
            dispatch,
            "Workset reconstruction invariant failed",
            error.empty() ? "descriptor returned no valid workset"
                          : std::move(error));
        return false;
    }

    bool accepted = false;
    if (lane) {
        {
            std::lock_guard record_lock(dispatch->mutex);
            std::lock_guard lane_lock(lane->mutex);
            if (dispatch->phase == DispatchPhase::Reconstructing
                && !lane->stop
                && lane->generation == target.process_generation) {
                dispatch->definition =
                    std::move(reconstruction->workset);
                dispatch->item_index_by_id.clear();
                for (std::size_t index = 0;
                     index < dispatch->definition.items.size();
                     ++index) {
                    dispatch->item_index_by_id.emplace(
                        dispatch->definition.items[index]
                            .item_id.value(),
                        index);
                }
                dispatch->phase = DispatchPhase::Waiting;
                if (lane->reconstructing > 0) --lane->reconstructing;
                if (lane->reservations > 0) --lane->reservations;
                lane->waiting.push_back(dispatch);
                accepted = true;
            }
        }
        lane->cv.notify_all();
    }
    if (!accepted) {
        EnqueueAuthority(
            [this, dispatch]() {
                (void)ReleaseDispatch(
                    dispatch,
                    "RECONSTRUCTION_TARGET_LOST",
                    "reserved worker generation was lost before "
                    "reconstruction completed");
            });
        return false;
    }
    ++worksets_reconstructed_;
    WakeScheduler("reconstruction-complete", false);
    return true;
}

bool JobExecutionCoordinator::Impl::ValidateReconstruction(
    const savor::db::ClaimedPublishedWorkset& claimed,
    const savor::db::execution::programdb::
        WorksetReconstructionResult& reconstruction,
    std::string* error_out) const {
    const auto fail = [error_out](std::string error) {
        if (error_out) *error_out = std::move(error);
        return false;
    };
    if (claimed.items.empty()
        || reconstruction.workset.workset_id.value()
            != static_cast<std::uint64_t>(
                claimed.dispatch_attempt_id)
        || reconstruction.workset.items.size()
            != claimed.items.size()
        || reconstruction.ordered_job_ids.size()
            != claimed.items.size()) {
        return fail(
            "reconstructed workset identity or item count changed");
    }
    const auto& compatibility = claimed.compatibility;
    const auto& key = reconstruction.workset.execution_key;
    if (key.module.canonical_id
            != compatibility.module_canonical_id
        || key.module.revision
            != static_cast<std::uint32_t>(
                compatibility.module_version)
        || key.module.canonical_hash
            != compatibility.module_sha256
        || key.entrypoint != compatibility.entrypoint
        || key.verified_dependency_sha256
            != compatibility.verified_dependency_sha256
        || key.runtime_profile_sha256
            != compatibility.runtime_profile_sha256
        || (compatibility.execution_affinity_key.has_value()
            && key.canonical_sha256
                != *compatibility.execution_affinity_key)
        || (compatibility.baseline_affinity_key.has_value()
            && key.baseline.sha256
                != *compatibility.baseline_affinity_key)) {
        return fail(
            "reconstructed workset changed durable compatibility "
            "metadata");
    }
    if (compatibility.estimated_payload_bytes > 0
        && reconstruction.workset.encoded_size_bytes
            > compatibility.estimated_payload_bytes) {
        return fail(
            "reconstructed workset exceeded its published payload "
            "estimate");
    }
    for (std::size_t index = 0;
         index < claimed.items.size();
         ++index) {
        const auto& durable = claimed.items[index];
        const auto& runtime_item =
            reconstruction.workset.items[index];
        if (durable.item_ordinal < 0
            || (index > 0
                && claimed.items[index - 1].item_ordinal
                    >= durable.item_ordinal)
            || reconstruction.ordered_job_ids[index]
                != durable.job_id
            || runtime_item.ordinal != index
            || durable.dispatch_item_ordinal != index
            || runtime_item.correlation.durable_job_id
                != std::to_string(durable.job_id)
            || runtime_item.correlation.claim_token
                != claimed.claim_token
            || runtime_item.execution.attempt_id.value()
                != durable.reserved_attempt_id) {
            return fail(
                "descriptor changed durable workset membership, "
                "order, or attempt authority");
        }
    }
    savor::runtime::WorkerWorksetLimits limits{};
    limits.maximum_items_per_workset =
        config_.maximum_items_per_workset;
    limits.maximum_encoded_workset_bytes =
        static_cast<std::size_t>(
            config_.maximum_encoded_workset_bytes);
    const auto validation =
        savor::runtime::ValidateWorkerWorksetDefinition(
            reconstruction.workset,
            limits);
    if (!validation.ok) {
        return fail(
            "reconstructed runtime workset is invalid: "
            + validation.error.message);
    }
    if (error_out) error_out->clear();
    return true;
}

bool JobExecutionCoordinator::Impl::IsInvariantSubmissionRejection(
    const WorkerSubmitResult& result) noexcept {
    if (result.disposition
            == WorkerSubmitDisposition::InvalidWorkset
        || result.disposition
            == WorkerSubmitDisposition::DuplicateWorkset
        || result.disposition
            == WorkerSubmitDisposition::IncompatibleWorkset) {
        return true;
    }
    if (result.disposition
        != WorkerSubmitDisposition::DefiniteRejected) {
        return false;
    }
    switch (result.rejection_code) {
    case savor::wrms::RejectionCode::Unsupported:
    case savor::wrms::RejectionCode::InvalidArgument:
    case savor::wrms::RejectionCode::WorksetCatalogMismatch:
        return true;
    default:
        return false;
    }
}

void JobExecutionCoordinator::Impl::WorkerLaneLoop(
    const WorkerLanePtr& lane) {
    while (!stop_.load()) {
        WorkerControlCommand control;
        bool have_control = false;
        DispatchPtr dispatch;
        {
            std::unique_lock lock(lane->mutex);
            lane->cv.wait_for(
                lock,
                std::max(
                    config_.poll_interval,
                    std::chrono::milliseconds(1)),
                [&]() {
                    return stop_.load() || lane->stop
                        || !lane->controls.empty()
                        || lane->retry_submission
                        || !lane->waiting.empty();
                });
            if (stop_.load() || lane->stop) return;
            const auto now = Clock::now();
            if (!lane->controls.empty()) {
                if (lane->controls.front().retry_at <= now) {
                    control = std::move(lane->controls.front());
                    lane->controls.pop_front();
                    have_control = true;
                }
            } else if (!IsDispatchAdmissionPaused()) {
                dispatch = lane->retry_submission;
                if (!dispatch && !lane->waiting.empty()) {
                    dispatch = lane->waiting.front();
                }
            }
        }
        if (have_control) {
            ExecuteWorkerControl(lane, std::move(control));
            continue;
        }
        if (!dispatch) continue;

        savor::runtime::WorkerWorksetDefinition definition;
        std::size_t item_count = 0;
        {
            std::lock_guard lock(dispatch->mutex);
            if (dispatch->phase != DispatchPhase::Waiting
                && dispatch->phase != DispatchPhase::Submitting) {
                std::lock_guard lane_lock(lane->mutex);
                if (lane->retry_submission == dispatch) {
                    lane->retry_submission.reset();
                    lane->submitting.reset();
                } else {
                    const auto found = std::find(
                        lane->waiting.begin(),
                        lane->waiting.end(),
                        dispatch);
                    if (found != lane->waiting.end()) {
                        lane->waiting.erase(found);
                    }
                }
                continue;
            }
            definition = dispatch->definition;
            item_count = definition.items.size();
        }
        bool frozen_retry = false;
        {
            std::lock_guard lane_lock(lane->mutex);
            frozen_retry = lane->retry_submission == dispatch;
        }

        auto snapshot_cancellations = [&]() {
            std::pair<bool, std::vector<
                savor::db::CommittedJobCancellation>> result;
            std::lock_guard lock(cancellation_index_mutex_);
            for (const auto& item : dispatch->claimed.items) {
                if (pending_cancellation_hold_counts_.contains(
                        item.job_id)) {
                    result.first = true;
                }
                const auto cancellation =
                    committed_cancellations_by_job_.find(item.job_id);
                if (cancellation
                    != committed_cancellations_by_job_.end()) {
                    result.second.push_back(
                        cancellation->second.cancellation);
                }
            }
            return result;
        };
        auto cancellation_snapshot = frozen_retry
            ? [&]() {
                std::pair<bool, std::vector<
                    savor::db::CommittedJobCancellation>> result;
                std::lock_guard lock(dispatch->mutex);
                result.second =
                    dispatch->frozen_submission_cancellations;
                return result;
            }()
            : snapshot_cancellations();
        if (cancellation_snapshot.first) continue;
        const auto projected_live_items = item_count
            - std::min(item_count, cancellation_snapshot.second.size());

        bool accepting = false;
        for (const auto& worker :
             worker_coordinator_->SnapshotReadyWorkers()) {
            if (worker.worker_id == lane->worker_id
                && worker.process_generation == lane->generation
                && worker.accepting_workset
                && worker.available_item_credits
                    >= projected_live_items) {
                accepting = true;
                break;
            }
        }
        if (!accepting && projected_live_items != 0) continue;

        bool cancellation_pending_before_submit = false;
        savor::runtime::InitialWorksetCancellationSidecarV1 sidecar{
            .workset_id = definition.workset_id,
        };
        std::vector<savor::db::CommittedJobCancellation>
            sidecar_cancellations;
        {
            std::lock_guard cancellation_lock(cancellation_index_mutex_);
            std::lock_guard record_lock(dispatch->mutex);
            std::lock_guard lane_lock(lane->mutex);
            const bool retry_at_fence =
                lane->retry_submission == dispatch;
            if (retry_at_fence) {
                if (!dispatch->frozen_submission_sidecar.has_value()) {
                    cancellation_pending_before_submit = true;
                } else {
                    sidecar = *dispatch->frozen_submission_sidecar;
                    sidecar_cancellations =
                        dispatch->frozen_submission_cancellations;
                }
            } else {
                for (std::size_t index = 0;
                     index < dispatch->claimed.items.size();
                     ++index) {
                    const auto job_id =
                        dispatch->claimed.items[index].job_id;
                    if (pending_cancellation_hold_counts_.contains(job_id)) {
                        cancellation_pending_before_submit = true;
                        break;
                    }
                    const auto cancellation =
                        committed_cancellations_by_job_.find(job_id);
                    if (cancellation
                        != committed_cancellations_by_job_.end()) {
                        sidecar_cancellations.push_back(
                            cancellation->second.cancellation);
                        if (index < definition.items.size()) {
                            sidecar.item_ids.push_back(
                                definition.items[index].item_id);
                        }
                    }
                }
            }
            std::ranges::sort(sidecar.item_ids, {},
                [](const auto item_id) { return item_id.value(); });
            if (lane->retry_submission != dispatch) {
                const auto found = std::find(
                    lane->waiting.begin(),
                    lane->waiting.end(),
                    dispatch);
                if (found == lane->waiting.end()) continue;
                if (!cancellation_pending_before_submit) {
                    lane->waiting.erase(found);
                }
            }
            if (!cancellation_pending_before_submit) {
                lane->retry_submission.reset();
                lane->submitting =
                    dispatch->claimed.dispatch_attempt_id;
                RemoveProjectedAffinity(
                    *lane,
                    dispatch->claimed.dispatch_attempt_id);
                lane->has_submitted_workset = true;
                dispatch->phase = DispatchPhase::Submitting;
                dispatch->cancellation_sidecar_sha256 =
                    savor::runtime::
                        ComputeInitialWorksetCancellationSidecarSha256(
                            sidecar);
                dispatch->frozen_submission_sidecar = sidecar;
                dispatch->frozen_submission_cancellations =
                    sidecar_cancellations;
                for (const auto& cancellation : sidecar_cancellations) {
                    dispatch->initially_suppressed_jobs.insert(
                        cancellation.job_id);
                }
            }
        }
        if (cancellation_pending_before_submit) {
            continue;
        }
        WakeScheduler("worker-queue-capacity", false);

        if (sidecar.item_ids.size() == definition.items.size()) {
            ++fully_canceled_worksets_avoided_;
            {
                std::lock_guard lane_lock(lane->mutex);
                lane->submitting.reset();
            }
            for (const auto& cancellation : sidecar_cancellations) {
                QueueCancellationMutation({
                    .mutation = savor::db::
                        JobCancellationOutcomeCommand{
                            .kind = savor::db::
                                JobCancellationOutcomeKind::
                                    CancelWithoutWorker,
                            .cancellation_request_id =
                                cancellation.cancellation_request_id,
                            .job_id = cancellation.job_id,
                            .dispatch_attempt_id =
                                dispatch->claimed.dispatch_attempt_id,
                            .claim_token =
                                dispatch->claimed.claim_token,
                            .resolution_code =
                                "CANCELED_BEFORE_WORKER",
                            .requested_by =
                                "job_execution_coordinator",
                        },
                    .requested_resolution_code =
                        "CANCELED_BEFORE_WORKER",
                    .dispatch = dispatch,
                    .initial_suppression = true,
                });
            }
            WakeScheduler("fully-canceled-workset", false);
            continue;
        }

        ++submission_calls_started_;
        sidecar_items_submitted_.fetch_add(sidecar.item_ids.size());
        const auto submitted =
            worker_coordinator_->SubmitWorksetToWorker(
                dispatch->target,
                definition,
                sidecar);
        switch (submitted.disposition) {
        case WorkerSubmitDisposition::Accepted:
            ++submission_accepted_;
            break;
        case WorkerSubmitDisposition::AmbiguousAfterWrite:
            ++submission_ambiguous_after_write_;
            break;
        case WorkerSubmitDisposition::TargetTemporarilyUnavailable:
            ++submission_temporary_unavailable_;
            break;
        case WorkerSubmitDisposition::StaleGeneration:
            ++submission_stale_generation_;
            break;
        case WorkerSubmitDisposition::IncompatibleWorkset:
            ++submission_incompatible_;
            break;
        case WorkerSubmitDisposition::CoordinatorNotAccepting:
            ++submission_transport_canceled_before_write_;
            break;
        case WorkerSubmitDisposition::DefiniteRejected:
            if (submitted.error_code == "WorksetSubmissionNotWritten") {
                ++submission_transport_canceled_before_write_;
            } else {
                ++submission_deterministic_rejection_;
            }
            break;
        case WorkerSubmitDisposition::DuplicateWorkset:
        case WorkerSubmitDisposition::InvalidWorkset:
            ++submission_deterministic_rejection_;
            break;
        }
        if (submitted.disposition
            == WorkerSubmitDisposition::AmbiguousAfterWrite) {
            std::lock_guard lane_lock(lane->mutex);
            lane->retry_submission = dispatch;
            lane->cv.notify_all();
            continue;
        }
        const bool retryable_submission = submitted.disposition
                == WorkerSubmitDisposition::
                    TargetTemporarilyUnavailable
            || submitted.disposition
                == WorkerSubmitDisposition::
                    CoordinatorNotAccepting;
        bool submission_exit_pending = false;
        {
            std::lock_guard lock(dispatch->mutex);
            submission_exit_pending =
                dispatch->deferred_release.has_value()
                || !dispatch->pending_submission_cancellations.empty();
        }
        if (retryable_submission && !submission_exit_pending) {
            std::lock_guard lane_lock(lane->mutex);
            lane->retry_submission = dispatch;
            lane->cv.notify_all();
            continue;
        }
        if (!submitted.submitted()) {
            const bool invariant_rejection =
                IsInvariantSubmissionRejection(submitted);
            std::optional<DeferredDispatchRelease> deferred_release;
            std::deque<PendingSubmissionCancellation>
                pending_cancellations;
            {
                std::lock_guard record_lock(dispatch->mutex);
                if (dispatch->phase == DispatchPhase::Submitting) {
                    dispatch->phase = DispatchPhase::Waiting;
                }
                if (!invariant_rejection) {
                    deferred_release =
                        std::move(dispatch->deferred_release);
                    dispatch->deferred_release.reset();
                    pending_cancellations.swap(
                        dispatch->pending_submission_cancellations);
                    dispatch->frozen_submission_sidecar.reset();
                    dispatch->frozen_submission_cancellations.clear();
                    dispatch->cancellation_sidecar_sha256.clear();
                    dispatch->initially_suppressed_jobs.clear();
                }
            }
            {
                std::lock_guard lane_lock(lane->mutex);
                lane->submitting.reset();
                lane->retry_submission.reset();
            }
            std::string diagnostic = submitted.diagnostic.empty()
                ? "worker rejected reconstructed workset"
                : submitted.diagnostic;
            if (!submitted.error_code.empty()) {
                diagnostic += " [error_code="
                    + submitted.error_code + "]";
            }
            if (invariant_rejection) {
                if (submitted.disposition
                    == WorkerSubmitDisposition::IncompatibleWorkset) {
                    worker_coordinator_->QuarantineWorkerGeneration(
                        dispatch->target,
                        "homogeneous pool admission invariant failed: "
                            + diagnostic);
                }
                PauseForInvariant(
                    dispatch,
                    "Worker rejected a locally validated workset "
                    "contract",
                    diagnostic);
            } else if (deferred_release.has_value()) {
                EnqueueAuthority(
                    [this,
                     dispatch,
                     deferred_release = std::move(deferred_release)]() mutable {
                        (void)ReleaseDispatch(
                            dispatch,
                            std::move(deferred_release->reason_code),
                            std::move(deferred_release->reason_text),
                            deferred_release->keep_draining);
                    });
            } else {
                // Nothing crossed the transport. Keep the exact durable claim
                // and reconstruction, then rebuild a fresh sidecar from the
                // committed index on the next submission attempt.
                std::lock_guard lane_lock(lane->mutex);
                lane->waiting.push_front(dispatch);
                lane->projected_affinities.insert_or_assign(
                    dispatch->claimed.dispatch_attempt_id,
                    AffinityOf(dispatch->claimed.compatibility));
                lane->cv.notify_all();
            }
            continue;
        }

        const bool sidecar_receipt_valid =
            submitted.submission_receipt.has_value()
            && submitted.submission_receipt->workset_id
                == definition.workset_id
            && submitted.submission_receipt->sidecar_version
                == savor::runtime::
                    kInitialWorksetCancellationSidecarVersionV1
            && submitted.submission_receipt->applied_item_count
                == sidecar.item_ids.size()
            && submitted.submission_receipt
                    ->applied_sidecar_sha256
                == dispatch->cancellation_sidecar_sha256;
        if (!sidecar_receipt_valid) {
            ++sidecar_submit_receipts_mismatched_;
            PauseForInvariant(
                dispatch,
                "Worker accepted a workset without the frozen cancellation sidecar receipt",
                "dispatch_attempt_id=" + std::to_string(
                    dispatch->claimed.dispatch_attempt_id));
        } else if (submitted.submission_receipt->disposition
            == savor::runtime::WorksetSubmissionDispositionV1::
                AlreadyAccepted) {
            ++sidecar_submit_receipts_repeated_;
        } else {
            ++sidecar_submit_receipts_accepted_;
        }
        if (sidecar_receipt_valid) {
            bool first_receipt = false;
            {
                std::lock_guard lock(dispatch->mutex);
                first_receipt = !dispatch->sidecar_receipt_accounted;
                dispatch->sidecar_receipt_accounted = true;
            }
            if (first_receipt) {
                waiting_jobs_suppressed_by_sidecar_.fetch_add(
                    sidecar.item_ids.size());
            }
        }

        {
            std::lock_guard lock(dispatch->mutex);
            dispatch->submitted = true;
        }

        bool marked = false;
        std::optional<std::int64_t> active_lease_expiry;
        while (!stop_.load() && !marked) {
            savor::db::WorksetDispatchMutationReceipt receipt{};
            std::string error;
            const bool called =
                execution_db_->MarkWorksetActive(
                    {
                        .dispatch_attempt_id =
                            dispatch->claimed.dispatch_attempt_id,
                        .claim_token =
                            dispatch->claimed.claim_token,
                        .lease_duration_ms =
                            config_.workset_lease_duration.count(),
                        .requested_by =
                            "job_execution_coordinator",
                    },
                    &receipt,
                    &error);
            if (called && Applied(receipt.disposition)) {
                active_lease_expiry = receipt.lease_expires_at_utc;
                marked = true;
                break;
            }
            if (receipt.disposition
                == savor::db::ExecutionDbOperationDisposition::
                    BackendError) {
                RecordError(
                    error.empty()
                        ? "failed marking submitted workset dispatched"
                        : std::move(error));
                std::unique_lock lock(lane->mutex);
                lane->cv.wait_for(
                    lock,
                    std::max(
                        config_.poll_interval,
                        std::chrono::milliseconds(1)));
                continue;
            }
            PauseForInvariant(
                dispatch,
                "Submitted workset lost durable authority",
                error.empty()
                    ? "dispatch_attempt_id="
                        + std::to_string(
                            dispatch->claimed.dispatch_attempt_id)
                    : std::move(error));
            break;
        }
        if (!marked) {
            std::lock_guard lock(lane->mutex);
            lane->submitting.reset();
            continue;
        }

        std::optional<DeferredDispatchRelease> deferred_release;
        std::deque<PendingSubmissionCancellation>
            pending_cancellations;
        bool draining_on_mark = false;
        {
            std::lock_guard record_lock(dispatch->mutex);
            dispatch->dispatch_marked = true;
            draining_on_mark =
                dispatch->terminal_workset_state_observed;
            dispatch->phase = draining_on_mark
                ? DispatchPhase::Draining
                : DispatchPhase::Active;
            if (draining_on_mark) {
                dispatch->draining_retry_at = Clock::now();
            }
            deferred_release = std::move(dispatch->deferred_release);
            dispatch->deferred_release.reset();
            pending_cancellations.swap(
                dispatch->pending_submission_cancellations);
        }
        {
            std::lock_guard lane_lock(lane->mutex);
            lane->submitting.reset();
            if (!draining_on_mark) {
                lane->active =
                    dispatch->claimed.dispatch_attempt_id;
            }
        }
        if (!draining_on_mark) {
            const auto fallback_expiry =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()
                + config_.workset_lease_duration.count();
            RegisterLeaseHeartbeat(
                dispatch,
                active_lease_expiry.value_or(fallback_expiry));
        } else {
            EnqueueAuthority(
                [this, dispatch]() {
                    (void)MarkDispatchDraining(dispatch);
                });
        }
        ++worksets_submitted_;
        if (sidecar_receipt_valid) {
            for (const auto& cancellation : sidecar_cancellations) {
                QueueCancellationMutation({
                    .mutation = savor::db::
                        JobCancellationOutcomeCommand{
                            .kind = savor::db::
                                JobCancellationOutcomeKind::
                                    InitialSidecarApplied,
                            .cancellation_request_id =
                                cancellation.cancellation_request_id,
                            .job_id = cancellation.job_id,
                            .dispatch_attempt_id =
                                dispatch->claimed.dispatch_attempt_id,
                            .claim_token =
                                dispatch->claimed.claim_token,
                            .resolution_code =
                                "INITIAL_SIDECAR_APPLIED",
                            .requested_by =
                                "job_execution_coordinator",
                        },
                    .requested_resolution_code =
                        "INITIAL_SIDECAR_APPLIED",
                    .dispatch = dispatch,
                    .initial_suppression = true,
                });
            }
        }
        persistence_cv_.notify_all();
        WakeScheduler("dispatch-marked", false);
        for (auto& pending : pending_cancellations) {
            if (!pending.item_id.has_value()) {
                std::lock_guard lock(dispatch->mutex);
                for (const auto& [runtime_item_id, item_index] :
                     dispatch->item_index_by_id) {
                    if (item_index < dispatch->claimed.items.size()
                        && dispatch->claimed.items[item_index].job_id
                            == pending.cancellation.job_id) {
                        pending.item_id =
                            savor::runtime::WorkerWorksetItemId{
                                runtime_item_id};
                        break;
                    }
                }
            }
            if (!pending.item_id.has_value()) {
                PauseForInvariant(
                    dispatch,
                    "Cancellation route invariant failed after dispatch",
                    "dispatch_attempt_id="
                        + std::to_string(
                            dispatch->claimed.dispatch_attempt_id)
                        + " job_id="
                        + std::to_string(pending.cancellation.job_id)
                        + " missing reconstructed workset item",
                    pending.cancellation.job_id);
                continue;
            }
            EnqueueWorkerControl(
                dispatch->target,
                {
                    .kind = WorkerControlKind::CancelItem,
                    .dispatch_attempt_id =
                        dispatch->claimed.dispatch_attempt_id,
                    .job_id = pending.cancellation.job_id,
                    .item_id = pending.item_id,
                    .reason = pending.cancellation.reason_text.value_or(
                        pending.cancellation.reason_code),
                    .cancellation = pending.cancellation,
                    .enqueued_at = pending.enqueued_at,
                });
        }
        if (deferred_release.has_value()) {
            EnqueueAuthority(
                [this,
                 dispatch,
                 deferred_release = std::move(deferred_release)]() mutable {
                    (void)ReleaseDispatch(
                        dispatch,
                        std::move(deferred_release->reason_code),
                        std::move(deferred_release->reason_text),
                        deferred_release->keep_draining);
                });
        }
    }
}

void JobExecutionCoordinator::Impl::EnqueueWorkerControl(
    const WorkerExecutionTarget& target,
    WorkerControlCommand command) {
    if (command.enqueued_at == Clock::time_point{}) {
        command.enqueued_at = Clock::now();
    }
    const auto lane =
        FindWorkerLane(target.worker_id, target.process_generation);
    if (!lane) {
        ++worker_control_commands_abandoned_;
        if (command.kind == WorkerControlKind::Acknowledge) {
            ++worker_terminal_ack_abandoned_generation_loss_;
        }
        if (command.cancellation.has_value()) {
            EnqueueAuthority(
                [this,
                 cancellation = *command.cancellation,
                 enqueued_at = command.enqueued_at]() {
                    WorkerCommandResult stale{
                        .disposition =
                            WorkerCommandDisposition::StaleRoute,
                        .diagnostic =
                            "worker lane no longer exists",
                    };
                    CompleteCancellationDelivery(
                        cancellation,
                        stale,
                        false,
                        enqueued_at);
                });
        }
        return;
    }
    {
        std::lock_guard lock(lane->mutex);
        lane->controls.push_back(std::move(command));
        StoreMaximum(
            worker_control_queue_high_water_,
            static_cast<std::uint64_t>(lane->controls.size()));
    }
    ++worker_control_commands_queued_;
    lane->cv.notify_all();
}

void JobExecutionCoordinator::Impl::ExecuteWorkerControl(
    const WorkerLanePtr& lane,
    WorkerControlCommand command) {
    ++worker_control_commands_attempted_;
    if (command.kind != WorkerControlKind::Acknowledge) {
        const auto dispatch =
            FindDispatch(command.dispatch_attempt_id);
        bool workset_cancellation_applied = false;
        if (dispatch) {
            std::lock_guard lock(dispatch->mutex);
            workset_cancellation_applied =
                dispatch->workset_cancellation_applied;
        }
        if (workset_cancellation_applied) {
            if (command.cancellation.has_value()) {
                const auto cancellation = *command.cancellation;
                const auto enqueued_at = command.enqueued_at;
                EnqueueAuthority(
                    [this, cancellation, enqueued_at]() {
                        const auto latency =
                            static_cast<std::uint64_t>(
                                std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    Clock::now() - enqueued_at)
                                    .count());
                        cancellation_delivery_total_latency_ms_
                            .fetch_add(latency);
                        StoreMaximum(
                            cancellation_delivery_max_latency_ms_,
                            latency);
                        if (ResolveClaimedCancellation(
                                cancellation,
                                "WORKSET_CANCELLATION_ALREADY_APPLIED",
                                false)) {
                            ++worker_control_commands_applied_;
                            ++cancellations_already_applied_;
                        } else {
                            ++worker_control_commands_failed_;
                        }
                    });
            } else {
                ++worker_control_commands_applied_;
            }
            return;
        }
    }
    WorkerCommandResult result{};
    if (command.kind == WorkerControlKind::Acknowledge) {
        result =
            worker_coordinator_->AcknowledgeTerminal(command.terminal);
        if (result.accepted()) {
            ++worker_control_commands_applied_;
            const auto dispatch =
                FindDispatch(command.dispatch_attempt_id);
            if (dispatch) {
                {
                    std::lock_guard lock(dispatch->mutex);
                    dispatch->acknowledged_jobs.emplace(command.job_id);
                }
                ++worker_terminal_acks_;
                MaybeRetire(dispatch);
            }
            return;
        }
    } else if (command.kind == WorkerControlKind::CancelItem
        && command.item_id.has_value()) {
        result = worker_coordinator_->CancelWorksetItem(
            savor::runtime::WorkerWorksetId{
                static_cast<std::uint64_t>(
                    command.dispatch_attempt_id)},
            *command.item_id,
            command.reason);
    } else {
        result = worker_coordinator_->CancelWorkset(
            savor::runtime::WorkerWorksetId{
                static_cast<std::uint64_t>(
                    command.dispatch_attempt_id)},
            command.reason);
    }

    if (command.kind == WorkerControlKind::CancelWorkset
        && result.accepted()) {
        const auto dispatch =
            FindDispatch(command.dispatch_attempt_id);
        if (dispatch) {
            std::lock_guard lock(dispatch->mutex);
            dispatch->workset_cancellation_applied = true;
        }
    }

    if (command.cancellation.has_value()) {
        EnqueueAuthority(
            [this,
             cancellation = *command.cancellation,
             result,
             enqueued_at = command.enqueued_at,
             whole =
                 command.kind == WorkerControlKind::CancelWorkset]() {
                CompleteCancellationDelivery(
                    cancellation,
                    result,
                    whole,
                    enqueued_at);
            });
        return;
    }
    if (result.accepted()) {
        ++worker_control_commands_applied_;
    } else {
        ++worker_control_commands_failed_;
    }
    if (!result.accepted()
        && command.kind == WorkerControlKind::Acknowledge
        && !stop_.load()) {
        command.retry_at =
            Clock::now() + config_.terminal_retry_interval;
        {
            std::lock_guard lock(lane->mutex);
            lane->controls.push_back(std::move(command));
        }
        lane->cv.notify_all();
    }
}

void JobExecutionCoordinator::Impl::EnqueuePersistence(
    PersistenceEvent event) {
    if (stop_.load()) return;
    {
        std::lock_guard lock(persistence_mutex_);
        auto& stream = persistence_streams_[
            WorkerStreamKey(event.worker_id, event.generation)];
        stream.events.push_back(std::move(event));
        std::size_t depth = 0;
        for (const auto& [key, candidate] : persistence_streams_) {
            (void)key;
            depth += candidate.events.size();
        }
        persistence_high_water_ =
            std::max(persistence_high_water_, depth);
    }
    persistence_cv_.notify_all();
}

bool JobExecutionCoordinator::Impl::PersistPreparedWorkerEvent(
    savor::db::WorkerExecutionEventMutation mutation,
    savor::db::WorkerExecutionEventMutationReceipt* receipt_out,
    std::string* error_out) {
    auto pending = std::make_shared<PreparedWorkerEventMutation>();
    pending->mutation = std::move(mutation);
    pending->enqueued_at = Clock::now();
    {
        std::lock_guard lock(worker_event_batch_mutex_);
        worker_event_batch_queue_.push_back(pending);
        worker_event_batch_high_water_ = std::max(
            worker_event_batch_high_water_,
            worker_event_batch_queue_.size());
    }
    worker_event_batch_cv_.notify_one();

    std::unique_lock lock(pending->mutex);
    pending->cv.wait(lock, [this, &pending]() {
        return pending->completed || stop_.load();
    });
    if (!pending->completed) {
        if (error_out != nullptr) *error_out = "worker-event batcher stopped";
        return false;
    }
    if (receipt_out != nullptr) *receipt_out = pending->receipt;
    if (error_out != nullptr) *error_out = pending->error;
    return pending->succeeded;
}

void JobExecutionCoordinator::Impl::WorkerEventBatchLoop() {
    for (;;) {
        std::vector<PreparedWorkerEventMutationPtr> pending;
        bool full_flush = false;
        bool deadline_flush = false;
        bool barrier_flush = false;
        {
            std::unique_lock lock(worker_event_batch_mutex_);
            worker_event_batch_cv_.wait(lock, [this]() {
                return stop_.load() || !worker_event_batch_queue_.empty();
            });
            if (stop_.load() && worker_event_batch_queue_.empty()) return;

            const auto deadline = worker_event_batch_queue_.front()->enqueued_at
                + config_.worker_event_collection_delay;
            while (!stop_.load()
                && worker_event_batch_queue_.size()
                    < config_.worker_event_batch_size
                && config_.worker_event_collection_delay
                    > std::chrono::milliseconds::zero()) {
                if (worker_event_batch_cv_.wait_until(
                        lock,
                        deadline,
                        [this]() {
                            return stop_.load()
                                || worker_event_batch_queue_.size()
                                    >= config_.worker_event_batch_size;
                        })) {
                    break;
                }
                break;
            }
            full_flush = worker_event_batch_queue_.size()
                >= config_.worker_event_batch_size;
            deadline_flush = !full_flush && !stop_.load()
                && Clock::now() >= deadline;
            barrier_flush = stop_.load();
            const auto count = std::min(
                config_.worker_event_batch_size,
                worker_event_batch_queue_.size());
            pending.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                pending.push_back(
                    std::move(worker_event_batch_queue_.front()));
                worker_event_batch_queue_.pop_front();
            }
        }
        if (pending.empty()) continue;
        if (full_flush) ++worker_event_full_flushes_;
        else if (deadline_flush) ++worker_event_deadline_flushes_;
        else if (barrier_flush) ++worker_event_barrier_flushes_;

        savor::db::PersistWorkerExecutionEventsBatchCommand command;
        command.events.reserve(pending.size());
        for (const auto& item : pending) {
            command.events.push_back(item->mutation);
        }
        savor::db::PersistWorkerExecutionEventsBatchReceipt receipt;
        std::string error;
        const bool succeeded =
            execution_db_->PersistWorkerExecutionEventsBatch(
                command,
                &receipt,
                &error)
            && receipt.events.size() == pending.size();

        ++worker_event_batches_;
        worker_event_batch_items_.fetch_add(pending.size());
        worker_event_batch_total_size_.fetch_add(pending.size());
        StoreMaximum(worker_event_batch_max_size_, pending.size());
        const auto oldest_age = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            Clock::now() - pending.front()->enqueued_at).count();
        StoreMaximum(
            worker_event_batch_max_collection_age_ms_,
            static_cast<std::uint64_t>(std::max<std::int64_t>(0, oldest_age)));
        if (!succeeded) {
            ++worker_event_batch_rollbacks_;
            worker_event_batch_retries_.fetch_add(pending.size());
        }

        for (std::size_t index = 0; index < pending.size(); ++index) {
            auto& item = pending[index];
            {
                std::lock_guard lock(item->mutex);
                item->succeeded = succeeded;
                item->error = error;
                if (succeeded) item->receipt = std::move(receipt.events[index]);
                item->completed = true;
            }
            item->cv.notify_one();
        }
    }
}

void JobExecutionCoordinator::Impl::PersistenceLoop() {
    while (!stop_.load()) {
        std::string selected_key;
        PersistenceEvent event;
        {
            std::unique_lock lock(persistence_mutex_);
            persistence_cv_.wait_for(
                lock,
                std::max(
                    config_.poll_interval,
                    std::chrono::milliseconds(1)),
                [this]() {
                    if (stop_.load()) return true;
                    const auto now = Clock::now();
                    return std::any_of(
                        persistence_streams_.begin(),
                        persistence_streams_.end(),
                        [now](const auto& entry) {
                            if (entry.second.active
                                || entry.second.events.empty()) {
                                return false;
                            }
                            const auto* terminal =
                                std::get_if<TerminalEvent>(
                                    &entry.second.events.front().payload);
                            return terminal == nullptr
                                || terminal->retry_at <= now;
                        });
                });
            if (stop_.load()) return;
            const auto found = std::find_if(
                persistence_streams_.begin(),
                persistence_streams_.end(),
                [now = Clock::now()](const auto& entry) {
                    if (entry.second.active
                        || entry.second.events.empty()) {
                        return false;
                    }
                    const auto* terminal = std::get_if<TerminalEvent>(
                        &entry.second.events.front().payload);
                    return terminal == nullptr
                        || terminal->retry_at <= now;
                });
            if (found == persistence_streams_.end()) continue;
            selected_key = found->first;
            found->second.active = true;
            event = found->second.events.front();
        }

        const bool consumed = ProcessPersistenceEvent(event);
        bool retry_pending = false;
        {
            std::lock_guard lock(persistence_mutex_);
            const auto found = persistence_streams_.find(selected_key);
            if (found != persistence_streams_.end()) {
                if (consumed && !found->second.events.empty()) {
                    found->second.events.pop_front();
                } else if (!consumed && !found->second.events.empty()) {
                    found->second.events.front() = event;
                }
                found->second.active = false;
                if (found->second.events.empty()) {
                    persistence_streams_.erase(found);
                }
            }
            retry_pending = std::any_of(
                persistence_streams_.begin(),
                persistence_streams_.end(),
                [](const auto& entry) {
                    return std::any_of(
                        entry.second.events.begin(),
                        entry.second.events.end(),
                        [](const PersistenceEvent& candidate) {
                            const auto* terminal =
                                std::get_if<TerminalEvent>(
                                    &candidate.payload);
                            return terminal != nullptr
                                && terminal->retry_attempt != 0;
                        });
                });
        }
        if (consumed && !retry_pending && blob_store_ready_.load()) {
            storage_paused_.store(false);
        }
        if (!consumed) {
            std::unique_lock lock(persistence_mutex_);
            persistence_cv_.wait_for(
                lock,
                std::max(
                    config_.poll_interval,
                    std::chrono::milliseconds(1)));
        } else {
            persistence_cv_.notify_all();
        }
    }
}

bool JobExecutionCoordinator::Impl::ProcessPersistenceEvent(
    PersistenceEvent& event) {
    return std::visit(
        [this](auto& payload) -> bool {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, ItemStartedEvent>) {
                return HandleItemStarted(payload);
            } else if constexpr (std::is_same_v<T, TerminalEvent>) {
                return HandleTerminal(payload);
            } else {
                return HandleWorksetSummary(payload);
            }
        },
        event.payload);
}

bool JobExecutionCoordinator::Impl::HandleItemStarted(
    const ItemStartedEvent& event) {
    const auto dispatch_id =
        static_cast<std::int64_t>(event.payload.workset_id);
    const auto dispatch = FindDispatch(dispatch_id);
    if (!dispatch) {
        RecordWarning(
            "Ignored ItemStarted for unknown workset",
            "dispatch_attempt_id=" + std::to_string(dispatch_id),
            static_cast<std::int64_t>(event.source.worker_id));
        return true;
    }

    savor::db::ClaimedPublishedWorkset claimed;
    savor::db::ClaimedPublishedWorksetItem durable_item;
    savor::runtime::WorksetItemTemplate runtime_item;
    {
        std::lock_guard lock(dispatch->mutex);
        if (dispatch->submitted && !dispatch->dispatch_marked) {
            return false;
        }
        const auto item_found =
            dispatch->item_index_by_id.find(event.payload.item_id);
        if (!dispatch->submitted
            || item_found == dispatch->item_index_by_id.end()
            || dispatch->target.worker_id != event.source.worker_id
            || dispatch->target.process_generation
                != event.source.process_generation) {
            if (dispatch->phase == DispatchPhase::ReleasedDraining) {
                return true;
            }
            PauseForInvariant(
                dispatch,
                "Worker ItemStarted correlation mismatch",
                "dispatch_attempt_id=" + std::to_string(dispatch_id));
            return true;
        }
        const auto index = item_found->second;
        runtime_item = dispatch->definition.items[index];
        durable_item = dispatch->claimed.items[index];
        claimed = dispatch->claimed;
        if (event.payload.item_ordinal != runtime_item.ordinal
            || event.payload.invocation_id
                != runtime_item.execution.execution_id.value()
            || event.payload.attempt_id
                != runtime_item.execution.attempt_id.value()) {
            if (dispatch->phase == DispatchPhase::ReleasedDraining) {
                return true;
            }
            PauseForInvariant(
                dispatch,
                "Worker ItemStarted identity mismatch",
                "job_id=" + std::to_string(durable_item.job_id));
            return true;
        }
        if (dispatch->phase == DispatchPhase::ReleasedDraining) {
            return true;
        }
    }

    savor::db::WorksetJobStartReceipt receipt{};
    savor::db::WorkerExecutionEventMutationReceipt batch_receipt{};
    std::string error;
    const bool accepted =
        PersistPreparedWorkerEvent(
            savor::db::MarkWorksetJobStartedCommand{
                .dispatch_attempt_id = dispatch_id,
                .claim_token = claimed.claim_token,
                .job_id = durable_item.job_id,
                .dispatch_item_ordinal =
                    event.payload.item_ordinal,
                .reserved_attempt_id =
                    durable_item.reserved_attempt_id,
                .worker_invocation_id =
                    std::to_string(event.payload.invocation_id),
                .requested_by = "job_execution_coordinator",
            },
            &batch_receipt,
            &error);
    if (accepted) {
        if (const auto* typed =
                std::get_if<savor::db::WorksetJobStartReceipt>(
                    &batch_receipt)) {
            receipt = *typed;
        } else {
            error = "worker-event batch returned the wrong start receipt";
        }
    }
    if (!accepted || !Applied(receipt.disposition)) {
        bool authority_released = false;
        {
            std::lock_guard lock(dispatch->mutex);
            authority_released =
                dispatch->phase == DispatchPhase::Released
                || dispatch->phase == DispatchPhase::ReleasedDraining
                || dispatch->phase == DispatchPhase::Retired;
        }
        if (authority_released) return true;
        if (receipt.disposition
            == savor::db::ExecutionDbOperationDisposition::
                BackendError) {
            RecordError(
                error.empty()
                    ? "failed accepting worker ItemStarted"
                    : std::move(error));
            return false;
        } else {
            PauseForInvariant(
                dispatch,
                "Failed accepting worker ItemStarted",
                error.empty()
                    ? "durable start authority rejected"
                    : std::move(error));
        }
        return true;
    }
    if (receipt.disposition
        == savor::db::ExecutionDbOperationDisposition::Applied) {
        ++jobs_started_;
    }
    return true;
}

bool JobExecutionCoordinator::Impl::HandleTerminal(
    TerminalEvent& event) {
    if (event.retry_at > Clock::now()) return false;
    const auto& envelope = event.envelope;
    const auto& payload = envelope.terminal;
    const auto dispatch_id =
        static_cast<std::int64_t>(payload.workset_id);
    const auto dispatch = FindDispatch(dispatch_id);
    if (!dispatch) {
        RecordWarning(
            "Ignored terminal for unknown workset",
            "dispatch_attempt_id=" + std::to_string(dispatch_id),
            static_cast<std::int64_t>(envelope.worker_id));
        return true;
    }

    savor::db::ClaimedPublishedWorkset claimed;
    savor::db::ClaimedPublishedWorksetItem durable_item;
    savor::runtime::WorksetItemTemplate runtime_item;
    savor::runtime::WorkerItemTerminalCorrelation terminal;
    bool authority_released = false;
    bool already_staged = false;
    {
        std::lock_guard lock(dispatch->mutex);
        if (dispatch->submitted && !dispatch->dispatch_marked) {
            return false;
        }
        const auto item_found =
            dispatch->item_index_by_id.find(payload.item_id);
        if (!dispatch->submitted
            || item_found == dispatch->item_index_by_id.end()
            || dispatch->target.worker_id != envelope.worker_id
            || dispatch->target.process_generation
                != envelope.process_generation) {
            if (dispatch->phase == DispatchPhase::ReleasedDraining) {
                ++worker_terminals_discarded_after_authority_release_;
                return true;
            }
            PauseForInvariant(
                dispatch,
                "Worker terminal route mismatch",
                "dispatch_attempt_id=" + std::to_string(dispatch_id));
            return true;
        }
        const auto index = item_found->second;
        runtime_item = dispatch->definition.items[index];
        durable_item = dispatch->claimed.items[index];
        claimed = dispatch->claimed;
        if (payload.item_ordinal != runtime_item.ordinal
            || payload.invocation_id
                != runtime_item.execution.execution_id.value()
            || payload.attempt_id
                != runtime_item.execution.attempt_id.value()
            || payload.terminal_id == 0
            || payload.terminal_order == 0) {
            if (dispatch->phase == DispatchPhase::ReleasedDraining) {
                ++worker_terminals_discarded_after_authority_release_;
                return true;
            }
            PauseForInvariant(
                dispatch,
                "Worker terminal identity mismatch",
                "job_id=" + std::to_string(durable_item.job_id));
            return true;
        }
        terminal =
            {
                .workset_id =
                    savor::runtime::WorkerWorksetId{
                        payload.workset_id},
                .item_id =
                    savor::runtime::WorkerWorksetItemId{
                        payload.item_id},
                .item_ordinal = payload.item_ordinal,
                .invocation_id =
                    savor::runtime::InvocationId{
                        payload.invocation_id},
                .attempt_id =
                    savor::runtime::AttemptId{
                        payload.attempt_id},
                .terminal_id =
                    savor::runtime::WorkerTerminalId{
                        payload.terminal_id},
                .terminal_order =
                    savor::runtime::WorkerTerminalOrder{
                        payload.terminal_order},
            };
        const auto known =
            dispatch->terminal_by_job.find(durable_item.job_id);
        if (known != dispatch->terminal_by_job.end()
            && known->second != terminal) {
            if (dispatch->phase == DispatchPhase::ReleasedDraining) {
                ++worker_terminals_discarded_after_authority_release_;
                return true;
            }
            PauseForInvariant(
                dispatch,
                "Worker terminal changed identity",
                "job_id=" + std::to_string(durable_item.job_id));
            return true;
        }
        dispatch->terminal_by_job.emplace(
            durable_item.job_id,
            terminal);
        authority_released =
            dispatch->phase == DispatchPhase::ReleasedDraining;
        already_staged =
            dispatch->staged_jobs.contains(durable_item.job_id);
    }

    if (authority_released) {
        ++worker_terminals_discarded_after_authority_release_;
        EnqueueWorkerControl(
            dispatch->target,
            {
                .kind = WorkerControlKind::Acknowledge,
                .dispatch_attempt_id = dispatch_id,
                .job_id = durable_item.job_id,
                .terminal = terminal,
            });
        return true;
    }

    if (already_staged) {
        EnqueueWorkerControl(
            dispatch->target,
            {
                .kind = WorkerControlKind::Acknowledge,
                .dispatch_attempt_id = dispatch_id,
                .job_id = durable_item.job_id,
                .terminal = terminal,
            });
        return true;
    }

    std::vector<std::uint8_t> envelope_bytes;
    std::string error;
    if (!savor::runtime::EncodeDurableWorkerTerminalEnvelope(
            envelope,
            &envelope_bytes,
            &error)) {
        PauseForInvariant(
            dispatch,
            "Worker terminal envelope encoding failed",
            std::move(error));
        return true;
    }

    auto prepared = event.prepared;
    if (!prepared) {
        savor::db::execution::WorkerResultBlobReference blob{};
        if (!blob_store_->Stage(
                {
                    .workset_id = claimed.workset_id,
                    .dispatch_attempt_id = dispatch_id,
                    .job_id = durable_item.job_id,
                    .terminal_id = payload.terminal_id,
                    .envelope = envelope_bytes,
                },
                &blob,
                &error)) {
            ++worker_terminal_staging_failures_;
            blob_store_ready_.store(false);
            RecordError(
                error.empty()
                    ? "failed staging private worker terminal blob"
                    : std::move(error));
            ++event.retry_attempt;
            event.retry_at = Clock::now()
                + CappedExponentialDelay(
                    config_.terminal_retry_interval,
                    config_.terminal_retry_max_interval,
                    event.retry_attempt);
            storage_paused_.store(true);
            WakeScheduler("storage-pause", false);
            return false;
        }
        prepared = std::make_shared<PreparedTerminalPersistence>();
        prepared->blob = std::move(blob);
        prepared->command =
            savor::db::StageWorkerTerminalCommand{
                .dispatch_attempt_id = dispatch_id,
                .claim_token = claimed.claim_token,
                .job_id = durable_item.job_id,
                .dispatch_item_ordinal = payload.item_ordinal,
                .reserved_attempt_id = durable_item.reserved_attempt_id,
                .terminal_status = TerminalStatusName(payload.status),
                .terminal_fingerprint = prepared->blob.sha256,
                .terminal_id = std::to_string(payload.terminal_id),
                .error_code = payload.error_code.empty()
                    ? std::nullopt
                    : std::optional<std::string>(payload.error_code),
                .error_text = payload.message.empty()
                    ? std::nullopt
                    : std::optional<std::string>(payload.message),
                .unstarted = payload.unstarted,
                .result_blob = {
                    .relative_path = prepared->blob.relative_path,
                    .sha256 = prepared->blob.sha256,
                    .size_bytes = static_cast<std::uint64_t>(
                        prepared->blob.size_bytes),
                    .format = prepared->blob.format,
                },
                .requested_by = "job_execution_coordinator",
            };
    }
    const auto& blob = prepared->blob;

    savor::db::StageWorkerTerminalReceipt receipt{};
    savor::db::WorkerExecutionEventMutationReceipt batch_receipt{};
    const bool persisted = PersistPreparedWorkerEvent(
            prepared->command,
            &batch_receipt,
            &error);
    if (persisted) {
        if (const auto* typed =
                std::get_if<savor::db::StageWorkerTerminalReceipt>(
                    &batch_receipt)) {
            receipt = *typed;
        } else {
            error = "worker-event batch returned the wrong terminal receipt";
        }
    }
    if (!persisted || !Applied(receipt.disposition)) {
        if (receipt.disposition
            == savor::db::ExecutionDbOperationDisposition::
                BackendError) {
            ++worker_terminal_staging_failures_;
            RecordError(
                error.empty()
                    ? "failed staging worker terminal in DB"
                    : std::move(error));
            event.prepared = prepared;
            ++event.retry_attempt;
            event.retry_at = Clock::now()
                + CappedExponentialDelay(
                    config_.terminal_retry_interval,
                    config_.terminal_retry_max_interval,
                    event.retry_attempt);
            storage_paused_.store(true);
            WakeScheduler("storage-pause", false);
            return false;
        } else {
            if (prepared->pinned) {
                blob_store_->Unpin(blob.relative_path);
                prepared->pinned = false;
            }
            std::string remove_error;
            if (!blob_store_->Remove(
                    blob.relative_path,
                    &remove_error)) {
                RecordWarning(
                    "Failed removing unreferenced worker terminal blob",
                    std::move(remove_error),
                    static_cast<std::int64_t>(envelope.worker_id),
                    durable_item.job_id);
            }
            bool authority_released = false;
            {
                std::lock_guard lock(dispatch->mutex);
                authority_released =
                    dispatch->phase == DispatchPhase::Released
                    || dispatch->phase
                        == DispatchPhase::ReleasedDraining
                    || dispatch->phase == DispatchPhase::Retired;
            }
            if (authority_released) {
                ++worker_terminals_discarded_after_authority_release_;
                EnqueueWorkerControl(
                    dispatch->target,
                    {
                        .kind =
                            WorkerControlKind::Acknowledge,
                        .dispatch_attempt_id = dispatch_id,
                        .job_id = durable_item.job_id,
                        .terminal = terminal,
                    });
            } else {
                std::ostringstream detail;
                detail << "disposition="
                       << ExecutionDbDispositionName(
                              receipt.disposition)
                       << " dispatch_attempt_id=" << dispatch_id
                       << " workset_id=" << claimed.workset_id
                       << " job_id=" << durable_item.job_id
                       << " item_id=" << payload.item_id
                       << " item_ordinal=" << payload.item_ordinal
                       << " reserved_attempt_id="
                       << durable_item.reserved_attempt_id
                       << " terminal_id=" << payload.terminal_id;
                if (!receipt.durable_job_state.empty()) {
                    detail << " durable_job_state="
                           << receipt.durable_job_state;
                }
                if (!error.empty()) {
                    detail << " db_error=" << error;
                }
                PauseForInvariant(
                    dispatch,
                    "Worker terminal durable authority rejected",
                    detail.str(),
                    durable_item.job_id);
            }
        }
        return true;
    }

    if (prepared->pinned) {
        blob_store_->Unpin(blob.relative_path);
        prepared->pinned = false;
    }
    bool newly_staged = false;
    bool all_staged = false;
    {
        std::lock_guard lock(dispatch->mutex);
        newly_staged =
            dispatch->staged_jobs.emplace(durable_item.job_id).second;
        all_staged =
            dispatch->staged_jobs.size()
                + dispatch->initially_suppressed_jobs.size()
            == dispatch->claimed.items.size();
    }
    if (newly_staged) {
        ++worker_terminals_staged_;
        if (event.retry_attempt != 0) {
            ++worker_terminal_retry_attempts_;
        }
        std::optional<savor::db::CommittedJobCancellation>
            cancellation;
        {
            std::lock_guard lock(cancellation_index_mutex_);
            const auto found = committed_cancellations_by_job_.find(
                durable_item.job_id);
            if (found != committed_cancellations_by_job_.end()) {
                cancellation = found->second.cancellation;
            }
        }
        if (cancellation.has_value()) {
            (void)ResolveClaimedCancellation(
                *cancellation,
                "WORKER_TERMINAL",
                false,
                dispatch_id,
                claimed.claim_token);
        }
    }
    if (all_staged) {
        UnregisterLeaseHeartbeat(
            dispatch_id,
            claimed.claim_token);
    }
    EnqueueWorkerControl(
        dispatch->target,
        {
            .kind = WorkerControlKind::Acknowledge,
            .dispatch_attempt_id = dispatch_id,
            .job_id = durable_item.job_id,
            .terminal = terminal,
        });
    MaybeRetire(dispatch);
    return true;
}

bool JobExecutionCoordinator::Impl::HandleWorksetSummary(
    const SummaryEvent& event) {
    const auto dispatch_id =
        static_cast<std::int64_t>(event.payload.workset_id);
    const auto dispatch = FindDispatch(dispatch_id);
    if (!dispatch) return true;
    {
        std::lock_guard lock(dispatch->mutex);
        if (dispatch->submitted && !dispatch->dispatch_marked) {
            return false;
        }
        if (dispatch->target.worker_id != event.source.worker_id
            || dispatch->target.process_generation
                != event.source.process_generation
            || event.payload.item_count
                != dispatch->claimed.items.size()
            || event.payload.initially_suppressed_count
                != dispatch->initially_suppressed_jobs.size()
            || static_cast<std::uint64_t>(
                    event.payload.completed_count)
                    + event.payload.unstarted_count
                    + event.payload.initially_suppressed_count
                != event.payload.item_count) {
            if (dispatch->phase == DispatchPhase::ReleasedDraining) {
                return true;
            }
            PauseForInvariant(
                dispatch,
                "Worker workset summary mismatch",
                "dispatch_attempt_id=" + std::to_string(dispatch_id));
            return true;
        }
        dispatch->summary_observed = true;
    }
    ClearLaneIdentity(dispatch->target, dispatch_id);
    WakeScheduler("workset-summary", false);
    MaybeRetire(dispatch);
    return true;
}

void JobExecutionCoordinator::Impl::EnqueueAuthority(
    std::function<void()> action) {
    if (stop_.load()) return;
    {
        std::lock_guard lock(authority_mutex_);
        authority_actions_.push_back(std::move(action));
    }
    authority_cv_.notify_all();
}

void JobExecutionCoordinator::Impl::AuthorityLoop() {
    while (!stop_.load()) {
        bool advanced = false;
        for (;;) {
            std::function<void()> action;
            {
                std::lock_guard lock(authority_mutex_);
                if (authority_actions_.empty()) break;
                action = std::move(authority_actions_.front());
                authority_actions_.pop_front();
            }
            if (action) action();
            advanced = true;
        }

        ProcessLifecycleRequest();
        RefreshStorageReadiness();
        advanced = ProcessPendingDrainingTransitions() || advanced;

        if (!advanced) {
            std::unique_lock lock(authority_mutex_);
            authority_cv_.wait_for(
                lock,
                std::max(
                    config_.poll_interval,
                    std::chrono::milliseconds(1)));
        }
    }
    running_.store(false);
    lifecycle_cv_.notify_all();
}

bool JobExecutionCoordinator::Impl::ResolveClaimedCancellation(
    const savor::db::CommittedJobCancellation& cancellation,
    std::string resolution_code,
    bool finalize_job_canceled,
    std::optional<std::int64_t> dispatch_attempt_id,
    std::optional<std::string> claim_token) {
    const auto requested_code = resolution_code;
    QueueCancellationMutation(
        {
            .mutation = savor::db::JobCancellationOutcomeCommand{
                .kind = finalize_job_canceled
                    ? dispatch_attempt_id.has_value()
                        ? savor::db::JobCancellationOutcomeKind::
                            InitialSidecarApplied
                        : savor::db::JobCancellationOutcomeKind::
                            CancelWithoutWorker
                    : savor::db::JobCancellationOutcomeKind::
                        WorkerTerminalResolved,
                .cancellation_request_id =
                    cancellation.cancellation_request_id,
                .job_id = cancellation.job_id,
                .dispatch_attempt_id = dispatch_attempt_id,
                .claim_token = std::move(claim_token),
                .resolution_code = std::move(resolution_code),
                .requested_by = "job_execution_coordinator",
            },
            .requested_resolution_code = requested_code,
        });
    return true;
}

void JobExecutionCoordinator::Impl::QueueCancellationMutation(
    PendingCancellationMutation mutation) {
    mutation.enqueued_at = Clock::now();
    {
        std::lock_guard lock(cancellation_mutation_mutex_);
        cancellation_mutations_.push_back(std::move(mutation));
        cancellation_mutation_high_water_ = std::max(
            cancellation_mutation_high_water_,
            cancellation_mutations_.size());
        pending_cancellation_mutations_.store(
            cancellation_mutations_.size());
    }
    cancellation_mutation_cv_.notify_all();
}

void JobExecutionCoordinator::Impl::CancellationMutationLoop() {
    for (;;) {
        std::vector<PendingCancellationMutation> pending;
        bool full_flush = false;
        bool deadline_flush = false;
        bool barrier_flush = false;
        {
            std::unique_lock lock(cancellation_mutation_mutex_);
            cancellation_mutation_cv_.wait(lock, [this]() {
                return cancellation_mutation_stop_
                    || !cancellation_mutations_.empty();
            });
            if (cancellation_mutation_stop_
                && cancellation_mutations_.empty()) {
                return;
            }
            const auto deadline = cancellation_mutations_.front().enqueued_at
                + config_.cancellation_mutation_delay;
            while (!cancellation_mutation_stop_
                && cancellation_mutations_.size()
                    < config_.cancellation_batch_size
                && Clock::now() < deadline) {
                cancellation_mutation_cv_.wait_until(lock, deadline);
            }
            full_flush = cancellation_mutations_.size()
                >= config_.cancellation_batch_size;
            deadline_flush = !full_flush && !cancellation_mutation_stop_
                && Clock::now() >= deadline;
            barrier_flush = cancellation_mutation_stop_;
            const auto count = std::min(
                config_.cancellation_batch_size,
                cancellation_mutations_.size());
            pending.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                pending.push_back(std::move(cancellation_mutations_[index]));
            }
            cancellation_mutations_.erase(
                cancellation_mutations_.begin(),
                cancellation_mutations_.begin()
                    + static_cast<std::ptrdiff_t>(count));
            pending_cancellation_mutations_.store(
                cancellation_mutations_.size());
        }
        if (pending.empty()) continue;
        if (full_flush) ++cancellation_mutation_full_flushes_;
        else if (deadline_flush) ++cancellation_mutation_deadline_flushes_;
        else if (barrier_flush) ++cancellation_mutation_barrier_flushes_;

        savor::db::MutateJobCancellationsBatchCommand command;
        command.mutations.reserve(pending.size());
        for (const auto& item : pending) {
            command.mutations.push_back(item.mutation);
        }
        std::vector<savor::db::JobCancellationReceipt> receipts;
        std::string error;
        bool committed = false;
        for (;;) {
            committed = execution_db_->MutateJobCancellationsBatch(
                command, &receipts, &error)
                && receipts.size() == pending.size();
            if (committed) break;
            ++cancellation_mutation_rollbacks_;
            cancellation_mutation_retries_.fetch_add(pending.size());
            cancellation_storage_paused_.store(true);
            WakeScheduler("cancellation-storage-pause", false);
            RecordError(
                error.empty()
                    ? "failed committing cancellation mutation batch"
                    : error);
            if (cancellation_mutation_stop_) break;
            std::unique_lock lock(cancellation_mutation_mutex_);
            cancellation_mutation_cv_.wait_for(
                lock, std::chrono::milliseconds(5), [this]() {
                    return cancellation_mutation_stop_.load();
                });
            receipts.clear();
            error.clear();
        }
        ++cancellation_mutation_batches_;
        cancellation_mutation_batch_items_.fetch_add(pending.size());
        StoreMaximum(cancellation_mutation_max_size_, pending.size());
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - pending.front().enqueued_at).count();
        StoreMaximum(
            cancellation_mutation_max_collection_age_ms_,
            static_cast<std::uint64_t>(std::max<std::int64_t>(0, age)));
        if (!committed) {
            if (cancellation_mutation_stop_.load()) {
                worker_control_commands_abandoned_.fetch_add(pending.size());
                return;
            }
            for (auto& item : pending) {
                QueueCancellationMutation(std::move(item));
            }
            continue;
        }
        if (cancellation_storage_paused_.exchange(false)) {
            WakeScheduler("cancellation-storage-resume", false);
        }
        for (std::size_t index = 0; index < pending.size(); ++index) {
            const auto& item = pending[index];
            const auto& receipt = receipts[index];
            if (!Applied(receipt.disposition)) {
                RecordError(
                    "cancellation mutation lost durable authority: disposition="
                    + std::string(
                        ExecutionDbDispositionName(receipt.disposition)));
                ++worker_control_commands_failed_;
                if (item.dispatch) {
                    PauseForInvariant(
                        item.dispatch,
                        "Cancellation outcome lost durable authority",
                        "request_id=" + std::to_string(
                            item.mutation.cancellation_request_id)
                            + " job_id=" + std::to_string(
                                item.mutation.job_id)
                            + " disposition="
                            + ExecutionDbDispositionName(
                                receipt.disposition),
                        item.mutation.job_id);
                } else {
                    invariant_paused_.store(true);
                    WakeScheduler(
                        "cancellation-outcome-invariant", false);
                }
                continue;
            }
            if (receipt.state == "RESOLVED") {
                std::lock_guard lock(cancellation_index_mutex_);
                committed_cancellations_by_job_.erase(receipt.job_id);
            }
            if (item.initial_suppression && item.dispatch) {
                bool fully_suppressed = false;
                bool all_persisted = false;
                {
                    std::lock_guard lock(item.dispatch->mutex);
                    item.dispatch->sidecar_persisted_jobs.insert(
                        receipt.job_id);
                    fully_suppressed =
                        item.dispatch->initially_suppressed_jobs.size()
                        == item.dispatch->claimed.items.size();
                    all_persisted =
                        item.dispatch->sidecar_persisted_jobs.size()
                        == item.dispatch->initially_suppressed_jobs.size();
                    if (fully_suppressed && all_persisted) {
                        item.dispatch->phase = DispatchPhase::Retired;
                    }
                }
                if (fully_suppressed && all_persisted) {
                    ClearLaneIdentity(
                        item.dispatch->target,
                        item.dispatch->claimed.dispatch_attempt_id);
                    RemoveDispatch(
                        item.dispatch->claimed.dispatch_attempt_id,
                        item.dispatch);
                } else {
                    MaybeRetire(item.dispatch);
                }
            }
            RecordCancellationResolution(
                receipt.resolution_code.value_or(
                    item.requested_resolution_code.empty()
                        ? item.delivery ? "DELIVERED" : "RESOLVED"
                        : item.requested_resolution_code));
            if (item.delivery) {
                ++worker_control_commands_applied_;
                if (receipt.disposition
                    == savor::db::ExecutionDbOperationDisposition::AlreadyApplied) {
                    ++cancellations_already_applied_;
                } else {
                    ++cancellations_delivered_;
                }
            }
        }
    }
}

void JobExecutionCoordinator::Impl::CompleteCancellationDelivery(
    savor::db::CommittedJobCancellation cancellation,
    const WorkerCommandResult& result,
    bool whole_workset,
    Clock::time_point enqueued_at) {
    const auto latency = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - enqueued_at).count());
    cancellation_delivery_total_latency_ms_.fetch_add(latency);
    StoreMaximum(cancellation_delivery_max_latency_ms_, latency);
    if (!result.accepted()) {
        ++worker_control_commands_failed_;
        RecordWarning(
            "Worker cancellation delivery was not accepted",
            result.diagnostic,
            result.worker_id.has_value()
                ? static_cast<std::int64_t>(*result.worker_id)
                : 0,
            cancellation.job_id);
        EnqueueAuthority(
            [this, cancellation = std::move(cancellation),
             diagnostic = result.diagnostic]() mutable {
                QueueCancellationMutation(
                    {
                        .mutation = savor::db::
                            JobCancellationOutcomeCommand{
                                .kind = savor::db::
                                    JobCancellationOutcomeKind::
                                        DeliveryFailed,
                                .cancellation_request_id =
                                    cancellation.cancellation_request_id,
                                .job_id = cancellation.job_id,
                                .error_code =
                                    "WORKER_CANCELLATION_DELIVERY_FAILED",
                                .error_text = diagnostic.empty()
                                    ? "worker cancellation command was not accepted"
                                    : std::move(diagnostic),
                                .requested_by =
                                    "job_execution_coordinator",
                            },
                        .requested_resolution_code =
                            "REQUESTED",
                    });
            });
        return;
    }
    (void)whole_workset;
    EnqueueAuthority(
        [this, cancellation = std::move(cancellation)]() mutable {
            QueueCancellationMutation(
                {
                    .mutation = savor::db::
                        JobCancellationOutcomeCommand{
                            .kind = savor::db::
                                JobCancellationOutcomeKind::
                                    WorkerDeliveryAccepted,
                            .cancellation_request_id =
                                cancellation.cancellation_request_id,
                            .job_id = cancellation.job_id,
                            .requested_by =
                                "job_execution_coordinator",
                        },
                    .requested_resolution_code = "DELIVERED",
                    .delivery = true,
                });
        });
}

void JobExecutionCoordinator::Impl::HandleWorksetState(
    const WorkerCoordinatorEventContext& source,
    const savor::wrms::WorksetStatePayload& payload) {
    const bool terminal =
        payload.state == savor::wrms::WorksetStateCode::Completed
        || payload.state == savor::wrms::WorksetStateCode::Cancelled
        || payload.state == savor::wrms::WorksetStateCode::Failed;
    if (!terminal
        || payload.workset_id == 0
        || payload.workset_id
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
        return;
    }
    const auto dispatch_id =
        static_cast<std::int64_t>(payload.workset_id);
    const auto dispatch = FindDispatch(dispatch_id);
    if (!dispatch) return;

    bool persist_draining = false;
    bool source_mismatch = false;
    {
        std::lock_guard lock(dispatch->mutex);
        if (dispatch->target.worker_id != source.worker_id
            || dispatch->target.process_generation
                != source.process_generation) {
            if (dispatch->phase == DispatchPhase::Released
                || dispatch->phase == DispatchPhase::ReleasedDraining
                || dispatch->phase == DispatchPhase::Retired) {
                return;
            }
            source_mismatch = true;
        } else {
            dispatch->terminal_workset_state_observed = true;
            if (dispatch->dispatch_marked
                && dispatch->phase == DispatchPhase::Active) {
                dispatch->phase = DispatchPhase::Draining;
                dispatch->draining_retry_at = Clock::now();
                persist_draining = true;
            }
        }
    }
    if (source_mismatch) {
        PauseForInvariant(
            dispatch,
            "Worker terminal workset state source mismatch",
            "dispatch_attempt_id=" + std::to_string(dispatch_id));
        return;
    }
    ClearLaneIdentity(dispatch->target, dispatch_id);
    UnregisterLeaseHeartbeat(
        dispatch_id,
        dispatch->claimed.claim_token);
    if (persist_draining) {
        EnqueueAuthority(
            [this, dispatch]() {
                (void)MarkDispatchDraining(dispatch);
            });
    }
    WakeScheduler("workset-terminal-state", false);
}

bool JobExecutionCoordinator::Impl::MarkDispatchDraining(
    const DispatchPtr& dispatch) {
    if (!dispatch) return true;
    savor::db::ClaimedPublishedWorkset claimed;
    DispatchPhase phase = DispatchPhase::Released;
    {
        std::lock_guard lock(dispatch->mutex);
        claimed = dispatch->claimed;
        phase = dispatch->phase;
        if (dispatch->draining_transition_persisted) return true;
        if (phase == DispatchPhase::Draining
            && dispatch->draining_retry_at > Clock::now()) {
            return false;
        }
    }
    if (phase == DispatchPhase::Released
        || phase == DispatchPhase::ReleasedDraining
        || phase == DispatchPhase::Retired) {
        return true;
    }
    savor::db::WorksetDispatchMutationReceipt receipt{};
    std::string error;
    if (!execution_db_->MarkWorksetDraining(
            {
                .dispatch_attempt_id = claimed.dispatch_attempt_id,
                .claim_token = claimed.claim_token,
                .requested_by = "job_execution_coordinator",
            },
            &receipt,
            &error)) {
        {
            std::lock_guard lock(dispatch->mutex);
            if (dispatch->phase == DispatchPhase::Draining) {
                dispatch->draining_retry_at = Clock::now()
                    + config_.workset_lease_retry_interval;
            }
        }
        RecordError(
            error.empty()
                ? "failed marking workset draining"
                : std::move(error));
        return false;
    }
    if (Applied(receipt.disposition)) {
        {
            std::lock_guard lock(dispatch->mutex);
            if (dispatch->phase == DispatchPhase::Draining) {
                dispatch->draining_transition_persisted = true;
            }
        }
        ++draining_transitions_;
        return true;
    }
    {
        std::lock_guard lock(dispatch->mutex);
        if (dispatch->phase == DispatchPhase::Released
            || dispatch->phase == DispatchPhase::ReleasedDraining
            || dispatch->phase == DispatchPhase::Retired) {
            return true;
        }
    }
    PauseForInvariant(
        dispatch,
        "Durable draining transition rejected",
        "dispatch_attempt_id="
            + std::to_string(claimed.dispatch_attempt_id)
            + " disposition="
            + ExecutionDbDispositionName(receipt.disposition));
    return false;
}

bool JobExecutionCoordinator::Impl::ProcessPendingDrainingTransitions() {
    std::vector<DispatchPtr> pending;
    const auto now = Clock::now();
    {
        std::lock_guard lock(registry_mutex_);
        for (const auto& [dispatch_id, dispatch] : dispatches_) {
            (void)dispatch_id;
            std::lock_guard dispatch_lock(dispatch->mutex);
            if (dispatch->phase == DispatchPhase::Draining
                && !dispatch->draining_transition_persisted
                && dispatch->draining_retry_at <= now) {
                pending.push_back(dispatch);
            }
        }
    }
    bool advanced = false;
    for (const auto& dispatch : pending) {
        advanced = MarkDispatchDraining(dispatch) || advanced;
    }
    return advanced;
}

void JobExecutionCoordinator::Impl::HandleWorkerUnavailable(
    WorkerUnavailableEvent event) {
    ++worker_losses_;
    std::vector<DispatchPtr> affected;
    {
        std::lock_guard lock(registry_mutex_);
        for (const auto& [dispatch_id, dispatch] : dispatches_) {
            (void)dispatch_id;
            std::lock_guard record_lock(dispatch->mutex);
            if (dispatch->target.worker_id == event.source.worker_id
                && dispatch->target.process_generation
                    == event.source.process_generation) {
                affected.push_back(dispatch);
            }
        }
    }
    for (const auto& dispatch : affected) {
        (void)ReleaseDispatch(
            dispatch,
            "WORKER_LOST",
            event.diagnostic);
    }
    if (!event.diagnostic.empty()) {
        RecordWarning(
            "Worker became unavailable",
            event.diagnostic,
            static_cast<std::int64_t>(event.source.worker_id));
    }
    WakeScheduler("worker-unavailable", false);
}

void JobExecutionCoordinator::Impl::RefreshStorageReadiness() {
    if (blob_store_ready_.load()) return;
    const auto now = Clock::now();
    if (now < next_blob_readiness_retry_at_) return;
    std::string error;
    if (blob_store_->ValidateReady(&error)) {
        blob_store_ready_.store(true);
        blob_readiness_retry_attempt_ = 0;
        bool has_retries = false;
        {
            std::lock_guard lock(persistence_mutex_);
            has_retries = std::any_of(
                persistence_streams_.begin(),
                persistence_streams_.end(),
                [](const auto& entry) {
                    return std::any_of(
                        entry.second.events.begin(),
                        entry.second.events.end(),
                        [](const PersistenceEvent& event) {
                            const auto* terminal =
                                std::get_if<TerminalEvent>(&event.payload);
                            return terminal != nullptr
                                && terminal->retry_attempt != 0;
                        });
                });
        }
        if (!has_retries) storage_paused_.store(false);
        WakeScheduler("storage-ready", false);
        return;
    }
    ++blob_readiness_failures_;
    ++blob_readiness_retry_attempt_;
    storage_paused_.store(true);
    next_blob_readiness_retry_at_ =
        now
        + CappedExponentialDelay(
            config_.blob_readiness_retry_interval,
            config_.blob_readiness_retry_max_interval,
            blob_readiness_retry_attempt_);
    RecordError(
        error.empty()
            ? "worker result blob store remains unavailable"
            : std::move(error));
}

void JobExecutionCoordinator::Impl::ProcessLifecycleRequest() {
    LifecycleRequestKind kind{};
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (!lifecycle_request_.pending) return;
        kind = lifecycle_request_.kind;
    }

    std::vector<DispatchPtr> release;
    bool submission_in_progress = false;
    {
        std::lock_guard lock(registry_mutex_);
        for (const auto& [dispatch_id, dispatch] : dispatches_) {
            (void)dispatch_id;
            std::lock_guard record_lock(dispatch->mutex);
            if (kind == LifecycleRequestKind::ReleaseUnsubmitted
                && (dispatch->phase == DispatchPhase::Active
                    || dispatch->phase == DispatchPhase::Draining)) {
                continue;
            }
            if (dispatch->phase == DispatchPhase::Submitting) {
                submission_in_progress = true;
                continue;
            }
            if (dispatch->phase != DispatchPhase::Released
                && dispatch->phase != DispatchPhase::ReleasedDraining
                && dispatch->phase != DispatchPhase::Retired) {
                release.push_back(dispatch);
            }
        }
    }
    if (submission_in_progress) return;

    std::string first_error;
    for (const auto& dispatch : release) {
        if (!ReleaseDispatch(
                dispatch,
                kind == LifecycleRequestKind::
                        RecoverAfterWorkersStopped
                    ? "WORKER_COORDINATOR_STOPPED"
                    : "COORDINATOR_QUIESCED",
                "coordinator lifecycle release")
            && first_error.empty()) {
            first_error =
                "one or more workset claims could not be released";
        }
    }
    {
        std::lock_guard lock(lifecycle_mutex_);
        lifecycle_request_.error = std::move(first_error);
        lifecycle_request_.pending = false;
        lifecycle_request_.completed = true;
    }
    lifecycle_cv_.notify_all();
}

JobExecutionCoordinator::Impl::DispatchPtr
JobExecutionCoordinator::Impl::FindDispatch(
    std::int64_t dispatch_attempt_id) const {
    std::lock_guard lock(registry_mutex_);
    const auto found = dispatches_.find(dispatch_attempt_id);
    return found == dispatches_.end()
        ? DispatchPtr{}
        : found->second;
}

void JobExecutionCoordinator::Impl::InsertDispatch(
    const DispatchPtr& dispatch) {
    std::lock_guard lock(registry_mutex_);
    dispatches_.emplace(
        dispatch->claimed.dispatch_attempt_id,
        dispatch);
}

void JobExecutionCoordinator::Impl::RemoveDispatch(
    std::int64_t dispatch_attempt_id,
    const DispatchPtr& expected) {
    std::lock_guard lock(registry_mutex_);
    const auto found = dispatches_.find(dispatch_attempt_id);
    if (found != dispatches_.end()
        && found->second == expected) {
        dispatches_.erase(found);
    }
}

bool JobExecutionCoordinator::Impl::ReleaseDispatch(
    const DispatchPtr& dispatch,
    std::string reason_code,
    std::string reason_text,
    bool keep_draining,
    bool* deferred_out,
    bool retain_release_after_submission) {
    if (deferred_out != nullptr) {
        *deferred_out = false;
    }
    if (!dispatch) return true;
    savor::db::ClaimedPublishedWorkset claimed;
    WorkerExecutionTarget target;
    DispatchPhase previous = DispatchPhase::Claimed;
    bool submitted = false;
    const auto release_reason = reason_code;
    {
        std::lock_guard lock(dispatch->mutex);
        if (dispatch->phase == DispatchPhase::Released
            || dispatch->phase == DispatchPhase::ReleasedDraining
            || dispatch->phase == DispatchPhase::Retired) {
            return true;
        }
        if (dispatch->phase == DispatchPhase::ReleaseRequested) {
            return true;
        }
        if (dispatch->phase == DispatchPhase::Submitting
            && !dispatch->dispatch_marked) {
            if (retain_release_after_submission
                && !dispatch->deferred_release.has_value()) {
                dispatch->deferred_release = DeferredDispatchRelease{
                    .reason_code = std::move(reason_code),
                    .reason_text = std::move(reason_text),
                    .keep_draining = keep_draining,
                };
            }
            if (deferred_out != nullptr) {
                *deferred_out = true;
            }
            return true;
        }
        previous = dispatch->phase;
        dispatch->phase = DispatchPhase::ReleaseRequested;
        claimed = dispatch->claimed;
        target = dispatch->target;
        submitted = dispatch->submitted;
    }

    savor::db::WorksetDispatchMutationReceipt receipt{};
    std::string error;
    if (!execution_db_->ReleaseWorksetDispatch(
            {
                .dispatch_attempt_id =
                    claimed.dispatch_attempt_id,
                .claim_token = claimed.claim_token,
                .reason_code = std::move(reason_code),
                .reason_text = reason_text.empty()
                    ? std::nullopt
                    : std::optional<std::string>(
                        std::move(reason_text)),
                .requested_by = "job_execution_coordinator",
            },
            &receipt,
            &error)
        || !Applied(receipt.disposition)) {
        {
            std::lock_guard lock(dispatch->mutex);
            if (dispatch->phase
                == DispatchPhase::ReleaseRequested) {
                dispatch->phase = previous;
            }
        }
        RecordError(
            error.empty()
                ? "failed releasing workset dispatch"
                : std::move(error));
        return false;
    }
    UnregisterLeaseHeartbeat(
        claimed.dispatch_attempt_id,
        claimed.claim_token);
    {
        std::lock_guard lock(dispatch_release_mutex_);
        ++dispatch_release_reason_counts_[release_reason];
        const char* phase = "UNKNOWN";
        switch (previous) {
        case DispatchPhase::Claimed: phase = "CLAIMED"; break;
        case DispatchPhase::Reconstructing: phase = "RECONSTRUCTING"; break;
        case DispatchPhase::Waiting: phase = "WAITING"; break;
        case DispatchPhase::Submitting: phase = "SUBMITTING"; break;
        case DispatchPhase::Active: phase = "ACTIVE"; break;
        case DispatchPhase::Draining: phase = "DRAINING"; break;
        case DispatchPhase::ReleasedDraining:
            phase = "RELEASED_DRAINING";
            break;
        case DispatchPhase::ReleaseRequested: phase = "RELEASE_REQUESTED"; break;
        case DispatchPhase::Released: phase = "RELEASED"; break;
        case DispatchPhase::Retired: phase = "RETIRED"; break;
        }
        ++dispatch_release_phase_counts_[phase];
    }

    const auto lane =
        FindWorkerLane(target.worker_id, target.process_generation);
    if (lane) {
        std::lock_guard lock(lane->mutex);
        const auto waiting = std::find(
            lane->waiting.begin(),
            lane->waiting.end(),
            dispatch);
        if (waiting != lane->waiting.end()) {
            lane->waiting.erase(waiting);
        }
        if (lane->retry_submission == dispatch) {
            lane->retry_submission.reset();
        }
        RemoveProjectedAffinity(
            *lane,
            claimed.dispatch_attempt_id);
        if (previous == DispatchPhase::Reconstructing) {
            if (lane->reconstructing > 0) --lane->reconstructing;
            if (lane->reservations > 0) --lane->reservations;
        }
        if (lane->submitting
            == std::optional<std::int64_t>(
                claimed.dispatch_attempt_id)) {
            lane->submitting.reset();
        }
        if (lane->active
            == std::optional<std::int64_t>(
                claimed.dispatch_attempt_id)) {
            lane->active.reset();
        }
        lane->cv.notify_all();
    }
    {
        std::lock_guard lock(dispatch->mutex);
        dispatch->phase =
            keep_draining && submitted
            ? DispatchPhase::ReleasedDraining
            : DispatchPhase::Released;
    }
    if (!(keep_draining && submitted)) {
        RemoveDispatch(claimed.dispatch_attempt_id, dispatch);
    }
    WakeScheduler("reservation-released", false);
    return true;
}

void JobExecutionCoordinator::Impl::PauseForInvariant(
    const DispatchPtr& dispatch,
    std::string message,
    std::string detail,
    std::int64_t job_id) {
    ++reconstruction_invariant_failures_;
    invariant_paused_.store(true);
    std::int64_t worker_id = 0;
    if (dispatch) {
        worker_id = static_cast<std::int64_t>(
            dispatch->target.worker_id);
    }
    RecordWarning(message, detail, worker_id, job_id);
    RecordError(detail);
    if (dispatch) {
        EnqueueAuthority(
            [this, dispatch, detail]() {
                bool submitted = false;
                WorkerExecutionTarget target;
                std::int64_t dispatch_id = 0;
                {
                    std::lock_guard lock(dispatch->mutex);
                    submitted = dispatch->submitted;
                    target = dispatch->target;
                    dispatch_id =
                        dispatch->claimed.dispatch_attempt_id;
                }
                if (submitted) {
                    EnqueueWorkerControl(
                        target,
                        {
                            .kind =
                                WorkerControlKind::CancelWorkset,
                            .dispatch_attempt_id = dispatch_id,
                            .reason = detail,
                        });
                }
                (void)ReleaseDispatch(
                    dispatch,
                    "COORDINATOR_INVARIANT_FAILURE",
                    detail,
                    submitted);
            });
    }
    WakeScheduler("invariant-pause", false);
}

void JobExecutionCoordinator::Impl::ClearLaneIdentity(
    const WorkerExecutionTarget& target,
    std::int64_t dispatch_attempt_id) {
    const auto lane =
        FindWorkerLane(target.worker_id, target.process_generation);
    if (!lane) return;
    std::lock_guard lock(lane->mutex);
    if (lane->submitting
        == std::optional<std::int64_t>(dispatch_attempt_id)) {
        lane->submitting.reset();
    }
    if (lane->active
        == std::optional<std::int64_t>(dispatch_attempt_id)) {
        lane->active.reset();
    }
    lane->cv.notify_all();
}

void JobExecutionCoordinator::Impl::MaybeRetire(
    const DispatchPtr& dispatch) {
    if (!dispatch) return;
    bool complete = false;
    std::int64_t dispatch_id = 0;
    WorkerExecutionTarget target;
    std::string claim_token;
    {
        std::lock_guard lock(dispatch->mutex);
        dispatch_id = dispatch->claimed.dispatch_attempt_id;
        target = dispatch->target;
        claim_token = dispatch->claimed.claim_token;
        const auto item_count = dispatch->claimed.items.size();
        const auto suppressed =
            dispatch->initially_suppressed_jobs.size();
        const auto executable = item_count
            - std::min(item_count, suppressed);
        const bool sidecar_persisted =
            dispatch->sidecar_persisted_jobs.size() == suppressed;
        complete = dispatch->phase == DispatchPhase::ReleasedDraining
            ? dispatch->summary_observed
                && dispatch->acknowledged_jobs.size() == executable
                && sidecar_persisted
            : dispatch->summary_observed
                && dispatch->staged_jobs.size() == executable
                && dispatch->acknowledged_jobs.size() == executable
                && sidecar_persisted;
        if (complete) dispatch->phase = DispatchPhase::Retired;
    }
    if (!complete) return;
    UnregisterLeaseHeartbeat(dispatch_id, claim_token);
    ClearLaneIdentity(target, dispatch_id);
    RemoveDispatch(dispatch_id, dispatch);
}

void JobExecutionCoordinator::Impl::RegisterLeaseHeartbeat(
    const DispatchPtr& dispatch,
    std::int64_t lease_expires_at_utc) {
    if (!dispatch) return;
    savor::db::ClaimedPublishedWorkset claimed;
    {
        std::lock_guard lock(dispatch->mutex);
        if (dispatch->phase != DispatchPhase::Active
            || !dispatch->dispatch_marked
            || dispatch->terminal_workset_state_observed) {
            return;
        }
        claimed = dispatch->claimed;
    }
    LeaseHeartbeatEntry entry{
        .dispatch_attempt_id = claimed.dispatch_attempt_id,
        .claim_token = claimed.claim_token,
        .lease_expires_at_utc = lease_expires_at_utc,
        .renew_at = LeaseRenewalDeadline(
            lease_expires_at_utc,
            config_.workset_lease_renewal_point),
    };
    {
        std::lock_guard lock(lease_heartbeat_mutex_);
        lease_heartbeat_entries_.insert_or_assign(
            claimed.dispatch_attempt_id,
            std::move(entry));
    }
    lease_heartbeat_cv_.notify_all();
}

void JobExecutionCoordinator::Impl::UnregisterLeaseHeartbeat(
    std::int64_t dispatch_attempt_id,
    std::string_view claim_token) {
    {
        std::lock_guard lock(lease_heartbeat_mutex_);
        const auto found =
            lease_heartbeat_entries_.find(dispatch_attempt_id);
        if (found == lease_heartbeat_entries_.end()
            || found->second.claim_token != claim_token) {
            return;
        }
        lease_heartbeat_entries_.erase(found);
    }
    lease_heartbeat_cv_.notify_all();
}

void JobExecutionCoordinator::Impl::LeaseHeartbeatLoop() {
    while (!stop_.load()) {
        std::vector<LeaseHeartbeatEntry> due;
        {
            std::unique_lock lock(lease_heartbeat_mutex_);
            for (;;) {
                if (stop_.load()) return;
                if (lease_heartbeat_entries_.empty()) {
                    lease_heartbeat_cv_.wait(
                        lock,
                        [this]() {
                            return stop_.load()
                                || !lease_heartbeat_entries_.empty();
                        });
                    continue;
                }
                const auto earliest = std::min_element(
                    lease_heartbeat_entries_.begin(),
                    lease_heartbeat_entries_.end(),
                    [](const auto& lhs, const auto& rhs) {
                        return lhs.second.renew_at
                            < rhs.second.renew_at;
                    });
                const auto now = Clock::now();
                if (earliest->second.renew_at > now) {
                    lease_heartbeat_cv_.wait_until(
                        lock,
                        earliest->second.renew_at);
                    continue;
                }
                for (auto& [dispatch_id, candidate] :
                     lease_heartbeat_entries_) {
                    (void)dispatch_id;
                    if (candidate.renew_at <= now) {
                        due.push_back(candidate);
                        candidate.renew_at = now
                            + config_.workset_lease_retry_interval;
                    }
                }
                break;
            }
        }

        std::vector<LeaseHeartbeatEntry> renewable;
        std::vector<savor::db::WorksetDispatchLeaseRequest> requests;
        renewable.reserve(due.size());
        requests.reserve(due.size());
        for (const auto& entry : due) {
            const auto dispatch = FindDispatch(entry.dispatch_attempt_id);
            if (!dispatch) {
                UnregisterLeaseHeartbeat(
                    entry.dispatch_attempt_id,
                    entry.claim_token);
                continue;
            }
            DispatchPhase phase = DispatchPhase::Released;
            WorkerExecutionTarget target{};
            std::string durable_token;
            std::string expected_sidecar_sha256;
            {
                std::lock_guard lock(dispatch->mutex);
                phase = dispatch->phase;
                target = dispatch->target;
                durable_token = dispatch->claimed.claim_token;
                expected_sidecar_sha256 =
                    dispatch->cancellation_sidecar_sha256;
            }
            if (durable_token != entry.claim_token
                || phase != DispatchPhase::Active) {
                UnregisterLeaseHeartbeat(
                    entry.dispatch_attempt_id,
                    entry.claim_token);
                continue;
            }
            const auto lane = FindWorkerLane(
                target.worker_id,
                target.process_generation);
            bool lane_reports_active = false;
            if (lane) {
                std::lock_guard lane_lock(lane->mutex);
                lane_reports_active = lane->active.has_value()
                    && *lane->active == entry.dispatch_attempt_id;
            }
            if (!lane_reports_active) {
                UnregisterLeaseHeartbeat(
                    entry.dispatch_attempt_id,
                    entry.claim_token);
                continue;
            }

            ++active_residence_probes_;
            savor::wrms::WorksetResidenceSnapshotV1 evidence{};
            std::string evidence_error;
            if (!worker_coordinator_->ConfirmWorksetResidence(
                    target,
                    savor::runtime::WorkerWorksetId{
                        static_cast<std::uint64_t>(
                            entry.dispatch_attempt_id)},
                    &evidence,
                    &evidence_error)) {
                ++active_residence_failures_;
                UnregisterLeaseHeartbeat(
                    entry.dispatch_attempt_id,
                    entry.claim_token);
                RecordWarning(
                    "Active workset residence check failed",
                    evidence_error,
                    static_cast<std::int64_t>(target.worker_id));
                continue;
            }
            if (evidence.cancellation_sidecar_sha256
                != expected_sidecar_sha256) {
                ++active_residence_failures_;
                UnregisterLeaseHeartbeat(
                    entry.dispatch_attempt_id,
                    entry.claim_token);
                std::ostringstream diagnostic;
                diagnostic
                    << "worker residence sidecar mismatch: dispatch_attempt_id="
                    << entry.dispatch_attempt_id
                    << " expected=" << expected_sidecar_sha256
                    << " observed="
                    << evidence.cancellation_sidecar_sha256;
                worker_coordinator_->QuarantineWorkerGeneration(
                    target,
                    diagnostic.str());
                RecordWarning(
                    "Active workset residence sidecar check failed",
                    diagnostic.str(),
                    static_cast<std::int64_t>(target.worker_id));
                continue;
            }
            ++active_residence_matches_;
            renewable.push_back(entry);
            requests.push_back({
                .dispatch_attempt_id = entry.dispatch_attempt_id,
                .claim_token = entry.claim_token,
            });
        }
        if (requests.empty()) continue;

        std::string error;
        ++active_lease_renewal_batches_;
        auto receipts = execution_db_->RenewActiveWorksetLeases(
            {
                .requests = std::move(requests),
                .lease_duration_ms =
                    config_.workset_lease_duration.count(),
            },
            &error);
        if (receipts.size() != renewable.size()) {
            receipts.assign(
                renewable.size(),
                savor::db::WorksetDispatchLeaseReceipt{});
        }
        for (std::size_t index = 0; index < renewable.size(); ++index) {
            const auto& entry = renewable[index];
            auto receipt = receipts[index];
            receipt.dispatch_attempt_id = entry.dispatch_attempt_id;
            {
                std::lock_guard lock(lease_heartbeat_mutex_);
                const auto found = lease_heartbeat_entries_.find(
                    entry.dispatch_attempt_id);
                if (found != lease_heartbeat_entries_.end()
                    && found->second.claim_token == entry.claim_token) {
                    if (Applied(receipt.disposition)) {
                        const auto fallback_expiry =
                            std::chrono::duration_cast<
                                std::chrono::milliseconds>(
                                std::chrono::system_clock::now()
                                    .time_since_epoch())
                                .count()
                            + config_.workset_lease_duration.count();
                        const auto durable_expiry =
                            receipt.lease_expires_at_utc.value_or(
                                fallback_expiry);
                        found->second.lease_expires_at_utc =
                            durable_expiry;
                        found->second.renew_at = LeaseRenewalDeadline(
                            durable_expiry,
                            config_.workset_lease_renewal_point);
                    } else if (receipt.disposition
                        == savor::db::ExecutionDbOperationDisposition::
                            BackendError) {
                        ++active_lease_renewal_retries_;
                    } else {
                        lease_heartbeat_entries_.erase(found);
                    }
                }
            }
            EnqueueAuthority(
                [this,
                 dispatch_attempt_id = entry.dispatch_attempt_id,
                 claim_token = entry.claim_token,
                 receipt,
                 error]() mutable {
                    HandleLeaseHeartbeatResult(
                        dispatch_attempt_id,
                        std::move(claim_token),
                        receipt.disposition
                            != savor::db::
                                ExecutionDbOperationDisposition::BackendError,
                        receipt,
                        std::move(error));
                });
        }
    }
}

void JobExecutionCoordinator::Impl::HandleLeaseHeartbeatResult(
    std::int64_t dispatch_attempt_id,
    std::string claim_token,
    bool call_succeeded,
    savor::db::WorksetDispatchLeaseReceipt receipt,
    std::string error) {
    const auto dispatch = FindDispatch(dispatch_attempt_id);
    if (!dispatch) return;
    {
        std::lock_guard lock(dispatch->mutex);
        if (dispatch->claimed.claim_token != claim_token
            || dispatch->phase != DispatchPhase::Active) {
            return;
        }
        if (call_succeeded && Applied(receipt.disposition)) {
            return;
        }
    }
    if (receipt.disposition
        == savor::db::ExecutionDbOperationDisposition::BackendError) {
        RecordError(
            error.empty()
                ? "failed renewing workset dispatch lease"
                : std::move(error));
        return;
    }
    bool submitted = false;
    WorkerExecutionTarget target;
    {
        std::lock_guard lock(dispatch->mutex);
        submitted = dispatch->submitted;
        target = dispatch->target;
    }
    if (submitted) {
        EnqueueWorkerControl(
            target,
            {
                .kind = WorkerControlKind::CancelWorkset,
                .dispatch_attempt_id = dispatch_attempt_id,
                .reason =
                    "durable dispatch lease authority was rejected",
            });
    }
    (void)ReleaseDispatch(
        dispatch,
        "DISPATCH_LEASE_AUTHORITY_LOST",
        error.empty()
            ? "durable dispatch lease authority was rejected"
            : std::move(error),
        submitted);
    RecordWarning(
        "Workset dispatch authority was lost",
        "dispatch_attempt_id="
            + std::to_string(dispatch_attempt_id));
}

void JobExecutionCoordinator::Impl::RecordWarning(
    std::string message,
    std::string detail,
    std::int64_t worker_id,
    std::int64_t job_id) {
    std::lock_guard lock(warning_mutex_);
    warnings_.push_back(
        {
            .sequence = ++warning_sequence_,
            .worker_id = worker_id,
            .job_id = job_id,
            .observed_mono_ns = NowMonoNs(),
            .message = std::move(message),
            .detail = std::move(detail),
        });
    while (warnings_.size() > 128) warnings_.pop_front();
}

void JobExecutionCoordinator::Impl::RecordError(std::string error) {
    std::lock_guard lock(error_mutex_);
    last_error_ = std::move(error);
}

void JobExecutionCoordinator::Impl::RecordCancellationResolution(
    std::string code) {
    if (code.empty()) code = "UNSPECIFIED";
    std::lock_guard lock(cancellation_resolution_mutex_);
    ++cancellation_resolution_counts_[std::move(code)];
}

std::string JobExecutionCoordinator::Impl::NextToken(
    std::string_view purpose) {
    return coordinator_token_ + '-' + std::string(purpose) + '-'
        + std::to_string(token_sequence_.fetch_add(1));
}

JobExecutionCoordinator::JobExecutionCoordinator(
    savor::db::IExecutionDb* execution_db,
    const savor::db::execution::programdb::ProgramKindRegistry*
        program_kind_registry,
    WorkerCoordinator* worker_coordinator,
    savor::db::execution::WorkerResultBlobStore* blob_store,
    JobExecutionCoordinatorConfig config)
    : impl_(std::make_unique<Impl>(
          execution_db,
          program_kind_registry,
          worker_coordinator,
          blob_store,
          std::move(config))) {}

JobExecutionCoordinator::~JobExecutionCoordinator() = default;

bool JobExecutionCoordinator::Start(std::string* error_out) {
    return impl_->Start(error_out);
}
void JobExecutionCoordinator::Quiesce() { impl_->Quiesce(); }
bool JobExecutionCoordinator::ReleaseBufferedClaims(
    std::string* error_out) {
    return impl_->ReleaseBufferedClaims(error_out);
}
bool JobExecutionCoordinator::RecoverAfterWorkersStopped(
    std::string* error_out) {
    return impl_->RecoverAfterWorkersStopped(error_out);
}
void JobExecutionCoordinator::Stop() { impl_->Stop(); }
void JobExecutionCoordinator::SetPaused(bool paused) {
    impl_->SetPaused(paused);
}
bool JobExecutionCoordinator::IsPaused() const noexcept {
    return impl_->IsPaused();
}
bool JobExecutionCoordinator::IsRunning() const noexcept {
    return impl_->IsRunning();
}
bool JobExecutionCoordinator::ClearInvariantPause() {
    return impl_->ClearInvariantPause();
}
void JobExecutionCoordinator::RegisterCancellationCommitPending(
    std::uint64_t hold_id,
    const std::vector<savor::db::ExecutionCancellationRequestSpec>&
        cancellations) {
    impl_->RegisterCancellationCommitPending(hold_id, cancellations);
}
void JobExecutionCoordinator::RegisterCommittedCancellations(
    std::uint64_t hold_id,
    const std::vector<savor::db::CommittedJobCancellation>&
        cancellations) {
    impl_->RegisterCommittedCancellations(hold_id, cancellations);
}
void JobExecutionCoordinator::OpenCancellationAdmission() {
    impl_->OpenCancellationAdmission();
}
JobExecutionCoordinatorTelemetry
JobExecutionCoordinator::SnapshotTelemetry() const {
    return impl_->SnapshotTelemetry();
}
std::vector<JobExecutionWorkerLaneSnapshot>
JobExecutionCoordinator::SnapshotWorkerLanes() const {
    return impl_->SnapshotWorkerLanes();
}
std::vector<JobExecutionCoordinatorWarning>
JobExecutionCoordinator::SnapshotWarnings() const {
    return impl_->SnapshotWarnings();
}

} // namespace savor::runner::parallel::savordb
