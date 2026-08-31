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
#include "Runner/Runtime/Worksets/WorksetWireCodec.h"

namespace savor::runner::parallel::savordb {

bool detail::DispatchReadyToRetire(
    const DispatchRetirementFacts& facts) noexcept {
    if (!facts.summary_observed
        || facts.acknowledged_items != facts.executable_items
        || !facts.sidecar_persisted) {
        return false;
    }
    switch (facts.authority) {
    case DispatchRetirementAuthority::DrainingPersisted:
        return facts.staged_items == facts.executable_items;
    case DispatchRetirementAuthority::ReleasedDraining:
        return true;
    case DispatchRetirementAuthority::None:
    case DispatchRetirementAuthority::DrainingPending:
    default:
        return false;
    }
}

detail::CancellationDeliveryFailureDescription
detail::DescribeCancellationDeliveryFailure(
    const WorkerCommandResult& result) noexcept {
    using Disposition = WorkerCommandDisposition;
    using Rejection = savor::wrms::RejectionCode;
    switch (result.disposition) {
    case Disposition::LocalRejected:
        return {
            "Worker cancellation request was rejected locally",
            "WORKER_CANCELLATION_LOCAL_REJECTION",
        };
    case Disposition::NotFound:
        return {
            "Worker cancellation route was not found",
            "WORKER_CANCELLATION_ROUTE_NOT_FOUND",
        };
    case Disposition::StaleRoute:
        return {
            "Worker cancellation route was stale",
            "WORKER_CANCELLATION_STALE_ROUTE",
        };
    case Disposition::TransportCanceledBeforeWrite:
        return {
            "Worker cancellation command was not written",
            "WORKER_CANCELLATION_NOT_WRITTEN",
        };
    case Disposition::AmbiguousAfterWrite:
        return {
            "Worker cancellation outcome was ambiguous after write",
            "WORKER_CANCELLATION_AMBIGUOUS_AFTER_WRITE",
        };
    case Disposition::CoordinatorStopped:
        return {
            "Worker cancellation coordinator was stopped",
            "WORKER_CANCELLATION_COORDINATOR_STOPPED",
        };
    case Disposition::Accepted:
        return {
            "Accepted worker cancellation was classified as a failure",
            "WORKER_CANCELLATION_CLASSIFICATION_ERROR",
        };
    case Disposition::DefiniteRejected:
        break;
    }

    switch (result.rejection_code) {
    case Rejection::None:
        return {
            "Worker cancellation was rejected without a rejection code",
            "WORKER_CANCELLATION_REJECTED_WITHOUT_CODE",
        };
    case Rejection::Unsupported:
        return {
            "Worker cancellation command was unsupported",
            "WORKER_CANCELLATION_UNSUPPORTED",
        };
    case Rejection::InvalidState:
        return {
            "Worker cancellation was rejected for invalid worker state",
            "WORKER_CANCELLATION_INVALID_STATE",
        };
    case Rejection::InvalidArgument:
        return {
            "Worker cancellation was rejected for an invalid argument",
            "WORKER_CANCELLATION_INVALID_ARGUMENT",
        };
    case Rejection::SessionUnavailable:
        return {
            "Worker cancellation session was unavailable",
            "WORKER_CANCELLATION_SESSION_UNAVAILABLE",
        };
    case Rejection::SessionMismatch:
        return {
            "Worker cancellation session did not match",
            "WORKER_CANCELLATION_SESSION_MISMATCH",
        };
    case Rejection::SessionTainted:
        return {
            "Worker cancellation session was tainted",
            "WORKER_CANCELLATION_SESSION_TAINTED",
        };
    case Rejection::ProgramRuntimeUnavailable:
        return {
            "Worker cancellation program runtime was unavailable",
            "WORKER_CANCELLATION_RUNTIME_UNAVAILABLE",
        };
    case Rejection::InvocationAlreadyActive:
        return {
            "Worker cancellation encountered an already-active invocation",
            "WORKER_CANCELLATION_INVOCATION_ALREADY_ACTIVE",
        };
    case Rejection::InvocationNotActive:
        return {
            "Worker cancellation invocation was not active",
            "WORKER_CANCELLATION_INVOCATION_NOT_ACTIVE",
        };
    case Rejection::InvocationMismatch:
        return {
            "Worker cancellation invocation did not match",
            "WORKER_CANCELLATION_INVOCATION_MISMATCH",
        };
    case Rejection::DuplicateCancellation:
        return {
            "Worker cancellation was already requested",
            "WORKER_CANCELLATION_ALREADY_REQUESTED",
        };
    case Rejection::WorksetEpochMismatch:
        return {
            "Worker cancellation workset epoch did not match",
            "WORKER_CANCELLATION_WORKSET_EPOCH_MISMATCH",
        };
    case Rejection::BackendFailure:
        return {
            "Worker cancellation backend failed",
            "WORKER_CANCELLATION_BACKEND_FAILURE",
        };
    case Rejection::RuntimeStopping:
        return {
            "Worker cancellation runtime was stopping",
            "WORKER_CANCELLATION_RUNTIME_STOPPING",
        };
    case Rejection::InternalFailure:
        return {
            "Worker cancellation encountered an internal failure",
            "WORKER_CANCELLATION_INTERNAL_FAILURE",
        };
    case Rejection::WorksetAlreadyActive:
        return {
            "Worker cancellation encountered an already-active workset",
            "WORKER_CANCELLATION_WORKSET_ALREADY_ACTIVE",
        };
    case Rejection::WorksetNotFound:
        return {
            "Worker cancellation workset was not resident",
            "WORKER_CANCELLATION_WORKSET_NOT_FOUND",
        };
    case Rejection::WorksetItemNotFound:
        return {
            "Worker cancellation item was not resident",
            "WORKER_CANCELLATION_ITEM_NOT_FOUND",
        };
    case Rejection::ProgramPackageRejected:
        return {
            "Worker cancellation encountered a rejected program package",
            "WORKER_CANCELLATION_PROGRAM_PACKAGE_REJECTED",
        };
    case Rejection::CapacityExceeded:
        return {
            "Worker cancellation exceeded runtime capacity",
            "WORKER_CANCELLATION_CAPACITY_EXCEEDED",
        };
    case Rejection::TerminalNotFound:
        return {
            "Worker cancellation terminal was not found",
            "WORKER_CANCELLATION_TERMINAL_NOT_FOUND",
        };
    case Rejection::TerminalMismatch:
        return {
            "Worker cancellation terminal did not match",
            "WORKER_CANCELLATION_TERMINAL_MISMATCH",
        };
    case Rejection::WorksetItemAlreadyTerminal:
        return {
            "Worker returned an item-terminal rejection for an incompatible cancellation command",
            "WORKER_CANCELLATION_UNEXPECTED_ITEM_TERMINAL",
        };
    }
    return {
        "Worker cancellation returned an unknown rejection code",
        "WORKER_CANCELLATION_UNKNOWN_REJECTION",
    };
}

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
    std::vector<JobExecutionWorkerDispatchSnapshot>
        SnapshotWorkerDispatches() const;
    std::vector<JobExecutionCoordinatorWarning>
        SnapshotWarnings() const;

private:
    enum class DispatchPhase : std::uint8_t {
        Claimed = 0,
        Reconstructing,
        Prepared,
        Submitting,
        Activating,
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
        bool activation_io_pending = false;
        Clock::time_point activation_retry_at{};
        bool terminal_workset_state_observed = false;
        bool draining_transition_persisted = false;
        bool draining_io_pending = false;
        Clock::time_point draining_retry_at{};
        Clock::time_point release_retry_at{};
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
    struct ProgressEvent {
        WorkerCoordinatorEventContext source;
        savor::wrms::InvocationProgressPayload payload;
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
        std::variant<ItemStartedEvent, ProgressEvent, TerminalEvent, SummaryEvent>;
    struct PersistenceEvent {
        std::size_t worker_id = 0;
        std::uint64_t generation = 0;
        std::int64_t dispatch_attempt_id = 0;
        Clock::time_point enqueued_at{};
        PersistencePayload payload;
        struct PersistMutationRequest {
            savor::db::WorkerExecutionEventMutation mutation;
        };
        struct StageBlobRequest {
            std::int64_t workset_id = 0;
            std::int64_t dispatch_attempt_id = 0;
            std::int64_t job_id = 0;
            std::uint64_t terminal_id = 0;
            std::vector<std::uint8_t> envelope;
        };
        using IoRequest = std::variant<
            PersistMutationRequest,
            StageBlobRequest>;
        enum class IoKind : std::uint8_t {
            PersistMutation = 0,
            StageBlob,
        };
        struct IoCompletion {
            IoKind kind = IoKind::PersistMutation;
            bool succeeded = false;
            savor::db::WorkerExecutionEventMutationReceipt receipt;
            savor::db::execution::WorkerResultBlobReference blob;
            std::string error;
        };
        std::optional<IoRequest> io_request;
        std::optional<IoCompletion> io_completion;
    };
    struct PersistenceStream {
        std::deque<PersistenceEvent> events;
        bool active = false;
        std::uint32_t retry_attempt = 0;
        Clock::time_point retry_at{};
        std::string last_error;
    };

    struct PersistenceIoYield {};

    enum class WorkerControlKind : std::uint8_t {
        Acknowledge = 0,
        CancelItem,
        CancelWorkset,
        ConfirmResidence,
    };
    struct WorkerControlCommand {
        WorkerControlKind kind = WorkerControlKind::Acknowledge;
        std::int64_t dispatch_attempt_id = 0;
        std::int64_t job_id = 0;
        savor::runtime::WorkerItemTerminalCorrelation terminal;
        std::optional<savor::runtime::WorkerWorksetItemId> item_id;
        std::string reason;
        std::optional<savor::db::CommittedJobCancellation> cancellation;
        std::string lease_claim_token;
        std::string expected_sidecar_sha256;
        Clock::time_point enqueued_at{};
        Clock::time_point retry_at{};
    };

    struct MailboxSubmissionCommand {
        std::uint64_t command_id = 0;
        WorkerExecutionTarget target;
        DispatchPtr dispatch;
        savor::runtime::WorkerWorksetDefinition definition;
        savor::runtime::InitialWorksetCancellationSidecarV1 sidecar;
        Clock::time_point enqueued_at{};
    };

    struct WorksetAffinity {
        std::optional<std::string> program_package_sha256;
        std::optional<std::string> execution_key;
    };

    struct WorkerMailbox {
        mutable std::mutex mutex;
        std::condition_variable cv;
        std::thread thread;
        std::size_t worker_id = 0;
        std::uint64_t generation = 0;
        bool stop = false;
        std::optional<std::int64_t> submitting;
        std::optional<std::int64_t> active;
        std::deque<MailboxSubmissionCommand> submissions;
        std::deque<WorkerControlCommand> controls;
        bool has_submitted_workset = false;
        std::optional<std::string> actual_program_package_sha256;
        std::optional<std::string> actual_execution_key;
    };
    using WorkerMailboxPtr = std::shared_ptr<WorkerMailbox>;

    enum class CoordinatorEventKind : std::uint8_t {
        AvailabilityChanged = 0,
        WorkerEvidence,
        WorkerUnavailable,
        ReconstructionCompleted,
        MailboxCompleted,
        PersistenceCompleted,
        CancellationCompleted,
        LeaseCompleted,
        CoordinationIoCompleted,
        SynchronousRequest,
    };
    struct CoordinatorEvent {
        CoordinatorEventKind kind = CoordinatorEventKind::WorkerEvidence;
        std::function<void()> apply;
    };

    struct ClaimIoCommand {
        std::string batch_nonce;
        std::size_t requested_workset_count = 0;
        bool reconciliation_due = false;
        bool ready_signal = false;
    };
    struct ActivateIoCommand {
        DispatchPtr dispatch;
        WorkerMailboxPtr mailbox;
    };
    struct DrainingIoCommand {
        DispatchPtr dispatch;
        savor::db::ClaimedPublishedWorkset claimed;
    };
    struct ReleaseIoCommand {
        DispatchPtr dispatch;
        savor::db::ClaimedPublishedWorkset claimed;
        WorkerExecutionTarget target;
        DispatchPhase previous = DispatchPhase::Claimed;
        bool submitted = false;
        bool keep_draining = false;
        std::string reason_code;
        std::string reason_text;
    };
    struct RenewLeaseIoCommand {
        std::int64_t dispatch_attempt_id = 0;
        std::string claim_token;
    };
    using CoordinationIoCommand = std::variant<
        ClaimIoCommand,
        ActivateIoCommand,
        DrainingIoCommand,
        ReleaseIoCommand,
        RenewLeaseIoCommand>;

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
    void ActorLoop();
    void ScheduleActor();
    void ReconstructionLoop();
    void PersistenceLoop();
    void CancellationMutationLoop();
    void LeaseHeartbeatLoop();
    void CoordinationIoLoop();
    void WorkerMailboxLoop(const WorkerMailboxPtr& mailbox);

    WorkerMailboxPtr EnsureWorkerMailbox(
        const ReadyWorkerDispatchSnapshot& worker);
    WorkerMailboxPtr FindWorkerMailbox(
        std::size_t worker_id,
        std::uint64_t generation) const;
    void StopWorkerMailboxes();
    static WorksetAffinity AffinityOf(
        const savor::db::ExecutionWorksetContract& contract);
    static int AffinityScore(
        const WorkerMailbox& lane,
        const savor::db::ExecutionWorksetContract& contract);
    static void RefreshActualAffinity(
        WorkerMailbox& lane,
        const ReadyWorkerDispatchSnapshot& worker);
    void WakeScheduler(std::string reason, bool reset_backoff);
    bool Reconstruct(const DispatchPtr& dispatch);
    bool ValidateReconstruction(
        const savor::db::ClaimedPublishedWorkset& claimed,
        const savor::db::execution::programdb::
            WorksetReconstructionResult& reconstruction,
        std::string* error_out) const;

    void EnqueuePersistence(PersistenceEvent event);
    void RoutePersistenceEvent(PersistenceEvent event);
    void FlushBufferedEvidence(std::int64_t dispatch_attempt_id);
    bool ProcessPersistenceEvent(PersistenceEvent& event);
    bool HandleItemStarted(const ItemStartedEvent& event);
    bool HandleProgress(const ProgressEvent& event);
    bool HandleTerminal(TerminalEvent& event);
    bool HandleWorksetSummary(const SummaryEvent& event);
    bool PersistDispatchWorkerEvent(
        savor::db::WorkerExecutionEventMutation mutation,
        savor::db::WorkerExecutionEventMutationReceipt* receipt_out,
        std::string* error_out);
    bool StageDispatchTerminalBlob(
        std::int64_t workset_id,
        std::int64_t dispatch_attempt_id,
        std::int64_t job_id,
        std::uint64_t terminal_id,
        const std::vector<std::uint8_t>& envelope,
        savor::db::execution::WorkerResultBlobReference* blob_out,
        std::string* error_out);

    void EnqueueActor(
        CoordinatorEventKind kind,
        std::function<void()> action);
    void RunOnActorAndWait(std::function<void()> action);
    [[nodiscard]] bool IsActorThread() const;
    void EnqueueCoordinationIo(CoordinationIoCommand command);
    void HandleClaimIoCompletion(
        ClaimIoCommand command,
        std::vector<savor::db::ClaimedPublishedWorkset> claimed,
        std::string error);
    void HandleActivationIoCompletion(
        const ActivateIoCommand& command,
        bool called,
        savor::db::WorksetDispatchMutationReceipt receipt,
        std::string error);
    void HandleDrainingIoCompletion(
        const DrainingIoCommand& command,
        bool called,
        savor::db::WorksetDispatchMutationReceipt receipt,
        std::string error);
    void HandleReleaseIoCompletion(
        ReleaseIoCommand command,
        bool called,
        savor::db::WorksetDispatchMutationReceipt receipt,
        std::string error);
    bool AdmitPreparedDispatch(
        const DispatchPtr& dispatch,
        const WorkerMailboxPtr& mailbox,
        const ReadyWorkerDispatchSnapshot& worker);
    void HandleSubmissionCompletion(
        const WorkerMailboxPtr& mailbox,
        MailboxSubmissionCommand command,
        WorkerSubmitResult result);
    void ActivateSubmittedDispatch(
        const DispatchPtr& dispatch,
        const WorkerMailboxPtr& mailbox);
    void EnqueueWorkerControl(
        const WorkerExecutionTarget& target,
        WorkerControlCommand command);
    void ExecuteWorkerControl(
        const WorkerMailboxPtr& mailbox,
        WorkerControlCommand command);
    void HandleWorkerControlCompletion(
        const WorkerMailboxPtr& mailbox,
        WorkerControlCommand command,
        WorkerCommandResult result);
    void HandleResidenceCompletion(
        const WorkerMailboxPtr& mailbox,
        WorkerControlCommand command,
        bool called,
        savor::wrms::WorksetResidenceSnapshotV1 evidence,
        std::string error);
    void QueueCancellationMutation(PendingCancellationMutation mutation);
    void ApplyCancellationMutationReceipts(
        std::vector<PendingCancellationMutation> pending,
        std::vector<savor::db::JobCancellationReceipt> receipts);
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
    bool ProcessPendingDispatchReleases();
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
    void ClearWorkerIdentity(
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
    JobExecutionCoordinatorTelemetry BuildTelemetrySnapshot() const;
    std::vector<JobExecutionWorkerDispatchSnapshot>
        BuildWorkerDispatchSnapshots() const;
    void PublishSnapshots();

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
    std::atomic<bool> global_storage_unavailable_{false};
    std::atomic<bool> cancellation_admission_open_{false};
    std::atomic<bool> blob_store_ready_{false};

    std::thread actor_thread_;
    std::thread::id actor_thread_id_{};
    std::thread reconstruction_thread_;
    std::thread lease_heartbeat_thread_;
    std::thread cancellation_mutation_thread_;
    std::thread coordination_io_thread_;
    std::vector<std::thread> persistence_threads_;

    mutable std::mutex actor_mutex_;
    std::condition_variable actor_cv_;
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
    std::uint64_t actor_wakeup_generation_ = 0;
    std::string last_actor_wake_reason_;
    bool claim_in_flight_ = false;

    mutable std::mutex registry_mutex_;
    std::unordered_map<std::int64_t, DispatchPtr> dispatches_;

    mutable std::mutex mailboxes_mutex_;
    std::unordered_map<std::size_t, WorkerMailboxPtr> mailboxes_;

    std::deque<DispatchPtr> prepared_dispatches_;

    mutable std::mutex reconstruction_mutex_;
    std::condition_variable reconstruction_cv_;
    std::deque<ReconstructionItem> reconstruction_queue_;
    bool reconstruction_active_ = false;
    std::size_t reconstruction_high_water_ = 0;
    std::uint64_t reconstruction_total_duration_ms_ = 0;
    std::uint64_t reconstruction_max_duration_ms_ = 0;

    mutable std::mutex persistence_mutex_;
    std::condition_variable persistence_cv_;
    std::map<std::int64_t, PersistenceStream> persistence_streams_;
    std::size_t persistence_high_water_ = 0;
    std::unordered_map<std::int64_t, std::deque<PersistenceEvent>>
        buffered_worker_evidence_;

    PersistenceEvent* active_persistence_event_ = nullptr;

    std::deque<CoordinatorEvent> actor_events_;
    mutable std::mutex coordination_io_mutex_;
    std::condition_variable coordination_io_cv_;
    std::deque<CoordinationIoCommand> coordination_io_commands_;
    bool coordination_io_stop_ = false;
    mutable std::mutex snapshot_mutex_;
    JobExecutionCoordinatorTelemetry cached_telemetry_{};
    std::vector<JobExecutionWorkerDispatchSnapshot>
        cached_worker_dispatches_;
    std::vector<JobExecutionCoordinatorWarning> cached_warnings_;
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
    std::atomic<std::uint64_t> dispatch_persistence_attempts_{0};
    std::atomic<std::uint64_t> dispatch_persistence_events_{0};
    std::atomic<std::uint64_t> dispatch_persistence_failures_{0};
    std::atomic<std::uint64_t> dispatch_persistence_retries_{0};
    Clock::time_point next_snapshot_publish_at_{};
    std::atomic<std::uint64_t> blob_readiness_failures_{0};
    std::atomic<std::uint64_t> cancellations_delivered_{0};
    std::atomic<std::uint64_t>
        cancellation_precommit_holds_registered_{0};
    std::atomic<std::uint64_t>
        cancellation_precommit_holds_promoted_{0};
    std::atomic<std::uint64_t> committed_cancellations_indexed_{0};
    std::atomic<std::uint64_t> waiting_jobs_suppressed_by_sidecar_{0};
    std::atomic<std::uint64_t> fully_suppressed_worksets_avoided_{0};
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
    std::atomic<std::uint64_t> startup_interrupted_jobs_{0};
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
    const auto descriptor_program_kinds =
        program_kind_registry_->RegisteredProgramKinds();
    for (const auto program_kind : descriptor_program_kinds) {
        const auto* phase = savor::runtime::fullphase::
            ProductionRegistry().Find(program_kind);
        const auto* descriptor =
            program_kind_registry_->Find(program_kind);
        if (phase == nullptr || descriptor == nullptr
            || !descriptor->full_phase_identity.has_value()
            || *descriptor->full_phase_identity != phase->identity()) {
            if (error_out) {
                *error_out =
                    "execution descriptor does not identify its immutable FullPhase: "
                    + std::to_string(program_kind);
            }
            return false;
        }
    }
    const auto& pool_limits =
        worker_coordinator_->RuntimeContract().limits;
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
        || config_.prepared_worksets_per_ready_worker == 0
        || config_.persistence_io_threads == 0
        || config_.max_pending_evidence_per_dispatch == 0
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
    prepared_dispatches_.clear();
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
        std::lock_guard lock(actor_mutex_);
        actor_events_.clear();
    }
    {
        std::lock_guard lock(coordination_io_mutex_);
        coordination_io_commands_.clear();
        coordination_io_stop_ = false;
    }
    {
        std::lock_guard lock(snapshot_mutex_);
        cached_telemetry_ = {};
        cached_worker_dispatches_.clear();
        cached_warnings_.clear();
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
        std::lock_guard lock(actor_mutex_);
        claim_backoff_ = {};
        last_assigned_worker_ =
            std::numeric_limits<std::size_t>::max();
        next_full_claim_reconciliation_ =
            Clock::now() + std::chrono::seconds(30);
        actor_wakeup_generation_ = 0;
        last_actor_wake_reason_.clear();
        claim_in_flight_ = false;
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
    startup_interrupted_jobs_.fetch_add(
        static_cast<std::uint64_t>(dispatch_recovery.jobs_interrupted));

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
        std::lock_guard lock(actor_mutex_);
        execution_work_availability_ = *availability;
    }
    execution_work_subscription_ =
        execution_db_->SubscribeExecutionWorkAvailability(
            [this](
                const savor::db::ExecutionWorkAvailabilitySnapshot& snapshot) {
                EnqueueActor(
                    CoordinatorEventKind::AvailabilityChanged,
                    [this, snapshot]() {
                    const bool generation_changed = snapshot.generation
                        != execution_work_availability_.generation;
                    const bool ready_changed = snapshot.has_ready_worksets
                        != execution_work_availability_.has_ready_worksets;
                    execution_work_availability_ = snapshot;
                    if (generation_changed) {
                        ++availability_signal_wakeups_;
                    }
                    if (ready_changed || generation_changed) {
                        WakeScheduler(
                            "ready-workset-generation",
                            snapshot.has_ready_worksets);
                    }
                    });
            });

    stop_.store(false);
    quiescing_.store(false);
    invariant_paused_.store(false);
    global_storage_unavailable_.store(false);
    cancellation_admission_open_.store(false);
    blob_store_ready_.store(true);
    blob_readiness_retry_attempt_ = 0;
    next_blob_readiness_retry_at_ = {};
    ConfigureWorkerCallbacks();
    running_.store(true);

    persistence_threads_.reserve(config_.persistence_io_threads);
    for (std::size_t index = 0;
         index < config_.persistence_io_threads;
         ++index) {
        persistence_threads_.emplace_back(
            [this]() { PersistenceLoop(); });
    }
    cancellation_mutation_thread_ =
        std::thread([this]() { CancellationMutationLoop(); });
    reconstruction_thread_ =
        std::thread([this]() { ReconstructionLoop(); });
    coordination_io_thread_ =
        std::thread([this]() { CoordinationIoLoop(); });
    actor_thread_ = std::thread([this]() { ActorLoop(); });
    lease_heartbeat_thread_ =
        std::thread([this]() { LeaseHeartbeatLoop(); });
    if (error_out) error_out->clear();
    return true;
}

void JobExecutionCoordinator::Impl::Quiesce() {
    quiescing_.store(true);
    WakeScheduler("quiesce", false);
    {
        std::lock_guard lock(mailboxes_mutex_);
        for (const auto& [worker_id, lane] : mailboxes_) {
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
    actor_cv_.notify_all();
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
    if (!running_.load() && !actor_thread_.joinable()) {
        return;
    }
    Quiesce();
    if (execution_work_subscription_ != 0) {
        execution_db_->UnsubscribeExecutionWorkAvailability(
            execution_work_subscription_);
        execution_work_subscription_ = 0;
    }
    if (running_.load() && actor_thread_.joinable()) {
        std::string ignored;
        (void)WaitForLifecycleRequest(
            LifecycleRequestKind::ReleaseUnsubmitted,
            &ignored);
    }

    stop_.store(true);
    actor_cv_.notify_all();
    reconstruction_cv_.notify_all();
    persistence_cv_.notify_all();
    actor_cv_.notify_all();
    lease_heartbeat_cv_.notify_all();
    lifecycle_cv_.notify_all();

    // The actor is the only owner that can create a worker mailbox. Join it
    // before draining mailboxes so no new transport owner can appear.
    if (actor_thread_.joinable()) actor_thread_.join();
    {
        std::lock_guard lock(coordination_io_mutex_);
        coordination_io_stop_ = true;
    }
    coordination_io_cv_.notify_all();
    if (coordination_io_thread_.joinable()) {
        coordination_io_thread_.join();
    }
    StopWorkerMailboxes();

    if (reconstruction_thread_.joinable()) {
        reconstruction_thread_.join();
    }
    if (lease_heartbeat_thread_.joinable()) {
        lease_heartbeat_thread_.join();
    }
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
    if (!running_.load()) {
        user_paused_.store(paused);
        std::lock_guard lock(snapshot_mutex_);
        cached_telemetry_.user_admission_paused = paused;
        return;
    }
    if (!IsActorThread()) {
        RunOnActorAndWait([this, paused]() { SetPaused(paused); });
        return;
    }
    user_paused_.store(paused);
    WakeScheduler(paused ? "user-pause" : "user-resume", false);
    {
        std::lock_guard lock(mailboxes_mutex_);
        for (const auto& [worker_id, lane] : mailboxes_) {
            (void)worker_id;
            lane->cv.notify_all();
        }
    }
    PublishSnapshots();
}

bool JobExecutionCoordinator::Impl::IsPaused() const noexcept {
    return user_paused_.load() || invariant_paused_.load();
}

bool JobExecutionCoordinator::Impl::IsRunning() const noexcept {
    return running_.load();
}

bool JobExecutionCoordinator::Impl::ClearInvariantPause() {
    if (!running_.load()) {
        const bool changed = invariant_paused_.exchange(false);
        std::lock_guard lock(snapshot_mutex_);
        cached_telemetry_.invariant_admission_paused = false;
        return changed;
    }
    if (!IsActorThread()) {
        bool changed = false;
        RunOnActorAndWait([this, &changed]() {
            changed = ClearInvariantPause();
        });
        return changed;
    }
    const bool changed = invariant_paused_.exchange(false);
    if (changed) {
        WakeScheduler("invariant-resume", false);
        std::lock_guard lock(mailboxes_mutex_);
        for (const auto& [worker_id, lane] : mailboxes_) {
            (void)worker_id;
            lane->cv.notify_all();
        }
    }
    PublishSnapshots();
    return changed;
}

void JobExecutionCoordinator::Impl::RegisterCancellationCommitPending(
    std::uint64_t hold_id,
    const std::vector<savor::db::ExecutionCancellationRequestSpec>&
        cancellations) {
    if (!IsActorThread()) {
        RunOnActorAndWait([this, hold_id, cancellations]() {
            RegisterCancellationCommitPending(hold_id, cancellations);
        });
        return;
    }
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
    std::lock_guard lock(mailboxes_mutex_);
    for (const auto& [_, lane] : mailboxes_) lane->cv.notify_all();
}

void JobExecutionCoordinator::Impl::RegisterCommittedCancellations(
    std::uint64_t hold_id,
    const std::vector<savor::db::CommittedJobCancellation>&
        cancellations) {
    if (!IsActorThread()) {
        RunOnActorAndWait([this, hold_id, cancellations]() {
            RegisterCommittedCancellations(hold_id, cancellations);
        });
        return;
    }
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
    std::lock_guard lock(mailboxes_mutex_);
    for (const auto& [_, lane] : mailboxes_) lane->cv.notify_all();
}

void JobExecutionCoordinator::Impl::OpenCancellationAdmission() {
    if (!IsActorThread()) {
        RunOnActorAndWait([this]() { OpenCancellationAdmission(); });
        return;
    }
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
    {
        std::lock_guard lock(mailboxes_mutex_);
        for (const auto& [_, lane] : mailboxes_) lane->cv.notify_all();
    }
    PublishSnapshots();
}

void JobExecutionCoordinator::Impl::WakeScheduler(
    std::string reason,
    bool reset_backoff) {
    {
        std::lock_guard lock(actor_mutex_);
        ++actor_wakeup_generation_;
        last_actor_wake_reason_ = std::move(reason);
        if (reset_backoff) {
            claim_backoff_ = {};
        }
    }
    ++scheduler_wakeups_;
    actor_cv_.notify_all();
}

bool JobExecutionCoordinator::Impl::IsDispatchAdmissionPaused()
    const noexcept {
    return quiescing_.load() || user_paused_.load()
        || invariant_paused_.load()
        || global_storage_unavailable_.load()
        || !cancellation_admission_open_.load();
}

JobExecutionCoordinatorTelemetry
JobExecutionCoordinator::Impl::SnapshotTelemetry() const {
    std::lock_guard lock(snapshot_mutex_);
    return cached_telemetry_;
}

JobExecutionCoordinatorTelemetry
JobExecutionCoordinator::Impl::BuildTelemetrySnapshot() const {
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
    telemetry.dispatch_persistence_attempts =
        dispatch_persistence_attempts_.load();
    telemetry.dispatch_persistence_events =
        dispatch_persistence_events_.load();
    telemetry.dispatch_persistence_failures =
        dispatch_persistence_failures_.load();
    telemetry.dispatch_persistence_retries =
        dispatch_persistence_retries_.load();
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
    telemetry.fully_suppressed_worksets_avoided =
        fully_suppressed_worksets_avoided_.load();
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
    telemetry.startup_interrupted_jobs = startup_interrupted_jobs_.load();
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
    telemetry.cancellation_admission_open =
        cancellation_admission_open_.load();
    telemetry.user_admission_paused = user_paused_.load();
    telemetry.invariant_admission_paused = invariant_paused_.load();
    telemetry.global_storage_unavailable =
        global_storage_unavailable_.load();
    {
        std::lock_guard lock(actor_mutex_);
        telemetry.last_scheduler_wake_reason =
            last_actor_wake_reason_;
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
            if (stream.active) ++telemetry.active_worker_streams;
            if (stream.active) {
                ++telemetry.persistence_streams_in_flight;
            } else if (stream.retry_at > now) {
                ++telemetry.persistence_streams_retrying;
            } else if (!stream.events.empty()) {
                ++telemetry.persistence_streams_ready;
            }
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
    telemetry.pending_cancellation_mutations =
        pending_cancellation_mutations_.load();
    {
        std::lock_guard lock(mailboxes_mutex_);
        for (const auto& [worker_id, lane] : mailboxes_) {
            (void)worker_id;
            std::lock_guard lane_lock(lane->mutex);
            telemetry.mailbox_command_depth +=
                lane->controls.size() + lane->submissions.size();
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
                } else if (command.kind == WorkerControlKind::CancelItem
                    || command.kind == WorkerControlKind::CancelWorkset) {
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
            if (record->phase == DispatchPhase::Prepared) {
                ++telemetry.prepared_worksets;
                ++telemetry.global_prepared_worksets;
            }
            if (record->phase == DispatchPhase::Draining) {
                ++telemetry.draining_worksets;
            }
        }
    }
    {
        std::lock_guard lock(error_mutex_);
        telemetry.last_error = last_error_;
    }
    return telemetry;
}

std::vector<JobExecutionWorkerDispatchSnapshot>
JobExecutionCoordinator::Impl::SnapshotWorkerDispatches() const {
    std::lock_guard lock(snapshot_mutex_);
    return cached_worker_dispatches_;
}

std::vector<JobExecutionWorkerDispatchSnapshot>
JobExecutionCoordinator::Impl::BuildWorkerDispatchSnapshots() const {
    std::vector<JobExecutionWorkerDispatchSnapshot> snapshots;
    std::vector<WorkerMailboxPtr> lanes;
    {
        std::lock_guard lock(mailboxes_mutex_);
        for (const auto& [worker_id, lane] : mailboxes_) {
            (void)worker_id;
            lanes.push_back(lane);
        }
    }
    std::sort(
        lanes.begin(),
        lanes.end(),
        [](const WorkerMailboxPtr& lhs, const WorkerMailboxPtr& rhs) {
            return lhs->worker_id < rhs->worker_id;
        });
    for (const auto& lane : lanes) {
        JobExecutionWorkerDispatchSnapshot snapshot{};
        {
            std::lock_guard lock(lane->mutex);
            snapshot.worker_id = lane->worker_id;
            snapshot.process_generation = lane->generation;
            snapshot.submitting_dispatch_attempt_id =
                lane->submitting;
            snapshot.active_dispatch_attempt_id = lane->active;
            snapshot.state = lane->active.has_value()
                ? JobExecutionWorkerDispatchState::Active
                : lane->submitting.has_value()
                ? JobExecutionWorkerDispatchState::Submitting
                : JobExecutionWorkerDispatchState::Ready;
            snapshot.mailbox_depth =
                lane->controls.size() + lane->submissions.size();
            for (const auto& command : lane->controls) {
                if (command.kind == WorkerControlKind::Acknowledge) {
                    ++snapshot.pending_acknowledgements;
                } else if (command.kind == WorkerControlKind::CancelItem
                    || command.kind == WorkerControlKind::CancelWorkset) {
                    ++snapshot.pending_cancellations;
                }
            }
            snapshot.actual_execution_affinity_key =
                lane->actual_execution_key;
        }
        if (snapshot.active_dispatch_attempt_id.has_value()) {
            const auto dispatch = FindDispatch(
                *snapshot.active_dispatch_attempt_id);
            if (dispatch) {
                std::lock_guard lock(dispatch->mutex);
                if (dispatch->phase == DispatchPhase::Draining) {
                    snapshot.state =
                        JobExecutionWorkerDispatchState::Draining;
                }
            }
        }
        {
            std::lock_guard lock(persistence_mutex_);
            const auto dispatch_id = snapshot.active_dispatch_attempt_id
                .has_value()
                ? snapshot.active_dispatch_attempt_id
                : snapshot.submitting_dispatch_attempt_id;
            const auto found = dispatch_id.has_value()
                ? persistence_streams_.find(*dispatch_id)
                : persistence_streams_.end();
            if (found != persistence_streams_.end()) {
                snapshot.persistence_queue_depth =
                    found->second.events.size();
                snapshot.persistence_retry_attempt =
                    found->second.retry_attempt;
                snapshot.persistence_diagnostic =
                    found->second.last_error;
                snapshot.persistence_state =
                    global_storage_unavailable_.load()
                    ? JobExecutionDispatchPersistenceState::GloballyBlocked
                    : found->second.active
                    ? JobExecutionDispatchPersistenceState::InFlight
                    : found->second.retry_at > Clock::now()
                    ? JobExecutionDispatchPersistenceState::RetryPending
                    : JobExecutionDispatchPersistenceState::Ready;
            }
        }
        snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
}

std::vector<JobExecutionCoordinatorWarning>
JobExecutionCoordinator::Impl::SnapshotWarnings() const {
    std::lock_guard lock(snapshot_mutex_);
    return cached_warnings_;
}

void JobExecutionCoordinator::Impl::PublishSnapshots() {
    auto telemetry = BuildTelemetrySnapshot();
    auto worker_dispatches = BuildWorkerDispatchSnapshots();
    std::vector<JobExecutionCoordinatorWarning> warnings;
    {
        std::lock_guard lock(warning_mutex_);
        warnings.assign(warnings_.begin(), warnings_.end());
    }
    std::lock_guard lock(snapshot_mutex_);
    cached_telemetry_ = std::move(telemetry);
    cached_worker_dispatches_ = std::move(worker_dispatches);
    cached_warnings_ = std::move(warnings);
}

void JobExecutionCoordinator::Impl::ConfigureWorkerCallbacks() {
    worker_coordinator_->SetCallbacks(
        {
            .availability_changed = [this]() {
                EnqueueActor(
                    CoordinatorEventKind::AvailabilityChanged,
                    [this]() {
                    WakeScheduler("worker-availability", false);
                    });
            },
            .workset_state =
                [this](
                    const WorkerCoordinatorEventContext& source,
                    const savor::wrms::WorksetStatePayload& payload) {
                    EnqueueActor(
                        CoordinatorEventKind::WorkerEvidence,
                        [this, source, payload]() {
                            HandleWorksetState(source, payload);
                        });
                },
            .item_started =
                [this](
                    const WorkerCoordinatorEventContext& source,
                    const savor::wrms::WorksetItemStartedPayload& payload) {
                    EnqueueActor(
                        CoordinatorEventKind::WorkerEvidence,
                        [this, source, payload]() {
                        RoutePersistenceEvent(
                            {
                                .worker_id = source.worker_id,
                                .generation = source.process_generation,
                                .enqueued_at = Clock::now(),
                                .payload = ItemStartedEvent{source, payload},
                            });
                        });
                },
            .item_progress =
                [this](
                    const WorkerCoordinatorEventContext& source,
                    const savor::wrms::InvocationProgressPayload& payload) {
                    EnqueueActor(
                        CoordinatorEventKind::WorkerEvidence,
                        [this, source, payload]() {
                        RoutePersistenceEvent(
                            {
                                .worker_id = source.worker_id,
                                .generation = source.process_generation,
                                .enqueued_at = Clock::now(),
                                .payload = ProgressEvent{source, payload},
                            });
                        });
                },
            .item_terminal =
                [this](
                    const savor::runtime::
                        DurableWorkerTerminalEnvelope& envelope) {
                    EnqueueActor(
                        CoordinatorEventKind::WorkerEvidence,
                        [this, envelope]() {
                        ++worker_terminals_observed_;
                        RoutePersistenceEvent(
                            {
                                .worker_id = envelope.worker_id,
                                .generation = envelope.process_generation,
                                .enqueued_at = Clock::now(),
                                .payload = TerminalEvent{envelope, 0},
                            });
                        });
                },
            .credits =
                [this](
                    const WorkerCoordinatorEventContext& source,
                    const savor::wrms::WorksetCreditsPayload&) {
                    EnqueueActor(
                        CoordinatorEventKind::AvailabilityChanged,
                        [this, source]() {
                        WakeScheduler("worker-credits", false);
                        });
                },
            .workset_summary =
                [this](
                    const WorkerCoordinatorEventContext& source,
                    const savor::wrms::WorksetSummaryPayload& payload) {
                    EnqueueActor(
                        CoordinatorEventKind::WorkerEvidence,
                        [this, source, payload]() {
                        RoutePersistenceEvent(
                            {
                                .worker_id = source.worker_id,
                                .generation = source.process_generation,
                                .enqueued_at = Clock::now(),
                                .payload = SummaryEvent{source, payload},
                            });
                        });
                },
            .worker_unavailable =
                [this](const WorkerUnavailableEvent& event) {
                    EnqueueActor(
                        CoordinatorEventKind::WorkerUnavailable,
                        [this, event]() mutable {
                            HandleWorkerUnavailable(std::move(event));
                        });
                },
        });
}

JobExecutionCoordinator::Impl::WorkerMailboxPtr
JobExecutionCoordinator::Impl::FindWorkerMailbox(
    std::size_t worker_id,
    std::uint64_t generation) const {
    std::lock_guard lock(mailboxes_mutex_);
    const auto found = mailboxes_.find(worker_id);
    if (found == mailboxes_.end()) return {};
    std::lock_guard lane_lock(found->second->mutex);
    return found->second->generation == generation
        ? found->second
        : WorkerMailboxPtr{};
}

JobExecutionCoordinator::Impl::WorkerMailboxPtr
JobExecutionCoordinator::Impl::EnsureWorkerMailbox(
    const ReadyWorkerDispatchSnapshot& worker) {
    WorkerMailboxPtr old_lane;
    WorkerMailboxPtr lane;
    {
        std::lock_guard lock(mailboxes_mutex_);
        const auto found = mailboxes_.find(worker.worker_id);
        if (found != mailboxes_.end()) {
            std::lock_guard lane_lock(found->second->mutex);
            if (found->second->generation
                == worker.process_generation) {
                return found->second;
            }
            old_lane = found->second;
            mailboxes_.erase(found);
        }
        lane = std::make_shared<WorkerMailbox>();
        lane->worker_id = worker.worker_id;
        lane->generation = worker.process_generation;
        mailboxes_.emplace(worker.worker_id, lane);
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
                EnqueueActor(
                    CoordinatorEventKind::MailboxCompleted,
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
        EnqueueActor(
            CoordinatorEventKind::WorkerUnavailable,
            [this, replaced]() mutable {
                HandleWorkerUnavailable(std::move(replaced));
            });
    }
    lane->thread =
        std::thread([this, lane]() { WorkerMailboxLoop(lane); });
    return lane;
}

void JobExecutionCoordinator::Impl::StopWorkerMailboxes() {
    std::vector<WorkerMailboxPtr> lanes;
    {
        std::lock_guard lock(mailboxes_mutex_);
        for (auto& [worker_id, lane] : mailboxes_) {
            (void)worker_id;
            lanes.push_back(lane);
        }
        mailboxes_.clear();
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
    const savor::db::ExecutionWorksetContract& contract) {
    return {
        .program_package_sha256 = contract.program_package_sha256,
        .execution_key = contract.execution_affinity_key,
    };
}

int JobExecutionCoordinator::Impl::AffinityScore(
    const WorkerMailbox& lane,
    const savor::db::ExecutionWorksetContract& contract) {
    const bool declares_affinity =
        !contract.program_package_sha256.empty()
        || contract.execution_affinity_key.has_value();
    if (!declares_affinity) return 0;

    int score = 0;
    if (lane.actual_program_package_sha256
        == std::optional<std::string>(
            contract.program_package_sha256)) {
        score += 1;
    }
    if (contract.execution_affinity_key.has_value()
        && lane.actual_execution_key == contract.execution_affinity_key) {
        score += 2;
    }
    return score;
}

void JobExecutionCoordinator::Impl::RefreshActualAffinity(
    WorkerMailbox& lane,
    const ReadyWorkerDispatchSnapshot& worker) {
    lane.actual_program_package_sha256 =
        worker.warm_program_package_sha256;
    lane.actual_execution_key = worker.warm_execution_key_sha256;
}

void JobExecutionCoordinator::Impl::ScheduleActor() {
    if (IsDispatchAdmissionPaused()) return;

    const auto actor_now = Clock::now();
    std::vector<DispatchPtr> activation_retries;
    {
        std::lock_guard lock(registry_mutex_);
        for (const auto& [_, dispatch] : dispatches_) {
            std::lock_guard record_lock(dispatch->mutex);
            if (dispatch->phase == DispatchPhase::Activating
                && dispatch->activation_retry_at != Clock::time_point{}
                && dispatch->activation_retry_at <= actor_now) {
                dispatch->activation_retry_at = {};
                activation_retries.push_back(dispatch);
            }
        }
    }
    for (const auto& dispatch : activation_retries) {
        const auto mailbox = FindWorkerMailbox(
            dispatch->target.worker_id,
            dispatch->target.process_generation);
        if (mailbox) ActivateSubmittedDispatch(dispatch, mailbox);
    }

    auto workers = worker_coordinator_->SnapshotReadyWorkers();
    workers.erase(
        std::remove_if(
            workers.begin(),
            workers.end(),
            [](const ReadyWorkerDispatchSnapshot& worker) {
                return !worker.accepting_workset
                    || !savor::runtime::IsNormalWorkerMode(worker.mode);
            }),
        workers.end());
    std::ranges::sort(workers, {},
        [](const ReadyWorkerDispatchSnapshot& worker) {
            return worker.worker_id;
        });

    for (const auto& worker : workers) {
        const auto mailbox = EnsureWorkerMailbox(worker);
        if (!mailbox) continue;
        std::lock_guard lock(mailbox->mutex);
        RefreshActualAffinity(*mailbox, worker);
    }

    // Prepared work is globally owned. Bind it to a physical worker only at
    // the exact submission fence.
    for (const auto& worker : workers) {
        const auto mailbox = FindWorkerMailbox(
            worker.worker_id,
            worker.process_generation);
        if (!mailbox) continue;
        {
            std::lock_guard lock(mailbox->mutex);
            if (mailbox->stop || mailbox->submitting.has_value()
                || mailbox->active.has_value()
                || !mailbox->submissions.empty()) {
                continue;
            }
        }

        auto best = prepared_dispatches_.end();
        int best_affinity = std::numeric_limits<int>::min();
        for (auto it = prepared_dispatches_.begin();
             it != prepared_dispatches_.end(); ++it) {
            const auto& dispatch = *it;
            std::lock_guard record_lock(dispatch->mutex);
            if (dispatch->phase != DispatchPhase::Prepared
                || dispatch->definition.items.size()
                    > worker.available_item_credits) {
                continue;
            }
            std::lock_guard mailbox_lock(mailbox->mutex);
            const auto score = AffinityScore(
                *mailbox,
                dispatch->claimed.contract);
            if (score > best_affinity) {
                best_affinity = score;
                best = it;
            }
        }
        if (best == prepared_dispatches_.end()) continue;
        auto dispatch = *best;
        if (AdmitPreparedDispatch(dispatch, mailbox, worker)) {
            prepared_dispatches_.erase(best);
        }
    }

    const auto preparation_capacity = workers.size()
        * config_.prepared_worksets_per_ready_worker;
    std::size_t unsubmitted = 0;
    {
        std::lock_guard lock(registry_mutex_);
        for (const auto& [_, dispatch] : dispatches_) {
            std::lock_guard record_lock(dispatch->mutex);
            switch (dispatch->phase) {
            case DispatchPhase::Claimed:
            case DispatchPhase::Reconstructing:
            case DispatchPhase::Prepared:
            case DispatchPhase::Submitting:
            case DispatchPhase::Activating:
                ++unsubmitted;
                break;
            default:
                break;
            }
        }
    }
    if (unsubmitted >= preparation_capacity || workers.empty()) return;

    const auto now = Clock::now();
    bool ready_signal = false;
    bool reconciliation_due = false;
    bool backoff_elapsed = false;
    {
        std::lock_guard lock(actor_mutex_);
        ready_signal = execution_work_availability_.has_ready_worksets;
        if (next_full_claim_reconciliation_ == Clock::time_point{}) {
            next_full_claim_reconciliation_ = now + std::chrono::seconds(30);
        }
        reconciliation_due = now >= next_full_claim_reconciliation_;
        backoff_elapsed = claim_backoff_.next_attempt == Clock::time_point{}
            || claim_backoff_.next_attempt <= now;
    }
    if (!ready_signal && !reconciliation_due && !backoff_elapsed) return;

    if (claim_in_flight_) return;
    const auto requested = preparation_capacity - unsubmitted;
    ++claim_batches_;
    claim_in_flight_ = true;
    EnqueueCoordinationIo(ClaimIoCommand{
        .batch_nonce = NextToken("workset-batch"),
        .requested_workset_count = requested,
        .reconciliation_due = reconciliation_due,
        .ready_signal = ready_signal,
    });
}

void JobExecutionCoordinator::Impl::HandleClaimIoCompletion(
    ClaimIoCommand command,
    std::vector<savor::db::ClaimedPublishedWorkset> claimed,
    std::string error) {
    claim_in_flight_ = false;
    const auto now = Clock::now();
    if (!error.empty()) RecordError(std::move(error));
    const auto requested = command.requested_workset_count;
    if (claimed.size() > requested) {
        for (auto& row : claimed) {
            auto dispatch = std::make_shared<DispatchRecord>();
            dispatch->claimed = std::move(row);
            dispatch->phase = DispatchPhase::Claimed;
            InsertDispatch(dispatch);
            (void)ReleaseDispatch(
                dispatch,
                "BATCH_CLAIM_OVERFLOW",
                "DB returned more claims than global preparation capacity");
        }
        claimed.clear();
        invariant_paused_.store(true);
        RecordError("batch claim returned more worksets than requested");
    }

    if (claimed.empty()) {
        ++empty_claim_batches_;
        ++claim_backoff_.empty_attempts;
        const std::uint64_t delays[] = {1000, 5000, 30000};
        const auto index = std::min<std::size_t>(
            claim_backoff_.empty_attempts - 1,
            std::size(delays) - 1);
        claim_backoff_.delay_ms = delays[index];
        claim_backoff_.next_attempt = now
            + std::chrono::milliseconds(claim_backoff_.delay_ms);
    } else {
        ++successful_claim_batches_;
        worksets_claimed_.fetch_add(claimed.size());
        if (command.reconciliation_due && !command.ready_signal) {
            reconciliation_claims_.fetch_add(claimed.size());
        }
        claim_backoff_ = {};
    }
    if (command.reconciliation_due) {
        next_full_claim_reconciliation_ = now + std::chrono::seconds(30);
    }

    if (IsDispatchAdmissionPaused()) {
        for (auto& row : claimed) {
            auto dispatch = std::make_shared<DispatchRecord>();
            dispatch->claimed = std::move(row);
            dispatch->phase = DispatchPhase::Claimed;
            InsertDispatch(dispatch);
            (void)ReleaseDispatch(
                dispatch,
                "COORDINATOR_QUIESCED",
                "claim completed after work admission closed");
        }
        return;
    }

    for (auto& row : claimed) {
        auto dispatch = std::make_shared<DispatchRecord>();
        dispatch->claimed = std::move(row);
        dispatch->target = {};
        dispatch->phase = DispatchPhase::Reconstructing;
        InsertDispatch(dispatch);
        {
            std::lock_guard lock(reconstruction_mutex_);
            reconstruction_queue_.push_back({dispatch, Clock::now()});
            reconstruction_high_water_ = std::max(
                reconstruction_high_water_,
                reconstruction_queue_.size());
        }
        reconstruction_cv_.notify_one();
    }
    WakeScheduler("claim-complete", false);
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
        savor::runtime::derived::WorksetDerivedStateBindingV1
            derived_state;
        std::optional<savor::runtime::WorksetCaptureBindingV1>
            capture;
        savor::runtime::progress::ProgressPlanV1 progress_plan;
        const auto derived_decoded =
            savor::runtime::DecodeWorksetDerivedStateBindingV1(
                claimed.derived_state.binding_payload,
                derived_state);
        const auto capture_decoded =
            savor::runtime::DecodeWorksetCaptureBindingV1(
                claimed.observation.capture_binding_payload,
                capture);
        const auto progress_decoded =
            savor::runtime::DecodeProgressPlanV1(
                claimed.observation.progress_plan_payload,
                progress_plan);
        const std::string capture_hash = capture
            ? capture->content_sha256
            : savor::runtime::EmptyWorksetCaptureBindingHashV1();
        if (!derived_decoded || !capture_decoded || !progress_decoded ||
            derived_state.content_sha256 !=
                claimed.derived_state.binding_sha256 ||
            capture_hash !=
                claimed.observation.capture_binding_sha256 ||
            progress_plan.content_sha256 !=
                claimed.observation.progress_plan_sha256) {
            error = !derived_decoded
                ? derived_decoded.message
                : !capture_decoded
                ? capture_decoded.message
                : !progress_decoded
                ? progress_decoded.message
                : "durable workset observation hashes do not match their payloads";
        }
        savor::db::execution::programdb::WorksetReconstructionContext
            context{};
        context.workset_id = claimed.workset_id;
        context.dispatch_attempt_id = claimed.dispatch_attempt_id;
        context.workflow_step_id = claimed.workflow_step_id;
        context.job_set_id = claimed.job_set_id;
        context.dispatch_token = claimed.claim_token;
        context.contract_key = claimed.contract.contract_key;
        context.state_compatibility = config_.state_compatibility;
        context.derived_state = std::move(derived_state);
        context.capture = std::move(capture);
        context.progress_plan = std::move(progress_plan);
        context.items.reserve(claimed.items.size());
        for (const auto& item : claimed.items) {
            context.items.push_back(
                {
                    .job_id = item.job_id,
                    .workset_item_ordinal = item.workset_item_ordinal,
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
            if (!error.empty()) {
                reconstruction.reset();
            } else {
            reconstruction =
                descriptor->workset_reconstruction->Reconstruct(
                    context,
                    &error);
            }
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error =
                "workset reconstruction threw an unknown exception";
        }
    }

    if (!reconstruction.has_value()
        || !ValidateReconstruction(
            claimed,
            *reconstruction,
            &error)) {
        EnqueueActor(
            CoordinatorEventKind::ReconstructionCompleted,
            [this, dispatch, error = std::move(error)]() mutable {
                PauseForInvariant(
                    dispatch,
                    "Workset reconstruction invariant failed",
                    error.empty() ? "descriptor returned no valid workset"
                                  : std::move(error));
            });
        return false;
    }

    auto definition = std::move(reconstruction->workset);
    EnqueueActor(
        CoordinatorEventKind::ReconstructionCompleted,
        [this, dispatch, definition = std::move(definition)]() mutable {
            {
                std::lock_guard record_lock(dispatch->mutex);
                if (dispatch->phase != DispatchPhase::Reconstructing) {
                    return;
                }
                dispatch->definition = std::move(definition);
                dispatch->item_index_by_id.clear();
                for (std::size_t index = 0;
                     index < dispatch->definition.items.size();
                     ++index) {
                    dispatch->item_index_by_id.emplace(
                        dispatch->definition.items[index].item_id.value(),
                        index);
                }
                dispatch->phase = DispatchPhase::Prepared;
            }
            prepared_dispatches_.push_back(dispatch);
            ++worksets_reconstructed_;
            WakeScheduler("reconstruction-complete", false);
        });
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
    const auto& contract = claimed.contract;
    const auto& derived_binding = claimed.derived_state;
    const auto& observation = claimed.observation;
    const auto& key = reconstruction.workset.execution_key;
    std::optional<savor::runtime::WorksetCaptureBindingV1>
        durable_capture;
    savor::runtime::derived::WorksetDerivedStateBindingV1
        durable_derived_state;
    savor::runtime::progress::ProgressPlanV1 durable_progress;
    if (!savor::runtime::DecodeWorksetDerivedStateBindingV1(
            derived_binding.binding_payload,
            durable_derived_state) ||
        !savor::runtime::DecodeWorksetCaptureBindingV1(
            observation.capture_binding_payload,
            durable_capture) ||
        !savor::runtime::DecodeProgressPlanV1(
            observation.progress_plan_payload,
            durable_progress)) {
        return fail(
            "durable workset observation binding could not be decoded");
    }
    if (key.module.canonical_id
            != contract.module_canonical_id
        || key.module.revision
            != static_cast<std::uint32_t>(
                contract.module_version)
        || key.module.canonical_hash
            != contract.module_sha256
        || key.entrypoint != contract.entrypoint
        || key.verified_dependency_sha256
            != contract.verified_dependency_sha256
        || key.runtime_profile_sha256
            != contract.runtime_profile_sha256
        || key.program_package_sha256
            != contract.program_package_sha256
        || key.derived_state_binding_sha256
            != derived_binding.binding_sha256
        || key.capture_binding_sha256
            != observation.capture_binding_sha256
        || key.progress_plan_sha256
            != observation.progress_plan_sha256
        || reconstruction.workset.derived_state !=
            durable_derived_state
        || reconstruction.workset.capture !=
            durable_capture
        || reconstruction.workset.progress_plan != durable_progress
        || (contract.execution_affinity_key.has_value()
            && key.canonical_sha256
                != *contract.execution_affinity_key)
        ) {
        return fail(
            "reconstructed workset changed its durable contract "
            "metadata");
    }
    if (contract.estimated_payload_bytes > 0
        && reconstruction.workset.encoded_size_bytes
            > contract.estimated_payload_bytes) {
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
        if (durable.workset_item_ordinal != index
            || reconstruction.ordered_job_ids[index]
                != durable.job_id
            || runtime_item.ordinal != index
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
            == WorkerSubmitDisposition::DuplicateWorkset) {
        return true;
    }
    if (result.disposition
        != WorkerSubmitDisposition::DefiniteRejected) {
        return false;
    }
    switch (result.rejection_code) {
    case savor::wrms::RejectionCode::Unsupported:
    case savor::wrms::RejectionCode::InvalidArgument:
    case savor::wrms::RejectionCode::ProgramPackageRejected:
        return true;
    default:
        return false;
    }
}

bool JobExecutionCoordinator::Impl::AdmitPreparedDispatch(
    const DispatchPtr& dispatch,
    const WorkerMailboxPtr& mailbox,
    const ReadyWorkerDispatchSnapshot& worker) {
    if (!dispatch || !mailbox) return false;

    savor::runtime::WorkerWorksetDefinition definition;
    savor::runtime::InitialWorksetCancellationSidecarV1 sidecar;
    std::vector<savor::db::CommittedJobCancellation> cancellations;
    {
        std::lock_guard cancellation_lock(cancellation_index_mutex_);
        std::lock_guard record_lock(dispatch->mutex);
        std::lock_guard mailbox_lock(mailbox->mutex);
        if (dispatch->phase != DispatchPhase::Prepared
            || mailbox->stop
            || mailbox->generation != worker.process_generation
            || mailbox->submitting.has_value()
            || mailbox->active.has_value()
            || !mailbox->submissions.empty()) {
            return false;
        }
        definition = dispatch->definition;
        sidecar.workset_id = definition.workset_id;
        for (std::size_t index = 0;
             index < dispatch->claimed.items.size(); ++index) {
            const auto job_id = dispatch->claimed.items[index].job_id;
            if (pending_cancellation_hold_counts_.contains(job_id)) {
                return false;
            }
            const auto found = committed_cancellations_by_job_.find(job_id);
            if (found == committed_cancellations_by_job_.end()) continue;
            cancellations.push_back(found->second.cancellation);
            if (index < definition.items.size()) {
                sidecar.item_ids.push_back(definition.items[index].item_id);
            }
        }
        std::ranges::sort(sidecar.item_ids, {},
            [](const auto item_id) { return item_id.value(); });

        dispatch->target = {
            .worker_id = worker.worker_id,
            .process_generation = worker.process_generation,
        };
        dispatch->phase = DispatchPhase::Submitting;
        dispatch->cancellation_sidecar_sha256 =
            savor::runtime::ComputeInitialWorksetCancellationSidecarSha256(
                sidecar);
        dispatch->frozen_submission_sidecar = sidecar;
        dispatch->frozen_submission_cancellations = cancellations;
        dispatch->initially_suppressed_jobs.clear();
        for (const auto& cancellation : cancellations) {
            dispatch->initially_suppressed_jobs.insert(cancellation.job_id);
        }
        mailbox->submitting = dispatch->claimed.dispatch_attempt_id;
        mailbox->has_submitted_workset = true;
    }

    if (sidecar.item_ids.size() == definition.items.size()) {
        ++fully_suppressed_worksets_avoided_;
        {
            std::lock_guard mailbox_lock(mailbox->mutex);
            mailbox->submitting.reset();
        }
        for (const auto& cancellation : cancellations) {
            const std::string resolution_code =
                cancellation.terminal_disposition == "SUPERSEDED"
                ? "SUPERSEDED_BEFORE_WORKER"
                : cancellation.terminal_disposition == "FAILED"
                ? "FAILURE_CASCADE_BEFORE_WORKER"
                : "WORKFLOW_CANCELED_BEFORE_WORKER";
            QueueCancellationMutation({
                .mutation = savor::db::JobCancellationOutcomeCommand{
                    .kind = savor::db::JobCancellationOutcomeKind::
                        ResolveWithoutWorker,
                    .cancellation_request_id =
                        cancellation.cancellation_request_id,
                    .job_id = cancellation.job_id,
                    .dispatch_attempt_id =
                        dispatch->claimed.dispatch_attempt_id,
                    .claim_token = dispatch->claimed.claim_token,
                    .resolution_code = resolution_code,
                    .requested_by = "job_execution_coordinator",
                },
                .requested_resolution_code = resolution_code,
                .dispatch = dispatch,
                .initial_suppression = true,
            });
        }
        WakeScheduler("fully-canceled-workset", false);
        return true;
    }

    MailboxSubmissionCommand command{
        .command_id = token_sequence_.fetch_add(1),
        .target = dispatch->target,
        .dispatch = dispatch,
        .definition = std::move(definition),
        .sidecar = std::move(sidecar),
        .enqueued_at = Clock::now(),
    };
    {
        std::lock_guard mailbox_lock(mailbox->mutex);
        mailbox->submissions.push_back(std::move(command));
    }
    ++submission_calls_started_;
    sidecar_items_submitted_.fetch_add(
        dispatch->frozen_submission_sidecar->item_ids.size());
    mailbox->cv.notify_all();
    return true;
}

void JobExecutionCoordinator::Impl::HandleSubmissionCompletion(
    const WorkerMailboxPtr& mailbox,
    MailboxSubmissionCommand command,
    WorkerSubmitResult submitted) {
    const auto& dispatch = command.dispatch;
    if (!dispatch || !mailbox) return;

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
        // An exact replay is the residence confirmation protocol: the worker
        // must return AlreadyAdmitted with the original sidecar receipt.
        {
            std::lock_guard mailbox_lock(mailbox->mutex);
            if (!mailbox->stop
                && mailbox->generation
                    == command.target.process_generation) {
                command.enqueued_at = Clock::now();
                mailbox->submissions.push_front(std::move(command));
                mailbox->cv.notify_all();
                return;
            }
        }
    }

    if (!submitted.submitted()) {
        const bool invariant_rejection =
            IsInvariantSubmissionRejection(submitted);
        const bool authoritative_evidence_arrived =
            buffered_worker_evidence_.contains(
                dispatch->claimed.dispatch_attempt_id);
        std::optional<DeferredDispatchRelease> deferred_release;
        {
            std::lock_guard record_lock(dispatch->mutex);
            deferred_release = std::move(dispatch->deferred_release);
            dispatch->deferred_release.reset();
            if (!invariant_rejection) {
                dispatch->phase = DispatchPhase::Prepared;
                dispatch->target = {};
                dispatch->frozen_submission_sidecar.reset();
                dispatch->frozen_submission_cancellations.clear();
                dispatch->cancellation_sidecar_sha256.clear();
                dispatch->initially_suppressed_jobs.clear();
            }
        }
        {
            std::lock_guard mailbox_lock(mailbox->mutex);
            mailbox->submitting.reset();
        }
        std::string diagnostic = submitted.diagnostic.empty()
            ? "worker rejected reconstructed workset"
            : submitted.diagnostic;
        if (!submitted.error_code.empty()) {
            diagnostic += " [error_code=" + submitted.error_code + "]";
        }
        if (authoritative_evidence_arrived) {
            PauseForInvariant(
                dispatch,
                "Worker evidence preceded a no-write submission completion",
                diagnostic);
            FlushBufferedEvidence(
                dispatch->claimed.dispatch_attempt_id);
        } else if (invariant_rejection) {
            PauseForInvariant(
                dispatch,
                "Worker rejected a locally validated workset contract",
                diagnostic);
        } else if (deferred_release.has_value()) {
            (void)ReleaseDispatch(
                dispatch,
                std::move(deferred_release->reason_code),
                std::move(deferred_release->reason_text),
                deferred_release->keep_draining);
        } else {
            prepared_dispatches_.push_front(dispatch);
        }
        WakeScheduler("submission-not-written", false);
        return;
    }

    const bool sidecar_receipt_valid =
        submitted.submission_receipt.has_value()
        && submitted.submission_receipt->workset_id
            == command.definition.workset_id
        && submitted.submission_receipt->sidecar_version
            == savor::runtime::kInitialWorksetCancellationSidecarVersionV1
        && submitted.submission_receipt->applied_item_count
            == command.sidecar.item_ids.size()
        && submitted.submission_receipt->applied_sidecar_sha256
            == dispatch->cancellation_sidecar_sha256;
    if (!sidecar_receipt_valid) {
        ++sidecar_submit_receipts_mismatched_;
        PauseForInvariant(
            dispatch,
            "Worker accepted a workset without the frozen cancellation sidecar receipt",
            "dispatch_attempt_id="
                + std::to_string(dispatch->claimed.dispatch_attempt_id));
        return;
    }
    if (submitted.submission_receipt->disposition
        == savor::runtime::WorksetSubmissionDispositionV1::AlreadyAdmitted) {
        ++sidecar_submit_receipts_repeated_;
    } else {
        ++sidecar_submit_receipts_accepted_;
    }
    if (!dispatch->sidecar_receipt_accounted) {
        dispatch->sidecar_receipt_accounted = true;
        waiting_jobs_suppressed_by_sidecar_.fetch_add(
            command.sidecar.item_ids.size());
    }

    {
        std::lock_guard record_lock(dispatch->mutex);
        dispatch->submitted = true;
        dispatch->phase = DispatchPhase::Activating;
        dispatch->activation_retry_at = {};
    }
    ActivateSubmittedDispatch(dispatch, mailbox);
}

void JobExecutionCoordinator::Impl::ActivateSubmittedDispatch(
    const DispatchPtr& dispatch,
    const WorkerMailboxPtr& mailbox) {
    if (!dispatch || !mailbox) return;
    {
        std::lock_guard record_lock(dispatch->mutex);
        if (dispatch->phase != DispatchPhase::Activating
            || dispatch->activation_io_pending) {
            return;
        }
        dispatch->activation_io_pending = true;
    }
    EnqueueCoordinationIo(ActivateIoCommand{dispatch, mailbox});
}

void JobExecutionCoordinator::Impl::HandleActivationIoCompletion(
    const ActivateIoCommand& command,
    bool called,
    savor::db::WorksetDispatchMutationReceipt receipt,
    std::string error) {
    const auto& dispatch = command.dispatch;
    const auto& mailbox = command.mailbox;
    if (!dispatch || !mailbox) return;
    {
        std::lock_guard record_lock(dispatch->mutex);
        dispatch->activation_io_pending = false;
        if (dispatch->phase != DispatchPhase::Activating) return;
    }
    if (!called || !Applied(receipt.disposition)) {
        if (receipt.disposition
            == savor::db::ExecutionDbOperationDisposition::BackendError) {
            RecordError(error.empty()
                ? "failed marking submitted workset active"
                : std::move(error));
            {
                std::lock_guard record_lock(dispatch->mutex);
                dispatch->activation_retry_at =
                    Clock::now() + std::chrono::milliseconds(100);
            }
            WakeScheduler("activation-retry", false);
            return;
        }
        PauseForInvariant(
            dispatch,
            "Submitted workset lost durable authority",
            error.empty()
                ? "dispatch_attempt_id="
                    + std::to_string(dispatch->claimed.dispatch_attempt_id)
                : std::move(error));
        return;
    }

    std::optional<DeferredDispatchRelease> deferred_release;
    std::deque<PendingSubmissionCancellation> pending_cancellations;
    bool draining = false;
    {
        std::lock_guard record_lock(dispatch->mutex);
        dispatch->dispatch_marked = true;
        draining = dispatch->terminal_workset_state_observed;
        dispatch->phase = draining
            ? DispatchPhase::Draining
            : DispatchPhase::Active;
        if (draining) dispatch->draining_retry_at = Clock::now();
        deferred_release = std::move(dispatch->deferred_release);
        dispatch->deferred_release.reset();
        pending_cancellations.swap(
            dispatch->pending_submission_cancellations);
    }
    {
        std::lock_guard mailbox_lock(mailbox->mutex);
        mailbox->submitting.reset();
        if (!draining) {
            mailbox->active = dispatch->claimed.dispatch_attempt_id;
        }
    }
    FlushBufferedEvidence(dispatch->claimed.dispatch_attempt_id);
    if (draining) {
        (void)MarkDispatchDraining(dispatch);
    } else {
        const auto fallback_expiry =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count()
            + config_.workset_lease_duration.count();
        RegisterLeaseHeartbeat(
            dispatch,
            receipt.lease_expires_at_utc.value_or(fallback_expiry));
    }
    ++worksets_submitted_;

    for (const auto& cancellation :
         dispatch->frozen_submission_cancellations) {
        QueueCancellationMutation({
            .mutation = savor::db::JobCancellationOutcomeCommand{
                .kind = savor::db::JobCancellationOutcomeKind::
                    InitialSidecarApplied,
                .cancellation_request_id =
                    cancellation.cancellation_request_id,
                .job_id = cancellation.job_id,
                .dispatch_attempt_id =
                    dispatch->claimed.dispatch_attempt_id,
                .claim_token = dispatch->claimed.claim_token,
                .resolution_code = "INITIAL_SIDECAR_APPLIED",
                .requested_by = "job_execution_coordinator",
            },
            .requested_resolution_code = "INITIAL_SIDECAR_APPLIED",
            .dispatch = dispatch,
            .initial_suppression = true,
        });
    }
    for (auto& pending : pending_cancellations) {
        if (!pending.item_id.has_value()) {
            for (const auto& [runtime_item_id, item_index] :
                 dispatch->item_index_by_id) {
                if (item_index < dispatch->claimed.items.size()
                    && dispatch->claimed.items[item_index].job_id
                        == pending.cancellation.job_id) {
                    pending.item_id =
                        savor::runtime::WorkerWorksetItemId{runtime_item_id};
                    break;
                }
            }
        }
        if (!pending.item_id.has_value()) {
            PauseForInvariant(
                dispatch,
                "Cancellation route invariant failed after dispatch",
                "missing reconstructed workset item",
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
        (void)ReleaseDispatch(
            dispatch,
            std::move(deferred_release->reason_code),
            std::move(deferred_release->reason_text),
            deferred_release->keep_draining);
    }
    persistence_cv_.notify_all();
    WakeScheduler("dispatch-active", false);
}

void JobExecutionCoordinator::Impl::WorkerMailboxLoop(
    const WorkerMailboxPtr& mailbox) {
    for (;;) {
        WorkerControlCommand control;
        MailboxSubmissionCommand submission;
        bool have_control = false;
        bool have_submission = false;
        {
            std::unique_lock lock(mailbox->mutex);
            mailbox->cv.wait(lock, [&]() {
                return stop_.load() || mailbox->stop
                    || !mailbox->controls.empty()
                    || !mailbox->submissions.empty();
            });
            if (stop_.load() || mailbox->stop) return;
            const auto now = Clock::now();
            const bool control_first = !mailbox->controls.empty()
                && (mailbox->submissions.empty()
                    || mailbox->controls.front().enqueued_at
                        <= mailbox->submissions.front().enqueued_at);
            if (control_first) {
                if (mailbox->controls.front().retry_at > now) {
                    mailbox->cv.wait_until(
                        lock,
                        mailbox->controls.front().retry_at);
                    continue;
                }
                control = std::move(mailbox->controls.front());
                mailbox->controls.pop_front();
                have_control = true;
            } else if (!mailbox->submissions.empty()) {
                submission = std::move(mailbox->submissions.front());
                mailbox->submissions.pop_front();
                have_submission = true;
            }
        }
        if (have_control) {
            ExecuteWorkerControl(mailbox, std::move(control));
            continue;
        }
        if (have_submission) {
            const auto result = worker_coordinator_->SubmitWorksetToWorker(
                submission.target,
                submission.definition,
                submission.sidecar);
            EnqueueActor(
                CoordinatorEventKind::MailboxCompleted,
                [this, mailbox, submission = std::move(submission), result]() mutable {
                    HandleSubmissionCompletion(
                        mailbox,
                        std::move(submission),
                        result);
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
        FindWorkerMailbox(target.worker_id, target.process_generation);
    if (!lane) {
        ++worker_control_commands_abandoned_;
        if (command.kind == WorkerControlKind::Acknowledge) {
            ++worker_terminal_ack_abandoned_generation_loss_;
        }
        if (command.cancellation.has_value()) {
            EnqueueActor(
                CoordinatorEventKind::MailboxCompleted,
                [this,
                 cancellation = *command.cancellation,
                 enqueued_at = command.enqueued_at]() {
                    WorkerCommandResult stale{
                        .disposition =
                            WorkerCommandDisposition::StaleRoute,
                        .diagnostic =
                            "worker mailbox no longer exists",
                    };
                    CompleteCancellationDelivery(
                        cancellation,
                        stale,
                        false,
                        enqueued_at);
                });
        } else if (command.kind == WorkerControlKind::ConfirmResidence) {
            EnqueueActor(
                CoordinatorEventKind::LeaseCompleted,
                [this,
                 dispatch_attempt_id = command.dispatch_attempt_id,
                 claim_token = command.lease_claim_token]() mutable {
                    UnregisterLeaseHeartbeat(
                        dispatch_attempt_id,
                        claim_token);
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
    const WorkerMailboxPtr& mailbox,
    WorkerControlCommand command) {
    ++worker_control_commands_attempted_;
    if (command.kind == WorkerControlKind::ConfirmResidence) {
        savor::wrms::WorksetResidenceSnapshotV1 evidence{};
        std::string error;
        const bool called = worker_coordinator_->ConfirmWorksetResidence(
            {
                .worker_id = mailbox->worker_id,
                .process_generation = mailbox->generation,
            },
            savor::runtime::WorkerWorksetId{
                static_cast<std::uint64_t>(
                    command.dispatch_attempt_id)},
            &evidence,
            &error);
        EnqueueActor(
            CoordinatorEventKind::LeaseCompleted,
            [this, mailbox, command = std::move(command), called, evidence,
             error = std::move(error)]() mutable {
                HandleResidenceCompletion(
                    mailbox,
                    std::move(command),
                    called,
                    evidence,
                    std::move(error));
            });
        return;
    }
    WorkerCommandResult result{};
    if (command.kind == WorkerControlKind::Acknowledge) {
        result = worker_coordinator_->AcknowledgeTerminal(command.terminal);
    } else if (command.kind == WorkerControlKind::CancelItem
        && command.item_id.has_value()) {
        result = worker_coordinator_->CancelWorksetItem(
            savor::runtime::WorkerWorksetId{
                static_cast<std::uint64_t>(command.dispatch_attempt_id)},
            *command.item_id,
            command.reason);
    } else {
        result = worker_coordinator_->CancelWorkset(
            savor::runtime::WorkerWorksetId{
                static_cast<std::uint64_t>(command.dispatch_attempt_id)},
            command.reason);
    }
    EnqueueActor(
        CoordinatorEventKind::MailboxCompleted,
        [this, mailbox, command = std::move(command), result]() mutable {
            HandleWorkerControlCompletion(
                mailbox,
                std::move(command),
                result);
        });
}

void JobExecutionCoordinator::Impl::HandleResidenceCompletion(
    const WorkerMailboxPtr& mailbox,
    WorkerControlCommand command,
    bool called,
    savor::wrms::WorksetResidenceSnapshotV1 evidence,
    std::string error) {
    if (!called) {
        ++active_residence_failures_;
        UnregisterLeaseHeartbeat(
            command.dispatch_attempt_id,
            command.lease_claim_token);
        RecordWarning(
            "Active workset residence check failed",
            std::move(error));
        return;
    }
    if (evidence.cancellation_sidecar_sha256
        != command.expected_sidecar_sha256) {
        ++active_residence_failures_;
        UnregisterLeaseHeartbeat(
            command.dispatch_attempt_id,
            command.lease_claim_token);
        RecordWarning(
            "Active workset residence sidecar check failed",
            "dispatch_attempt_id="
                + std::to_string(command.dispatch_attempt_id));
        if (mailbox) {
            worker_coordinator_->QuarantineWorkerGeneration(
                {
                    .worker_id = mailbox->worker_id,
                    .process_generation = mailbox->generation,
                },
                "active workset residence sidecar mismatch");
        }
        return;
    }
    ++active_residence_matches_;
    EnqueueCoordinationIo(RenewLeaseIoCommand{
        .dispatch_attempt_id = command.dispatch_attempt_id,
        .claim_token = std::move(command.lease_claim_token),
    });
}

void JobExecutionCoordinator::Impl::HandleWorkerControlCompletion(
    const WorkerMailboxPtr& mailbox,
    WorkerControlCommand command,
    WorkerCommandResult result) {
    const auto dispatch = FindDispatch(command.dispatch_attempt_id);
    if (command.kind == WorkerControlKind::CancelWorkset
        && result.accepted() && dispatch) {
        std::lock_guard lock(dispatch->mutex);
        dispatch->workset_cancellation_applied = true;
    }

    if (command.cancellation.has_value()) {
        CompleteCancellationDelivery(
            *command.cancellation,
            result,
            command.kind == WorkerControlKind::CancelWorkset,
            command.enqueued_at);
        return;
    }

    if (result.accepted()) {
        ++worker_control_commands_applied_;
        if (command.kind == WorkerControlKind::Acknowledge && dispatch) {
            {
                std::lock_guard lock(dispatch->mutex);
                dispatch->acknowledged_jobs.emplace(command.job_id);
            }
            ++worker_terminal_acks_;
            MaybeRetire(dispatch);
        }
        return;
    }

    ++worker_control_commands_failed_;
    if (command.kind == WorkerControlKind::Acknowledge
        && !stop_.load() && mailbox) {
        command.retry_at = Clock::now() + config_.terminal_retry_interval;
        {
            std::lock_guard lock(mailbox->mutex);
            if (!mailbox->stop) {
                mailbox->controls.push_back(std::move(command));
            } else {
                ++worker_control_commands_abandoned_;
                ++worker_terminal_ack_abandoned_generation_loss_;
                return;
            }
        }
        mailbox->cv.notify_all();
    }
}
void JobExecutionCoordinator::Impl::RoutePersistenceEvent(
    PersistenceEvent event) {
    const auto mailbox = FindWorkerMailbox(
        event.worker_id,
        event.generation);
    std::optional<std::int64_t> submitting;
    if (mailbox) {
        std::lock_guard lock(mailbox->mutex);
        submitting = mailbox->submitting;
    }
    if (submitting.has_value()) {
        const auto dispatch = FindDispatch(*submitting);
        if (dispatch) {
            std::lock_guard lock(dispatch->mutex);
            if (dispatch->phase == DispatchPhase::Submitting
                || dispatch->phase == DispatchPhase::Activating) {
                buffered_worker_evidence_[*submitting].push_back(
                    std::move(event));
                return;
            }
        }
    }
    EnqueuePersistence(std::move(event));
}

void JobExecutionCoordinator::Impl::FlushBufferedEvidence(
    std::int64_t dispatch_attempt_id) {
    const auto found = buffered_worker_evidence_.find(
        dispatch_attempt_id);
    if (found == buffered_worker_evidence_.end()) return;
    auto events = std::move(found->second);
    buffered_worker_evidence_.erase(found);
    for (auto& event : events) {
        EnqueuePersistence(std::move(event));
    }
}

void JobExecutionCoordinator::Impl::EnqueuePersistence(
    PersistenceEvent event) {
    if (stop_.load()) return;
    event.dispatch_attempt_id = std::visit(
        [](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, TerminalEvent>) {
                return static_cast<std::int64_t>(
                    payload.envelope.terminal.workset_id);
            } else {
                return static_cast<std::int64_t>(
                    payload.payload.workset_id);
            }
        },
        event.payload);
    bool overflow = false;
    {
        std::lock_guard lock(persistence_mutex_);
        auto& stream = persistence_streams_[event.dispatch_attempt_id];
        overflow = stream.events.size()
            >= config_.max_pending_evidence_per_dispatch;
        if (!overflow) stream.events.push_back(std::move(event));
        std::size_t depth = 0;
        for (const auto& [key, candidate] : persistence_streams_) {
            (void)key;
            depth += candidate.events.size();
        }
        persistence_high_water_ =
            std::max(persistence_high_water_, depth);
    }
    if (overflow) {
        const auto dispatch = FindDispatch(event.dispatch_attempt_id);
        if (dispatch) {
            RecordError(
                "dispatch evidence queue exceeded its configured bound");
            (void)ReleaseDispatch(
                dispatch,
                "EVIDENCE_BACKPRESSURE_OVERFLOW",
                "worker evidence exceeded the per-dispatch persistence bound");
        }
        return;
    }
    persistence_cv_.notify_all();
}

bool JobExecutionCoordinator::Impl::PersistDispatchWorkerEvent(
    savor::db::WorkerExecutionEventMutation mutation,
    savor::db::WorkerExecutionEventMutationReceipt* receipt_out,
    std::string* error_out) {
    if (active_persistence_event_ == nullptr) {
        if (error_out) {
            *error_out = "worker evidence persistence was requested outside the actor";
        }
        return false;
    }
    auto& event = *active_persistence_event_;
    if (event.io_completion.has_value()) {
        auto completion = std::move(*event.io_completion);
        event.io_completion.reset();
        if (completion.kind != PersistenceEvent::IoKind::PersistMutation) {
            if (error_out) {
                *error_out = "dispatch persistence received a blob-stage completion";
            }
            return false;
        }
        if (receipt_out) *receipt_out = std::move(completion.receipt);
        if (error_out) *error_out = std::move(completion.error);
        return completion.succeeded;
    }
    event.io_request = PersistenceEvent::PersistMutationRequest{
        std::move(mutation)};
    throw PersistenceIoYield{};
}

bool JobExecutionCoordinator::Impl::StageDispatchTerminalBlob(
    std::int64_t workset_id,
    std::int64_t dispatch_attempt_id,
    std::int64_t job_id,
    std::uint64_t terminal_id,
    const std::vector<std::uint8_t>& envelope,
    savor::db::execution::WorkerResultBlobReference* blob_out,
    std::string* error_out) {
    if (active_persistence_event_ == nullptr) {
        if (error_out) {
            *error_out = "terminal blob staging was requested outside the actor";
        }
        return false;
    }
    auto& event = *active_persistence_event_;
    if (event.io_completion.has_value()) {
        auto completion = std::move(*event.io_completion);
        event.io_completion.reset();
        if (completion.kind != PersistenceEvent::IoKind::StageBlob) {
            if (error_out) {
                *error_out = "terminal blob staging received a DB completion";
            }
            return false;
        }
        if (blob_out) *blob_out = std::move(completion.blob);
        if (error_out) *error_out = std::move(completion.error);
        return completion.succeeded;
    }
    event.io_request = PersistenceEvent::StageBlobRequest{
        .workset_id = workset_id,
        .dispatch_attempt_id = dispatch_attempt_id,
        .job_id = job_id,
        .terminal_id = terminal_id,
        .envelope = envelope,
    };
    throw PersistenceIoYield{};
}

void JobExecutionCoordinator::Impl::PersistenceLoop() {
    while (!stop_.load()) {
        std::int64_t selected_key = 0;
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
                            return entry.second.retry_at <= now;
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
                    return entry.second.retry_at <= now;
                });
            if (found == persistence_streams_.end()) continue;
            selected_key = found->first;
            found->second.active = true;
            event = found->second.events.front();
        }

        bool consumed = false;
        for (;;) {
            struct ActorPersistenceResult {
                std::mutex mutex;
                std::condition_variable cv;
                PersistenceEvent event;
                bool consumed = false;
                bool completed = false;
            };
            auto actor_result = std::make_shared<ActorPersistenceResult>();
            actor_result->event = std::move(event);
            EnqueueActor(
                CoordinatorEventKind::PersistenceCompleted,
                [this, actor_result]() {
                    actor_result->consumed =
                        ProcessPersistenceEvent(actor_result->event);
                    {
                        std::lock_guard lock(actor_result->mutex);
                        actor_result->completed = true;
                    }
                    actor_result->cv.notify_one();
                });
            {
                std::unique_lock lock(actor_result->mutex);
                actor_result->cv.wait(lock, [this, &actor_result]() {
                    return actor_result->completed || stop_.load();
                });
            }
            if (!actor_result->completed) return;
            consumed = actor_result->consumed;
            event = std::move(actor_result->event);
            if (!event.io_request.has_value()) break;

            PersistenceEvent::IoCompletion completion{};
            ++dispatch_persistence_attempts_;
            std::visit(
                [this, &completion](auto& request) {
                    using T = std::decay_t<decltype(request)>;
                    if constexpr (std::is_same_v<
                                      T,
                                      PersistenceEvent::
                                          PersistMutationRequest>) {
                        completion.kind =
                            PersistenceEvent::IoKind::PersistMutation;
                        savor::db::PersistWorkerExecutionEventsBatchCommand
                            command;
                        command.events.push_back(std::move(request.mutation));
                        savor::db::PersistWorkerExecutionEventsBatchReceipt
                            receipt;
                        completion.succeeded =
                            execution_db_->PersistWorkerExecutionEventsBatch(
                                command,
                                &receipt,
                                &completion.error)
                            && receipt.events.size() == 1;
                        if (completion.succeeded) {
                            completion.receipt = std::move(receipt.events[0]);
                            ++dispatch_persistence_events_;
                        }
                    } else {
                        completion.kind = PersistenceEvent::IoKind::StageBlob;
                        completion.succeeded = blob_store_->Stage(
                            {
                                .workset_id = request.workset_id,
                                .dispatch_attempt_id =
                                    request.dispatch_attempt_id,
                                .job_id = request.job_id,
                                .terminal_id = request.terminal_id,
                                .envelope = request.envelope,
                            },
                            &completion.blob,
                            &completion.error);
                    }
                },
                *event.io_request);
            if (!completion.succeeded) {
                ++dispatch_persistence_failures_;
            }
            event.io_request.reset();
            event.io_completion = std::move(completion);
        }
        {
            std::lock_guard lock(persistence_mutex_);
            const auto found = persistence_streams_.find(selected_key);
            if (found != persistence_streams_.end()) {
                if (consumed && !found->second.events.empty()) {
                    found->second.events.pop_front();
                    found->second.retry_attempt = 0;
                    found->second.retry_at = {};
                    found->second.last_error.clear();
                } else if (!consumed && !found->second.events.empty()) {
                    found->second.events.front() = event;
                    ++found->second.retry_attempt;
                    ++dispatch_persistence_retries_;
                    const auto* terminal = std::get_if<TerminalEvent>(
                        &found->second.events.front().payload);
                    found->second.retry_at = terminal != nullptr
                        && terminal->retry_at > Clock::now()
                        ? terminal->retry_at
                        : Clock::now() + CappedExponentialDelay(
                            config_.terminal_retry_interval,
                            config_.terminal_retry_max_interval,
                            found->second.retry_attempt);
                    found->second.last_error =
                        "dispatch persistence retry pending";
                }
                found->second.active = false;
                if (found->second.events.empty()) {
                    persistence_streams_.erase(found);
                }
            }
        }
        if (!consumed) {
            persistence_cv_.notify_all();
        } else {
            persistence_cv_.notify_all();
        }
    }
}

bool JobExecutionCoordinator::Impl::ProcessPersistenceEvent(
    PersistenceEvent& event) {
    active_persistence_event_ = &event;
    try {
        const bool consumed = std::visit(
            [this](auto& payload) -> bool {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, ItemStartedEvent>) {
                    return HandleItemStarted(payload);
                } else if constexpr (std::is_same_v<T, ProgressEvent>) {
                    return HandleProgress(payload);
                } else if constexpr (std::is_same_v<T, TerminalEvent>) {
                    return HandleTerminal(payload);
                } else {
                    return HandleWorksetSummary(payload);
                }
            },
            event.payload);
        active_persistence_event_ = nullptr;
        return consumed;
    } catch (const PersistenceIoYield&) {
        active_persistence_event_ = nullptr;
        return false;
    }
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
        PersistDispatchWorkerEvent(
            savor::db::MarkWorksetJobStartedCommand{
                .dispatch_attempt_id = dispatch_id,
                .claim_token = claimed.claim_token,
                .job_id = durable_item.job_id,
                .workset_item_ordinal =
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

bool JobExecutionCoordinator::Impl::HandleProgress(
    const ProgressEvent& event) {
    const auto dispatch_id =
        static_cast<std::int64_t>(event.payload.workset_id);
    const auto dispatch = FindDispatch(dispatch_id);
    if (!dispatch) {
        RecordWarning(
            "Ignored progress for unknown workset",
            "dispatch_attempt_id=" + std::to_string(dispatch_id),
            static_cast<std::int64_t>(event.source.worker_id));
        return true;
    }

    savor::db::ClaimedPublishedWorkset claimed;
    savor::db::ClaimedPublishedWorksetItem durable_item;
    savor::runtime::WorksetItemTemplate runtime_item;
    bool authority_released = false;
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
            if (dispatch->phase == DispatchPhase::ReleasedDraining
                || dispatch->phase == DispatchPhase::Released
                || dispatch->phase == DispatchPhase::Retired) {
                return true;
            }
            PauseForInvariant(
                dispatch,
                "Worker progress correlation mismatch",
                "dispatch_attempt_id=" + std::to_string(dispatch_id));
            return true;
        }
        const auto index = item_found->second;
        runtime_item = dispatch->definition.items[index];
        durable_item = dispatch->claimed.items[index];
        claimed = dispatch->claimed;
        authority_released =
            dispatch->phase == DispatchPhase::ReleasedDraining
            || dispatch->phase == DispatchPhase::Released
            || dispatch->phase == DispatchPhase::Retired;
        if (authority_released) return true;
        if (event.payload.item_ordinal != runtime_item.ordinal
            || event.payload.invocation_id
                != runtime_item.execution.execution_id.value()
            || event.payload.attempt_id
                != runtime_item.execution.attempt_id.value()
            || event.payload.durable_job_id
                != std::to_string(durable_item.job_id)
            || event.payload.ordinal == 0) {
            PauseForInvariant(
                dispatch,
                "Worker progress identity mismatch",
                "job_id=" + std::to_string(durable_item.job_id));
            return true;
        }
    }

    savor::db::WorkerExecutionEventMutationReceipt batch_receipt{};
    std::string error;
    const bool persisted = PersistDispatchWorkerEvent(
        savor::db::RecordCanonicalJobProgressCommand{
            .dispatch_attempt_id = dispatch_id,
            .claim_token = claimed.claim_token,
            .job_id = durable_item.job_id,
            .workset_item_ordinal = event.payload.item_ordinal,
            .reserved_attempt_id = durable_item.reserved_attempt_id,
            .workset_id = event.payload.workset_id,
            .item_id = event.payload.item_id,
            .invocation_id = event.payload.invocation_id,
            .ordinal = event.payload.ordinal,
            .library_id = event.payload.library_id,
            .library_revision = event.payload.library_revision,
            .progress_point_id = event.payload.progress_point_id,
            .has_routed_provenance =
                event.payload.has_routed_provenance,
            .routed_sequence = event.payload.routed_sequence,
            .sample_snapshot_id = event.payload.sample_snapshot_id,
            .trigger_epoch = event.payload.trigger_epoch,
            .schema_id = event.payload.schema_id,
            .schema_revision = event.payload.schema_revision,
            .schema_sha256 = event.payload.schema_sha256,
            .typed_payload = event.payload.typed_payload,
            .display_text = event.payload.display_text,
            .requested_by = "job_execution_coordinator",
        },
        &batch_receipt,
        &error);
    savor::db::CanonicalJobProgressReceipt receipt{};
    if (persisted) {
        if (const auto* typed =
                std::get_if<savor::db::CanonicalJobProgressReceipt>(
                    &batch_receipt)) {
            receipt = *typed;
        } else {
            error = "worker-event batch returned the wrong progress receipt";
        }
    }
    if (!persisted
        || (receipt.disposition
                != savor::db::ExecutionDbOperationDisposition::Applied
            && receipt.disposition
                != savor::db::ExecutionDbOperationDisposition::AlreadyApplied)) {
        if (receipt.disposition
            == savor::db::ExecutionDbOperationDisposition::BackendError) {
            RecordError(
                error.empty()
                    ? "failed persisting canonical job progress"
                    : std::move(error));
            return false;
        }
        {
            std::lock_guard lock(dispatch->mutex);
            authority_released =
                dispatch->phase == DispatchPhase::ReleasedDraining
                || dispatch->phase == DispatchPhase::Released
                || dispatch->phase == DispatchPhase::Retired;
        }
        if (!authority_released) {
            PauseForInvariant(
                dispatch,
                "Canonical job progress durable authority rejected",
                error.empty()
                    ? "job_id=" + std::to_string(durable_item.job_id)
                    : std::move(error),
                durable_item.job_id);
        }
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
        if (!StageDispatchTerminalBlob(
                claimed.workset_id,
                dispatch_id,
                durable_item.job_id,
                payload.terminal_id,
                envelope_bytes,
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
            WakeScheduler("dispatch-persistence-retry", false);
            return false;
        }
        prepared = std::make_shared<PreparedTerminalPersistence>();
        prepared->blob = std::move(blob);
        prepared->command =
            savor::db::StageWorkerTerminalCommand{
                .dispatch_attempt_id = dispatch_id,
                .claim_token = claimed.claim_token,
                .job_id = durable_item.job_id,
                .workset_item_ordinal = payload.item_ordinal,
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
        event.prepared = prepared;
    }
    const auto& blob = prepared->blob;

    savor::db::StageWorkerTerminalReceipt receipt{};
    savor::db::WorkerExecutionEventMutationReceipt batch_receipt{};
    const bool persisted = PersistDispatchWorkerEvent(
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
            WakeScheduler("dispatch-persistence-retry", false);
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
    WakeScheduler("workset-summary", false);
    MaybeRetire(dispatch);
    return true;
}

void JobExecutionCoordinator::Impl::EnqueueCoordinationIo(
    CoordinationIoCommand command) {
    {
        std::lock_guard lock(coordination_io_mutex_);
        if (coordination_io_stop_) return;
        coordination_io_commands_.push_back(std::move(command));
    }
    coordination_io_cv_.notify_one();
}

void JobExecutionCoordinator::Impl::CoordinationIoLoop() {
    for (;;) {
        CoordinationIoCommand command;
        {
            std::unique_lock lock(coordination_io_mutex_);
            coordination_io_cv_.wait(lock, [this]() {
                return coordination_io_stop_
                    || !coordination_io_commands_.empty();
            });
            if (coordination_io_stop_
                && coordination_io_commands_.empty()) {
                return;
            }
            command = std::move(coordination_io_commands_.front());
            coordination_io_commands_.pop_front();
        }

        std::visit(
            [this](auto io_command) mutable {
                using T = std::decay_t<decltype(io_command)>;
                if constexpr (std::is_same_v<T, ClaimIoCommand>) {
                    std::string error;
                    auto claimed = execution_db_->ClaimPublishedWorksetBatch(
                        {
                            .batch_nonce = io_command.batch_nonce,
                            .requested_workset_count =
                                io_command.requested_workset_count,
                        },
                        &error);
                    EnqueueActor(
                        CoordinatorEventKind::CoordinationIoCompleted,
                        [this, io_command = std::move(io_command),
                         claimed = std::move(claimed),
                         error = std::move(error)]() mutable {
                            HandleClaimIoCompletion(
                                std::move(io_command),
                                std::move(claimed),
                                std::move(error));
                        });
                } else if constexpr (
                    std::is_same_v<T, ActivateIoCommand>) {
                    savor::db::WorksetDispatchMutationReceipt receipt{};
                    std::string error;
                    const bool called = execution_db_->MarkWorksetActive(
                        {
                            .dispatch_attempt_id = io_command.dispatch
                                ->claimed.dispatch_attempt_id,
                            .claim_token =
                                io_command.dispatch->claimed.claim_token,
                            .lease_duration_ms =
                                config_.workset_lease_duration.count(),
                            .requested_by =
                                "job_execution_coordinator",
                        },
                        &receipt,
                        &error);
                    EnqueueActor(
                        CoordinatorEventKind::CoordinationIoCompleted,
                        [this, io_command = std::move(io_command), called,
                         receipt, error = std::move(error)]() mutable {
                            HandleActivationIoCompletion(
                                io_command,
                                called,
                                receipt,
                                std::move(error));
                        });
                } else if constexpr (
                    std::is_same_v<T, DrainingIoCommand>) {
                    savor::db::WorksetDispatchMutationReceipt receipt{};
                    std::string error;
                    const bool called = execution_db_->MarkWorksetDraining(
                        {
                            .dispatch_attempt_id =
                                io_command.claimed.dispatch_attempt_id,
                            .claim_token = io_command.claimed.claim_token,
                            .requested_by =
                                "job_execution_coordinator",
                        },
                        &receipt,
                        &error);
                    EnqueueActor(
                        CoordinatorEventKind::CoordinationIoCompleted,
                        [this, io_command = std::move(io_command), called,
                         receipt, error = std::move(error)]() mutable {
                            HandleDrainingIoCompletion(
                                io_command,
                                called,
                                receipt,
                                std::move(error));
                        });
                } else if constexpr (
                    std::is_same_v<T, ReleaseIoCommand>) {
                    savor::db::WorksetDispatchMutationReceipt receipt{};
                    std::string error;
                    const bool called = execution_db_->ReleaseWorksetDispatch(
                        {
                            .dispatch_attempt_id =
                                io_command.claimed.dispatch_attempt_id,
                            .claim_token = io_command.claimed.claim_token,
                            .reason_code = io_command.reason_code,
                            .reason_text = io_command.reason_text.empty()
                                ? std::nullopt
                                : std::optional<std::string>(
                                    io_command.reason_text),
                            .requested_by =
                                "job_execution_coordinator",
                        },
                        &receipt,
                        &error);
                    EnqueueActor(
                        CoordinatorEventKind::CoordinationIoCompleted,
                        [this, io_command = std::move(io_command), called,
                         receipt, error = std::move(error)]() mutable {
                            HandleReleaseIoCompletion(
                                std::move(io_command),
                                called,
                                receipt,
                                std::move(error));
                        });
                } else {
                    std::string error;
                    ++active_lease_renewal_batches_;
                    auto receipts = execution_db_->RenewActiveWorksetLeases(
                        {
                            .requests = {{
                                .dispatch_attempt_id =
                                    io_command.dispatch_attempt_id,
                                .claim_token = io_command.claim_token,
                            }},
                            .lease_duration_ms =
                                config_.workset_lease_duration.count(),
                        },
                        &error);
                    savor::db::WorksetDispatchLeaseReceipt receipt{};
                    if (!receipts.empty()) receipt = receipts.front();
                    receipt.dispatch_attempt_id =
                        io_command.dispatch_attempt_id;
                    EnqueueActor(
                        CoordinatorEventKind::LeaseCompleted,
                        [this, io_command = std::move(io_command), receipt,
                         error = std::move(error)]() mutable {
                            HandleLeaseHeartbeatResult(
                                io_command.dispatch_attempt_id,
                                std::move(io_command.claim_token),
                                receipt.disposition
                                    != savor::db::
                                        ExecutionDbOperationDisposition::
                                            BackendError,
                                receipt,
                                std::move(error));
                        });
                }
            },
            std::move(command));
    }
}

void JobExecutionCoordinator::Impl::EnqueueActor(
    CoordinatorEventKind kind,
    std::function<void()> action) {
    if (stop_.load()) return;
    {
        std::lock_guard lock(actor_mutex_);
        actor_events_.push_back({kind, std::move(action)});
    }
    actor_cv_.notify_all();
}

void JobExecutionCoordinator::Impl::RunOnActorAndWait(
    std::function<void()> action) {
    if (IsActorThread()) {
        action();
        return;
    }
    struct Rendezvous {
        std::mutex mutex;
        std::condition_variable cv;
        bool completed = false;
    };
    auto rendezvous = std::make_shared<Rendezvous>();
    EnqueueActor(
        CoordinatorEventKind::SynchronousRequest,
        [action = std::move(action), rendezvous]() mutable {
        action();
        {
            std::lock_guard lock(rendezvous->mutex);
            rendezvous->completed = true;
        }
        rendezvous->cv.notify_one();
        });
    std::unique_lock lock(rendezvous->mutex);
    rendezvous->cv.wait(lock, [this, &rendezvous]() {
        return rendezvous->completed || stop_.load();
    });
}

bool JobExecutionCoordinator::Impl::IsActorThread() const {
    std::lock_guard lock(actor_mutex_);
    return actor_thread_id_ == std::this_thread::get_id();
}

void JobExecutionCoordinator::Impl::ActorLoop() {
    {
        std::lock_guard lock(actor_mutex_);
        actor_thread_id_ = std::this_thread::get_id();
    }
    while (!stop_.load()) {
        bool advanced = false;
        for (;;) {
            CoordinatorEvent event;
            {
                std::lock_guard lock(actor_mutex_);
                if (actor_events_.empty()) break;
                event = std::move(actor_events_.front());
                actor_events_.pop_front();
            }
            if (event.apply) event.apply();
            advanced = true;
        }

        ProcessLifecycleRequest();
        RefreshStorageReadiness();
        advanced = ProcessPendingDrainingTransitions() || advanced;
        advanced = ProcessPendingDispatchReleases() || advanced;
        ScheduleActor();
        const auto snapshot_now = Clock::now();
        if (advanced || snapshot_now >= next_snapshot_publish_at_) {
            PublishSnapshots();
            next_snapshot_publish_at_ =
                snapshot_now + std::chrono::milliseconds(250);
        }

        if (!advanced) {
            auto deadline = Clock::now() + std::chrono::seconds(30);
            {
                std::lock_guard lock(registry_mutex_);
                for (const auto& [_, dispatch] : dispatches_) {
                    std::lock_guard record_lock(dispatch->mutex);
                    if (dispatch->phase == DispatchPhase::Activating
                        && dispatch->activation_retry_at
                            != Clock::time_point{}) {
                        deadline = std::min(
                            deadline, dispatch->activation_retry_at);
                    }
                    if (dispatch->phase == DispatchPhase::Draining
                        && !dispatch->draining_transition_persisted
                        && dispatch->draining_retry_at
                            != Clock::time_point{}) {
                        deadline = std::min(
                            deadline, dispatch->draining_retry_at);
                    }
                    if (dispatch->release_retry_at
                        != Clock::time_point{}) {
                        deadline = std::min(
                            deadline, dispatch->release_retry_at);
                    }
                }
            }
            std::unique_lock lock(actor_mutex_);
            if (claim_backoff_.next_attempt != Clock::time_point{}) {
                deadline = std::min(deadline, claim_backoff_.next_attempt);
            }
            if (next_full_claim_reconciliation_ != Clock::time_point{}) {
                deadline = std::min(
                    deadline, next_full_claim_reconciliation_);
            }
            if (next_blob_readiness_retry_at_ != Clock::time_point{}) {
                deadline = std::min(
                    deadline, next_blob_readiness_retry_at_);
            }
            if (next_snapshot_publish_at_ != Clock::time_point{}) {
                deadline = std::min(deadline, next_snapshot_publish_at_);
            }
            actor_cv_.wait_until(lock, deadline);
        }
    }
    {
        std::lock_guard lock(actor_mutex_);
        actor_thread_id_ = {};
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
                            ResolveWithoutWorker
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
            WakeScheduler("cancellation-persistence-retry", false);
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
        WakeScheduler("cancellation-persistence-complete", false);
        EnqueueActor(
            CoordinatorEventKind::CancellationCompleted,
            [this,
             pending = std::move(pending),
             receipts = std::move(receipts)]() mutable {
                ApplyCancellationMutationReceipts(
                    std::move(pending), std::move(receipts));
            });
    }
}

void JobExecutionCoordinator::Impl::ApplyCancellationMutationReceipts(
    std::vector<PendingCancellationMutation> pending,
    std::vector<savor::db::JobCancellationReceipt> receipts) {
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
                    ClearWorkerIdentity(
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
    if (result.terminal_item_cancellation_race()) {
        return;
    }
    if (!result.accepted()) {
        const auto failure =
            detail::DescribeCancellationDeliveryFailure(result);
        ++worker_control_commands_failed_;
        RecordWarning(
            failure.warning_message,
            result.diagnostic,
            result.worker_id.has_value()
                ? static_cast<std::int64_t>(*result.worker_id)
                : 0,
            cancellation.job_id);
        EnqueueActor(
            CoordinatorEventKind::CancellationCompleted,
            [this, cancellation = std::move(cancellation),
             diagnostic = result.diagnostic,
             failure]() mutable {
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
                                .error_code = failure.error_code,
                                .error_text = diagnostic.empty()
                                    ? failure.warning_message
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
    EnqueueActor(
        CoordinatorEventKind::CancellationCompleted,
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
    UnregisterLeaseHeartbeat(
        dispatch_id,
        dispatch->claimed.claim_token);
    if (persist_draining) {
        EnqueueActor(
            CoordinatorEventKind::WorkerEvidence,
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
        if (dispatch->draining_io_pending) return false;
        if (phase == DispatchPhase::Draining
            && dispatch->draining_retry_at > Clock::now()) {
            return false;
        }
        if (phase == DispatchPhase::Draining) {
            dispatch->draining_io_pending = true;
        }
    }
    if (phase == DispatchPhase::Released
        || phase == DispatchPhase::ReleasedDraining
        || phase == DispatchPhase::Retired) {
        return true;
    }
    EnqueueCoordinationIo(DrainingIoCommand{dispatch, std::move(claimed)});
    return true;
}

void JobExecutionCoordinator::Impl::HandleDrainingIoCompletion(
    const DrainingIoCommand& command,
    bool called,
    savor::db::WorksetDispatchMutationReceipt receipt,
    std::string error) {
    const auto& dispatch = command.dispatch;
    if (!dispatch) return;
    {
        std::lock_guard lock(dispatch->mutex);
        dispatch->draining_io_pending = false;
    }
    if (!called) {
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
        return;
    }
    if (Applied(receipt.disposition)) {
        bool first_persisted_transition = false;
        {
            std::lock_guard lock(dispatch->mutex);
            if (dispatch->phase == DispatchPhase::Draining
                && !dispatch->draining_transition_persisted) {
                dispatch->draining_transition_persisted = true;
                dispatch->draining_retry_at = {};
                first_persisted_transition = true;
            }
        }
        if (first_persisted_transition) {
            ++draining_transitions_;
        }
        MaybeRetire(dispatch);
        return;
    }
    {
        std::lock_guard lock(dispatch->mutex);
        if (dispatch->phase == DispatchPhase::Released
            || dispatch->phase == DispatchPhase::ReleasedDraining
            || dispatch->phase == DispatchPhase::Retired) {
            return;
        }
    }
    PauseForInvariant(
        dispatch,
        "Durable draining transition rejected",
        "dispatch_attempt_id="
            + std::to_string(command.claimed.dispatch_attempt_id)
            + " disposition="
            + ExecutionDbDispositionName(receipt.disposition));
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
    WorkerMailboxPtr retired_lane;
    {
        std::lock_guard lock(mailboxes_mutex_);
        const auto found = mailboxes_.find(event.source.worker_id);
        if (found != mailboxes_.end()) {
            std::lock_guard lane_lock(found->second->mutex);
            if (found->second->generation
                == event.source.process_generation) {
                retired_lane = found->second;
                retired_lane->stop = true;
                mailboxes_.erase(found);
            }
        }
    }
    if (retired_lane) {
        retired_lane->cv.notify_all();
        if (retired_lane->thread.joinable()
            && retired_lane->thread.get_id()
                != std::this_thread::get_id()) {
            retired_lane->thread.join();
        }
        std::lock_guard lane_lock(retired_lane->mutex);
        for (const auto& command : retired_lane->controls) {
            ++worker_control_commands_abandoned_;
            if (command.kind == WorkerControlKind::Acknowledge) {
                ++worker_terminal_ack_abandoned_generation_loss_;
            }
        }
        retired_lane->controls.clear();
    }
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
        global_storage_unavailable_.store(false);
        WakeScheduler("storage-ready", false);
        return;
    }
    ++blob_readiness_failures_;
    ++blob_readiness_retry_attempt_;
    global_storage_unavailable_.store(true);
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
    if (claim_in_flight_) return;

    std::vector<DispatchPtr> release;
    bool submission_in_progress = false;
    bool release_in_progress = false;
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
            if (dispatch->phase == DispatchPhase::ReleaseRequested) {
                release_in_progress = true;
                continue;
            }
            if (dispatch->phase != DispatchPhase::Released
                && dispatch->phase != DispatchPhase::ReleasedDraining
                && dispatch->phase != DispatchPhase::Retired) {
                release.push_back(dispatch);
            }
        }
    }
    if (submission_in_progress || release_in_progress) return;

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
    if (!release.empty()) return;
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
        if (dispatch->release_retry_at > Clock::now()) {
            return false;
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

    EnqueueCoordinationIo(ReleaseIoCommand{
        .dispatch = dispatch,
        .claimed = std::move(claimed),
        .target = target,
        .previous = previous,
        .submitted = submitted,
        .keep_draining = keep_draining,
        .reason_code = std::move(reason_code),
        .reason_text = std::move(reason_text),
    });
    return true;
}

void JobExecutionCoordinator::Impl::HandleReleaseIoCompletion(
    ReleaseIoCommand command,
    bool called,
    savor::db::WorksetDispatchMutationReceipt receipt,
    std::string error) {
    const auto& dispatch = command.dispatch;
    if (!dispatch) return;
    if (!called || !Applied(receipt.disposition)) {
        {
            std::lock_guard lock(dispatch->mutex);
            if (dispatch->phase
                == DispatchPhase::ReleaseRequested) {
                dispatch->phase = command.previous;
                dispatch->release_retry_at = Clock::now()
                    + config_.workset_lease_retry_interval;
                dispatch->deferred_release = DeferredDispatchRelease{
                    .reason_code = command.reason_code,
                    .reason_text = command.reason_text,
                    .keep_draining = command.keep_draining,
                };
            }
        }
        RecordError(
            error.empty()
                ? "failed releasing workset dispatch"
                : std::move(error));
        WakeScheduler("dispatch-release-retry", false);
        return;
    }
    UnregisterLeaseHeartbeat(
        command.claimed.dispatch_attempt_id,
        command.claimed.claim_token);
    {
        std::lock_guard lock(dispatch_release_mutex_);
        ++dispatch_release_reason_counts_[command.reason_code];
        const char* phase = "UNKNOWN";
        switch (command.previous) {
        case DispatchPhase::Claimed: phase = "CLAIMED"; break;
        case DispatchPhase::Reconstructing: phase = "RECONSTRUCTING"; break;
        case DispatchPhase::Prepared: phase = "PREPARED"; break;
        case DispatchPhase::Submitting: phase = "SUBMITTING"; break;
        case DispatchPhase::Activating: phase = "ACTIVATING"; break;
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
        FindWorkerMailbox(
            command.target.worker_id,
            command.target.process_generation);
    if (lane) {
        std::lock_guard lock(lane->mutex);
        if (lane->submitting
            == std::optional<std::int64_t>(
                command.claimed.dispatch_attempt_id)) {
            lane->submitting.reset();
        }
        if (lane->active
            == std::optional<std::int64_t>(
                command.claimed.dispatch_attempt_id)) {
            lane->active.reset();
        }
        lane->cv.notify_all();
    }
    prepared_dispatches_.erase(
        std::remove(
            prepared_dispatches_.begin(),
            prepared_dispatches_.end(),
            dispatch),
        prepared_dispatches_.end());
    {
        std::lock_guard lock(dispatch->mutex);
        dispatch->phase =
            command.keep_draining && command.submitted
            ? DispatchPhase::ReleasedDraining
            : DispatchPhase::Released;
        dispatch->release_retry_at = {};
        dispatch->deferred_release.reset();
    }
    if (!(command.keep_draining && command.submitted)) {
        RemoveDispatch(command.claimed.dispatch_attempt_id, dispatch);
    }
    WakeScheduler("dispatch-released", false);
}

bool JobExecutionCoordinator::Impl::ProcessPendingDispatchReleases() {
    std::vector<std::pair<DispatchPtr, DeferredDispatchRelease>> pending;
    const auto now = Clock::now();
    {
        std::lock_guard lock(registry_mutex_);
        for (const auto& [_, dispatch] : dispatches_) {
            std::lock_guard dispatch_lock(dispatch->mutex);
            if (dispatch->deferred_release.has_value()
                && dispatch->phase != DispatchPhase::Submitting
                && dispatch->phase != DispatchPhase::Activating
                && dispatch->phase != DispatchPhase::ReleaseRequested
                && dispatch->release_retry_at <= now) {
                pending.emplace_back(
                    dispatch,
                    std::move(*dispatch->deferred_release));
                dispatch->deferred_release.reset();
            }
        }
    }
    for (auto& [dispatch, release] : pending) {
        (void)ReleaseDispatch(
            dispatch,
            std::move(release.reason_code),
            std::move(release.reason_text),
            release.keep_draining);
    }
    return !pending.empty();
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
        EnqueueActor(
            CoordinatorEventKind::WorkerEvidence,
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

void JobExecutionCoordinator::Impl::ClearWorkerIdentity(
    const WorkerExecutionTarget& target,
    std::int64_t dispatch_attempt_id) {
    const auto lane =
        FindWorkerMailbox(target.worker_id, target.process_generation);
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
        detail::DispatchRetirementAuthority authority =
            detail::DispatchRetirementAuthority::None;
        if (dispatch->phase == DispatchPhase::ReleasedDraining) {
            authority = detail::DispatchRetirementAuthority::
                ReleasedDraining;
        } else if (dispatch->phase == DispatchPhase::Draining) {
            authority = dispatch->draining_transition_persisted
                ? detail::DispatchRetirementAuthority::
                    DrainingPersisted
                : detail::DispatchRetirementAuthority::
                    DrainingPending;
        }
        complete = detail::DispatchReadyToRetire(
            {
                .authority = authority,
                .summary_observed = dispatch->summary_observed,
                .executable_items = executable,
                .staged_items = dispatch->staged_jobs.size(),
                .acknowledged_items =
                    dispatch->acknowledged_jobs.size(),
                .sidecar_persisted = sidecar_persisted,
            });
        if (complete) dispatch->phase = DispatchPhase::Retired;
    }
    if (!complete) return;
    UnregisterLeaseHeartbeat(dispatch_id, claim_token);
    ClearWorkerIdentity(target, dispatch_id);
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
            const auto lane = FindWorkerMailbox(
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
            EnqueueWorkerControl(
                target,
                {
                    .kind = WorkerControlKind::ConfirmResidence,
                    .dispatch_attempt_id = entry.dispatch_attempt_id,
                    .lease_claim_token = entry.claim_token,
                    .expected_sidecar_sha256 =
                        std::move(expected_sidecar_sha256),
                    .enqueued_at = Clock::now(),
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
            const auto fallback_expiry =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()
                + config_.workset_lease_duration.count();
            const auto durable_expiry =
                receipt.lease_expires_at_utc.value_or(fallback_expiry);
            std::lock_guard lease_lock(lease_heartbeat_mutex_);
            const auto found = lease_heartbeat_entries_.find(
                dispatch_attempt_id);
            if (found != lease_heartbeat_entries_.end()
                && found->second.claim_token == claim_token) {
                found->second.lease_expires_at_utc = durable_expiry;
                found->second.renew_at = LeaseRenewalDeadline(
                    durable_expiry,
                    config_.workset_lease_renewal_point);
                lease_heartbeat_cv_.notify_all();
            }
            return;
        }
    }
    if (receipt.disposition
        == savor::db::ExecutionDbOperationDisposition::BackendError) {
        ++active_lease_renewal_retries_;
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
        "dispatch_attempt_id=" + std::to_string(dispatch_attempt_id));
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
std::vector<JobExecutionWorkerDispatchSnapshot>
JobExecutionCoordinator::SnapshotWorkerDispatches() const {
    return impl_->SnapshotWorkerDispatches();
}
std::vector<JobExecutionCoordinatorWarning>
JobExecutionCoordinator::SnapshotWarnings() const {
    return impl_->SnapshotWarnings();
}

} // namespace savor::runner::parallel::savordb
