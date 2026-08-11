#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Runner/IPC/WrmsProtocol.h"
#include "Runner/IPC/DurableWorkerTerminalEnvelope.h"
#include "Runner/Runtime/RuntimeTypes.h"
#include "Runner/Runtime/Worksets/WorksetTypes.h"
#include "../Worker/ProcessWorker.h"
#include "../Worker/WorkerStatusRegistry.h"
#include "FleetStartupSnapshot.h"

namespace savor::runner::parallel::savordb {

// This component deliberately has no execution-database, workflow, descriptor,
// or result-projection dependency. It owns only the physical worker fleet and
// the WRMS protocol sessions hosted by that fleet.

struct WorkerCoordinatorRuntimePreflightResult {
    // A custom preflight owns launch, WRMS negotiation, and OpenSession. A
    // true result therefore means the supplied ProcessWorker is ready for
    // workset commands, not merely that its executable was inspected.
    bool process_ready = false;
    std::optional<savor::runtime::WorkerRuntimeContractV1> runtime_contract;
    bool retryable = true;
    std::string error;
};

struct WorkerCoordinatorConfig {
    using RuntimeSlotPreparer = std::function<bool(
        std::size_t,
        const WorkerCoordinatorConfig&,
        std::filesystem::path*,
        std::string*)>;
    using WorkerRuntimePreflight =
        std::function<WorkerCoordinatorRuntimePreflightResult(
            std::size_t,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&)>;

    std::size_t desired_workers = 0;
    std::uint32_t controller_sleep_ms = 5;
    std::uint32_t worker_start_timeout_ms = 20000;
    std::uint32_t worker_start_retry_backoff_ms = 5000;
    std::uint32_t max_worker_start_attempts = 3;
    std::uint32_t max_concurrent_worker_starts = 2;
    std::uint32_t liveness_probe_interval_ms = 5000;
    std::uint32_t liveness_probe_timeout_ms = 2000;
    std::uint32_t liveness_probe_failure_threshold = 2;

    std::string worker_exe_path;
    std::string iso_path;
    std::string dolphin_base_dir;
    std::string worker_dir_root;
    std::string worker_binary_runtime_root;
    savor::runtime::WorkerMode worker_mode =
        savor::runtime::WorkerMode::Headless;
    std::string runtime_artifact_root;

    savor::runtime::WorkerRuntimeContractV1 expected_runtime_contract =
        savor::runtime::BuildProductionWorkerRuntimeContractV1();

    RuntimeSlotPreparer runtime_slot_preparer;
    WorkerRuntimePreflight worker_runtime_preflight;

};

enum class WorkerCoordinatorStartStatus : std::uint8_t {
    NotStarted = 0,
    Started,
    StartedWithoutReadyWorkers,
    StartupExhausted,
};

struct WorkerCoordinatorStartResult {
    WorkerCoordinatorStartStatus status =
        WorkerCoordinatorStartStatus::NotStarted;
    std::size_t ready_workers = 0;
    std::string diagnostic;

    [[nodiscard]] bool started() const noexcept {
        return status == WorkerCoordinatorStartStatus::Started
            || status
                == WorkerCoordinatorStartStatus::StartedWithoutReadyWorkers;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return started();
    }
};

struct WorkerWorksetDispatchInfo {
    std::string execution_key_sha256;
    std::string program_package_sha256;
    std::string baseline_sha256;
    std::uint32_t item_count = 0;
    std::size_t encoded_size_bytes = 0;
};

struct ReadyWorkerDispatchSnapshot {
    std::size_t worker_id = 0;
    std::uint64_t process_generation = 0;
    savor::runtime::WorkerMode mode = savor::runtime::WorkerMode::Headless;
    std::string runtime_contract_sha256;
    // Immediate physical-admission credits only. The execution coordinator
    // must not use this value as the capacity of its future-work buffer.
    std::uint32_t available_item_credits = 0;
    bool accepting_workset = false;
    std::optional<std::uint64_t> resident_workset_id;

    std::optional<std::string> warm_execution_key_sha256;
    std::optional<std::string> warm_program_package_sha256;
};

struct WorkerCoordinatorEventContext {
    std::size_t worker_id = 0;
    std::uint64_t process_generation = 0;
    std::uint16_t wrms_protocol_version = 0;
};

using WorkerTerminalEnvelope =
    savor::runtime::DurableWorkerTerminalEnvelope;

struct WorkerUnavailableEvent {
    WorkerCoordinatorEventContext source;
    std::vector<std::uint64_t> affected_workset_ids;
    std::string diagnostic;
};

struct WorkerCoordinatorCallbacks {
    std::function<void()> availability_changed;
    std::function<void(
        const WorkerCoordinatorEventContext&,
        const savor::wrms::WorksetStatePayload&)> workset_state;
    std::function<void(
        const WorkerCoordinatorEventContext&,
        const savor::wrms::WorksetItemStartedPayload&)> item_started;
    std::function<void(
        const WorkerCoordinatorEventContext&,
        const savor::wrms::InvocationProgressPayload&)> item_progress;
    std::function<void(const WorkerTerminalEnvelope&)> item_terminal;
    std::function<void(
        const WorkerCoordinatorEventContext&,
        const savor::wrms::WorksetCreditsPayload&)> credits;
    std::function<void(
        const WorkerCoordinatorEventContext&,
        const savor::wrms::WorksetSummaryPayload&)> workset_summary;
    std::function<void(const WorkerUnavailableEvent&)> worker_unavailable;
};

enum class WorkerSubmitDisposition : std::uint8_t {
    Accepted = 0,
    AmbiguousAfterWrite,
    TargetTemporarilyUnavailable,
    StaleGeneration,
    DuplicateWorkset,
    InvalidWorkset,
    CoordinatorNotAccepting,
    DefiniteRejected,
};

struct WorkerExecutionTarget {
    std::size_t worker_id = 0;
    std::uint64_t process_generation = 0;
};

struct WorkerSubmitResult {
    WorkerSubmitDisposition disposition =
        WorkerSubmitDisposition::CoordinatorNotAccepting;
    std::optional<std::size_t> worker_id;
    std::uint64_t process_generation = 0;
    savor::wrms::RejectionCode rejection_code =
        savor::wrms::RejectionCode::None;
    std::string error_code;
    std::string diagnostic;
    std::optional<savor::runtime::SubmitWorksetResultV1>
        submission_receipt;

    [[nodiscard]] bool submitted() const noexcept {
        return disposition == WorkerSubmitDisposition::Accepted
            || disposition == WorkerSubmitDisposition::AmbiguousAfterWrite;
    }
};

enum class WorkerCommandDisposition : std::uint8_t {
    Accepted = 0,
    LocalRejected,
    NotFound,
    StaleRoute,
    DefiniteRejected,
    TransportCanceledBeforeWrite,
    AmbiguousAfterWrite,
    CoordinatorStopped,
};

enum class WorkerCommandKind : std::uint8_t {
    Unknown = 0,
    AcknowledgeTerminal,
    CancelWorksetItem,
    CancelWorkset,
};

struct WorkerCommandResult {
    WorkerCommandDisposition disposition =
        WorkerCommandDisposition::CoordinatorStopped;
    WorkerCommandKind command_kind = WorkerCommandKind::Unknown;
    std::optional<std::size_t> worker_id;
    std::uint64_t process_generation = 0;
    bool request_frame_written = false;
    bool correlated_result_received = false;
    savor::wrms::RejectionCode rejection_code =
        savor::wrms::RejectionCode::None;
    std::string error_code;
    std::string diagnostic;

    [[nodiscard]] bool accepted() const noexcept {
        return disposition == WorkerCommandDisposition::Accepted;
    }

    [[nodiscard]] bool terminal_item_cancellation_race() const noexcept {
        return disposition == WorkerCommandDisposition::DefiniteRejected
            && command_kind == WorkerCommandKind::CancelWorksetItem
            && rejection_code
                == savor::wrms::RejectionCode::WorksetItemAlreadyTerminal;
    }
};

struct WorkerCoordinatorTelemetry {
    std::uint64_t start_attempts = 0;
    std::uint64_t start_failures = 0;
    std::uint64_t worker_losses = 0;
    std::uint64_t submit_attempts = 0;
    std::uint64_t submit_accepted = 0;
    std::uint64_t submit_ambiguous = 0;
    std::uint64_t submit_rejected = 0;
    std::uint64_t submit_temporary_unavailable = 0;
    std::uint64_t submit_stale_generation = 0;
    std::uint64_t submit_deterministic_rejection = 0;
    std::uint64_t submit_transport_canceled_before_write = 0;
    std::uint64_t item_cancellation_attempts = 0;
    std::uint64_t item_cancellation_accepted = 0;
    std::uint64_t terminal_ack_attempts = 0;
    std::uint64_t terminal_ack_accepted = 0;
    std::uint64_t terminal_envelopes = 0;
    std::uint64_t liveness_probe_attempts = 0;
    std::uint64_t liveness_probe_failures = 0;
    std::uint64_t liveness_quarantines = 0;
};

class WorkerCoordinator {
public:
    explicit WorkerCoordinator(WorkerCoordinatorConfig config);
    ~WorkerCoordinator();

    WorkerCoordinator(const WorkerCoordinator&) = delete;
    WorkerCoordinator& operator=(const WorkerCoordinator&) = delete;

    WorkerCoordinatorStartResult Start();
    void Stop();

    void SetPaused(bool paused);
    [[nodiscard]] bool IsPaused() const noexcept;
    void SetDesiredWorkerCount(std::size_t desired_workers);
    [[nodiscard]] std::size_t DesiredWorkerCount() const noexcept;
    [[nodiscard]] bool IsStarted() const noexcept;
    [[nodiscard]] const savor::runtime::WorkerRuntimeContractV1&
        RuntimeContract() const noexcept;

    void SetCallbacks(WorkerCoordinatorCallbacks callbacks);

    [[nodiscard]] std::vector<ReadyWorkerDispatchSnapshot>
        SnapshotReadyWorkers() const;
    [[nodiscard]] std::vector<WorkerSnapshot> SnapshotWorkers() const;
    [[nodiscard]] FleetStartupSnapshot SnapshotFleetStartup() const;
    [[nodiscard]] WorkerCoordinatorTelemetry SnapshotTelemetry() const;
    [[nodiscard]] WorkerCoordinatorStartResult SnapshotStartResult() const;
    [[nodiscard]] bool ConfirmWorksetResidence(
        WorkerExecutionTarget target,
        savor::runtime::WorkerWorksetId expected_workset_id,
        savor::wrms::WorksetResidenceSnapshotV1* snapshot_out = nullptr,
        std::string* diagnostic_out = nullptr);
    void QuarantineWorkerGeneration(
        WorkerExecutionTarget target,
        std::string diagnostic);

    [[nodiscard]] WorkerSubmitResult SubmitWorksetToWorker(
        WorkerExecutionTarget target,
        const savor::runtime::WorkerWorksetDefinition& workset,
        const savor::runtime::InitialWorksetCancellationSidecarV1&
            initial_cancellations);
    [[nodiscard]] WorkerCommandResult CancelWorksetItem(
        savor::runtime::WorkerWorksetId workset_id,
        savor::runtime::WorkerWorksetItemId item_id,
        std::string reason);
    [[nodiscard]] WorkerCommandResult CancelWorkset(
        savor::runtime::WorkerWorksetId workset_id,
        std::string reason);
    [[nodiscard]] WorkerCommandResult AcknowledgeTerminal(
        const savor::runtime::WorkerItemTerminalCorrelation& terminal);

    bool SetWorkerVisualSurface(
        std::size_t worker_id,
        std::uint64_t render_widget_handle,
        std::string host_events_pipe_name = {});

    [[nodiscard]] static WorkerWorksetDispatchInfo DispatchInfoOf(
        const savor::runtime::WorkerWorksetDefinition& workset);

private:
    struct WorkerSlot;
    using WorkerSlotPtr = std::shared_ptr<WorkerSlot>;

    struct WorkerSlot {
        mutable std::mutex mutex;
        mutable std::mutex submission_mutex;
        std::size_t id = 0;
        std::uint64_t process_generation = 0;
        std::shared_ptr<savor::ProcessWorker> worker;
        bool ready = false;
        bool startup_in_progress = false;
        std::thread startup_thread;
        bool start_retry_exhausted = false;
        std::uint32_t start_attempts = 0;
        std::uint32_t observed_start_attempts = 0;
        std::chrono::steady_clock::time_point next_start_after{};
        std::string last_start_error;
        std::chrono::steady_clock::time_point
            next_liveness_probe{};
        std::uint32_t consecutive_liveness_failures = 0;
        savor::runtime::WorkerMode mode =
            savor::runtime::WorkerMode::Headless;
        std::string runtime_contract_sha256;
        std::uint32_t available_item_credits = 0;
        bool submission_in_progress = false;
        bool quarantine_requested = false;
        std::string quarantine_diagnostic;
        std::optional<std::uint64_t> active_workset_id;
        std::optional<std::uint64_t> submitting_workset_id;
        std::optional<std::string> warm_execution_key_sha256;
        std::optional<std::string> warm_program_package_sha256;
        std::uint64_t accepted_worksets = 0;
        std::uint64_t completed_worksets = 0;
        std::uint64_t visual_render_widget_handle = 0;
        std::string visual_host_events_pipe_name;
    };

    struct WorkerVisualSurface {
        std::uint64_t render_widget_handle = 0;
        std::string host_events_pipe_name;
    };

    struct WorksetRoute {
        std::size_t worker_id = 0;
        std::uint64_t process_generation = 0;
        bool terminal_state_observed = false;
        bool summary_observed = false;
        std::unordered_set<std::string> retained_terminals;
    };

    WorkerSlotPtr MakeWorkerSlot(std::size_t worker_id);
    WorkerSlotPtr GetWorkerSlot(std::size_t worker_id) const;
    std::vector<WorkerSlotPtr> CopyWorkerSlots() const;
    void ConfigureWorkerCallbacks(const WorkerSlotPtr& slot);

    WorkerCoordinatorRuntimePreflightResult PreflightWorkerSlot(
        const WorkerSlotPtr& slot);
    bool PrepareRuntimeSlot(
        std::size_t worker_id,
        std::filesystem::path* executable_out,
        std::string* error_out) const;
    bool StartWorkerSlot(const WorkerSlotPtr& slot);
    bool StartWorkerSlotAsync(const WorkerSlotPtr& slot);
    bool BeginWorkerSlotStart(
        const WorkerSlotPtr& slot,
        std::uint32_t* attempt_out);
    void CompleteWorkerSlotStart(
        const WorkerSlotPtr& slot,
        std::uint32_t attempt);
    void StopWorkerSlot(const WorkerSlotPtr& slot);
    void ResetWorkerSlot(const WorkerSlotPtr& slot);
    void LifecycleLoop();
    void ReconcileWorkerPool();
    void DetectLostWorkers();
    void ProbeWorkerLiveness();

    [[nodiscard]] ReadyWorkerDispatchSnapshot SnapshotReadyWorker(
        const WorkerSlot& slot) const;
    [[nodiscard]] WorkerCoordinatorEventContext EventContext(
        const WorkerSlot& slot) const;
    void HandleWorksetState(
        std::size_t worker_id,
        std::uint64_t process_generation,
        const savor::wrms::WorksetStatePayload& payload);
    void HandleSessionEvent(
        std::size_t worker_id,
        std::uint64_t process_generation,
        const savor::wrms::SessionEventPayload& payload);
    void HandleItemStarted(
        std::size_t worker_id,
        std::uint64_t process_generation,
        const savor::wrms::WorksetItemStartedPayload& payload);
    void HandleItemProgress(
        std::size_t worker_id,
        std::uint64_t process_generation,
        const savor::wrms::InvocationProgressPayload& payload);
    void HandleItemTerminal(
        std::size_t worker_id,
        std::uint64_t process_generation,
        const savor::wrms::WorksetItemTerminalPayload& payload);
    void HandleCredits(
        std::size_t worker_id,
        std::uint64_t process_generation,
        const savor::wrms::WorksetCreditsPayload& payload);
    void HandleWorksetSummary(
        std::size_t worker_id,
        std::uint64_t process_generation,
        const savor::wrms::WorksetSummaryPayload& payload);

    [[nodiscard]] std::optional<WorkerCoordinatorEventContext>
        CurrentEventContext(
            std::size_t worker_id,
            std::uint64_t process_generation) const;
    void NotifyAvailabilityChanged() const;
    void NotifyWorkerUnavailable(WorkerUnavailableEvent event) const;
    void RefreshStartResult(std::string diagnostic = {});
    void RemoveCompletedRouteIfPossible(std::uint64_t workset_id);
    void RemoveRoutesForWorker(
        std::size_t worker_id,
        std::uint64_t process_generation,
        std::vector<std::uint64_t>* removed_worksets);

    [[nodiscard]] static std::string TerminalKey(
        const savor::runtime::WorkerItemTerminalCorrelation& terminal);
    [[nodiscard]] static std::string TerminalKey(
        const savor::wrms::WorksetItemTerminalPayload& terminal);

    WorkerCoordinatorConfig config_;
    std::atomic<std::size_t> desired_worker_count_{0};
    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> paused_{false};

    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex start_result_mutex_;
    WorkerCoordinatorStartResult start_result_;
    std::thread lifecycle_thread_;

    mutable std::mutex workers_mutex_;
    std::vector<WorkerSlotPtr> workers_;
    std::unordered_map<std::size_t, WorkerVisualSurface> visual_surfaces_;

    mutable std::mutex runtime_preparation_mutex_;
    mutable std::optional<std::filesystem::path>
        prepared_runtime_worker_executable_;

    mutable std::mutex routes_mutex_;
    std::unordered_map<std::uint64_t, WorksetRoute> routes_;

    mutable std::mutex callbacks_mutex_;
    WorkerCoordinatorCallbacks callbacks_;

    WorkerStatusRegistry worker_status_;

    std::atomic<std::uint64_t> start_attempts_{0};
    std::atomic<std::uint64_t> start_failures_{0};
    std::atomic<std::uint64_t> worker_losses_{0};
    std::atomic<std::uint64_t> submit_attempts_{0};
    std::atomic<std::uint64_t> submit_accepted_{0};
    std::atomic<std::uint64_t> submit_ambiguous_{0};
    std::atomic<std::uint64_t> submit_rejected_{0};
    std::atomic<std::uint64_t> submit_temporary_unavailable_{0};
    std::atomic<std::uint64_t> submit_stale_generation_{0};
    std::atomic<std::uint64_t> submit_deterministic_rejection_{0};
    std::atomic<std::uint64_t>
        submit_transport_canceled_before_write_{0};
    std::atomic<std::uint64_t> item_cancellation_attempts_{0};
    std::atomic<std::uint64_t> item_cancellation_accepted_{0};
    std::atomic<std::uint64_t> terminal_ack_attempts_{0};
    std::atomic<std::uint64_t> terminal_ack_accepted_{0};
    std::atomic<std::uint64_t> terminal_envelopes_{0};
    std::atomic<std::uint64_t> liveness_probe_attempts_{0};
    std::atomic<std::uint64_t> liveness_probe_failures_{0};
    std::atomic<std::uint64_t> liveness_quarantines_{0};
};

} // namespace savor::runner::parallel::savordb
