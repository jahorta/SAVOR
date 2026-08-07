#pragma once

#include "IDolphinBackend.h"
#include "RuntimeTypes.h"
#include "Execution/ExecutionEngine.h"
#include "Execution/HostActivityTracker.h"
#include "Services/Capture/CaptureService.h"
#include "Services/Artifacts/RuntimeArtifactSink.h"
#include "Services/Input/InputArbiter.h"
#include "Services/Memory/GuestMemory.h"
#include "Services/Memory/GuestMutationService.h"
#include "Services/Movie/InputMovieReservationAdapter.h"
#include "Services/Movie/MovieService.h"
#include "Services/Resources/SessionResourceLedger.h"
#include "Services/Screenshot/ScreenshotService.h"
#include "Services/Savestate/SessionSavestateBackendAdapter.h"
#include "Services/Savestate/SavestateService.h"
#include "Services/Telemetry/TelemetryBus.h"
#include "StopPoints/StopPointRouter.h"
#include "ProgramRuntime/Actions/SessionResourceBindingTable.h"

#include <chrono>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace savor::runtime {

enum class SessionOperation : std::uint8_t
{
    Open,
    BeginWorkset,
    EndWorkset,
    RestoreSavestate,
    Screenshot,
    CoreRestartReconciliation,
    JitRevalidation,
    BreakpointReconciliation,
    HealthCheck,
    Shutdown,
};

struct SessionOpenOptions
{
    BackendOpenOptions backend;
    std::filesystem::path runtime_artifact_root;
};

struct SessionSnapshot
{
    SessionId session_id;
    SessionDisposition disposition = SessionDisposition::Closed;
    WorksetEpoch workset_epoch;
    BackendCoreState core_state = BackendCoreState::Closed;
    bool open = false;
};

struct SessionOperationReceipt
{
    SessionOperation operation = SessionOperation::HealthCheck;
    bool ok = false;
    WorksetEpoch workset_epoch;
    SessionDisposition disposition = SessionDisposition::Closed;
    BackendResult backend;
};

class EmulationSession final
{
public:
    EmulationSession(
        SessionId session_id,
        std::unique_ptr<IDolphinBackend> backend,
        ExecutionEngineConfig execution_engine_config = {});
    ~EmulationSession();

    EmulationSession(const EmulationSession&) = delete;
    EmulationSession& operator=(const EmulationSession&) = delete;

    [[nodiscard]] SessionSnapshot snapshot() const noexcept;
    [[nodiscard]] bool ConfigureStopPointIngressNotification(
        std::atomic<std::uint64_t>* counter,
        void* notifier_context,
        StopPointIngressNotifier notifier) noexcept;

    SessionOperationReceipt Open(const SessionOpenOptions& options);
    SessionOperationReceipt BeginWorkset(WorkerWorksetId workset_id);
    SessionOperationReceipt EndWorkset(WorkerWorksetId workset_id);

    [[nodiscard]] ImmutableSavestateArtifactCaptureReceipt
        CaptureImmutableSavestateArtifact(
            const SavestateCaptureRequest& request);
    [[nodiscard]] SavestateFileArtifactReceipt CommitImmutableSavestateArtifact(
        const ImmutableSavestateArtifactPublicationReceipt& publication);
    [[nodiscard]] SavestateServiceResult AbandonImmutableSavestateArtifact(
        SavestateArtifactId artifact) noexcept;
    [[nodiscard]] SavestateServiceResult ReleaseSavestateArtifact(
        SavestateArtifactId artifact) noexcept;

    SessionOperationReceipt CaptureScreenshot(
        const std::filesystem::path& path,
        std::chrono::milliseconds timeout);
    SessionOperationReceipt RevalidateStopPointsAfterJit();
    SessionOperationReceipt ValidateBreakpointChangeNotification();
    [[nodiscard]] std::vector<StopRouteReceipt> DrainStopPointEvents();
    ExecutionSubmissionReceipt SubmitExecution(ExecutionRequest request);
    ExecutionSubmissionReceipt SubmitInterruptionChild(
        InterruptionFrameId frame_id,
        ExecutionRequest request);
    ExecutionControlReceipt CancelExecution(
        CancellationReason reason = CancellationReason::ExternalRequest);
    ExecutionControlReceipt CompleteInterruptionHandler(
        InterruptionFrameId frame_id,
        InterruptionHandlerOutcome outcome,
        std::string diagnostic = {});
    void HandleStopPointReceipt(StopRouteReceipt receipt);
    void PumpExecution();
    [[nodiscard]] std::vector<ExecutionEvent> DrainExecutionEvents();
    [[nodiscard]] ExecutionSnapshot execution_snapshot() const;
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    next_execution_wake() const;
    [[nodiscard]] BackendExecutionCapabilityMask
    execution_capabilities() const noexcept;
    SessionOperationReceipt CheckHealth();
    SessionOperationReceipt Shutdown();

    void MarkTainted(std::string diagnostic);
    void MarkCleanWithDiagnostics(std::string diagnostic);

    [[nodiscard]] const std::string& taint_diagnostic() const noexcept
    {
        return taint_diagnostic_;
    }

    [[nodiscard]] StopPointRouter* stop_points() noexcept
    {
        return stop_router_.get();
    }

    [[nodiscard]] const StopPointRouter* stop_points() const noexcept
    {
        return stop_router_.get();
    }

    [[nodiscard]] InputArbiter* input_arbiter() noexcept
    {
        return input_arbiter_.get();
    }

    [[nodiscard]] GuestMemory* guest_memory() noexcept
    {
        return guest_memory_.get();
    }

    [[nodiscard]] GuestMutationService* guest_mutations() noexcept
    {
        return guest_mutations_.get();
    }

    [[nodiscard]] TelemetryBus* telemetry() noexcept
    {
        return telemetry_bus_.get();
    }

    [[nodiscard]] std::vector<TelemetryEvent> DrainTelemetry();

    [[nodiscard]] MovieService* movie_service() noexcept
    {
        return movie_service_.get();
    }

    [[nodiscard]] CaptureService* capture_service() noexcept
    {
        return capture_service_.get();
    }

    [[nodiscard]] RuntimeArtifactSink* artifact_sink() noexcept
    {
        return artifact_sink_.get();
    }

    [[nodiscard]] SessionResourceLedger* resources() noexcept
    {
        return resource_ledger_.get();
    }

    [[nodiscard]] program::SessionResourceBindingTable*
    resource_bindings() noexcept
    {
        return resource_bindings_.get();
    }

private:
    friend class WorksetStateCoordinator;

    [[nodiscard]] bool BindOrCheckOwner() noexcept;
    [[nodiscard]] bool CanOperate() const noexcept;
    [[nodiscard]] bool HasHealthyCoreState() const noexcept;
    [[nodiscard]] SessionOperationReceipt Reject(
        SessionOperation operation,
        BackendErrorCode code,
        std::string message) const;
    [[nodiscard]] SessionOperationReceipt Complete(
        SessionOperation operation,
        BackendResult result);
    [[nodiscard]] SavestateHandleReceipt CaptureWorksetBaselineHandle();
    [[nodiscard]] SavestateRestoreReceipt RestoreWorksetBaselineHandle(
        SavestateHandleId handle);
    [[nodiscard]] SavestateFileArtifactReceipt ImportWorksetBaselineArtifact(
        const SavestateImportRequest& request);
    [[nodiscard]] SavestateRestoreReceipt RestoreWorksetBaselineArtifact(
        SavestateArtifactId artifact);
    [[nodiscard]] SavestateServiceResult ReleaseWorksetBaselineHandle(
        SavestateHandleId handle) noexcept;
    [[nodiscard]] SavestateServiceResult ReleaseWorksetBaselineArtifact(
        SavestateArtifactId artifact) noexcept;
    [[nodiscard]] SavestateRestoreReceipt ReconcileRestoredSavestate(
        SavestateRestoreReceipt receipt,
        const std::optional<MovieCheckpointMetadata>& expected_movie);
    [[nodiscard]] SavestateRestoreReceipt RestoreWorksetBaselineTransaction(
        const SavestateMovieRestoreContext& context,
        const std::function<SavestateRestoreReceipt()>& restore);
    [[nodiscard]] BackendResult ValidateStopPointsBeforeMovieCoreStop();
    [[nodiscard]] BackendResult SettleStopPointsAfterMovieCoreStop();
    [[nodiscard]] BackendResult ValidateStopPointsAfterMovieCoreStart();
    void ApplyBackendFailure(const BackendResult& result);
    void RefreshCoreState() noexcept;
    [[nodiscard]] BackendResult InitializeStopPoints(WorksetEpoch first_epoch);
    [[nodiscard]] BackendResult InitializeServices(WorksetEpoch first_epoch);
    [[nodiscard]] BackendResult InitializeServiceComposition();
    [[nodiscard]] BackendResult InitializeExecution(WorksetEpoch first_epoch);
    [[nodiscard]] BackendResult CleanupServices() noexcept;
    [[nodiscard]] BackendResult CleanupStopPoints();
    [[nodiscard]] BackendResult TaintAndRetireSessionAfterStopPointFailure(
        BackendResult failure);
    [[nodiscard]] BackendResult CleanupRuntimeComposition() noexcept;
    [[nodiscard]] static BackendResult FromSavestateService(
        const SavestateServiceResult& result);
    [[nodiscard]] static BackendResult FromMovieService(
        const MovieServiceResult& result);

    SessionId session_id_;
    std::unique_ptr<IDolphinBackend> backend_;
    std::unique_ptr<PhysicalStopPointManager> physical_stop_manager_;
    std::unique_ptr<StopPointRouter> stop_router_;
    std::unique_ptr<TelemetryBus> telemetry_bus_;
    std::unique_ptr<InputArbiter> input_arbiter_;
    std::unique_ptr<GuestMemory> guest_memory_;
    std::unique_ptr<GuestMutationService> guest_mutations_;
    std::unique_ptr<ScreenshotService> screenshot_service_;
    std::unique_ptr<RuntimeArtifactSink> artifact_sink_;
    std::unique_ptr<SessionSavestateBackendAdapter> savestate_backend_adapter_;
    std::unique_ptr<SavestateService> savestate_service_;
    std::unique_ptr<InputMovieReservationAdapter>
        movie_input_reservations_;
    std::unique_ptr<MovieService> movie_service_;
    std::unique_ptr<CaptureService> capture_service_;
    std::unique_ptr<SessionResourceLedger> resource_ledger_;
    std::unique_ptr<program::SessionResourceBindingTable>
        resource_bindings_;
    HostActivityTracker host_activity_;
    ExecutionEngineConfig execution_engine_config_;
    std::unique_ptr<ExecutionEngine> execution_engine_;
    std::vector<ExecutionEvent> retained_execution_events_;
    SessionOpenOptions open_options_;
    SessionDisposition disposition_ = SessionDisposition::Closed;
    WorksetEpoch workset_epoch_;
    WorksetEpoch::value_type next_workset_epoch_ = 1;
    WorkerWorksetId active_workset_id_;
    BackendCoreState core_state_ = BackendCoreState::Closed;
    std::thread::id owner_thread_;
    bool owner_bound_ = false;
    bool opened_ = false;
    bool shutdown_ = false;
    bool backend_shutdown_attempted_ = false;
    std::optional<SessionOperationReceipt> shutdown_receipt_;
    std::string taint_diagnostic_;
    std::atomic<std::uint64_t>* stop_ingress_notification_counter_ = nullptr;
    void* stop_ingress_notifier_context_ = nullptr;
    StopPointIngressNotifier stop_ingress_notifier_ = nullptr;
};

} // namespace savor::runtime
