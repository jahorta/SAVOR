#include "EmulationSession.h"

#include "../../Utils/Hash.h"

#include <algorithm>
#include <exception>
#include <iterator>
#include <sstream>
#include <utility>

namespace savor::runtime {
namespace {

template <typename Operation>
BackendResult CallBackend(const char* operation_name, Operation&& operation) noexcept
{
    try
    {
        return operation();
    }
    catch (const std::exception& ex)
    {
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            std::string(operation_name) + " threw: " + ex.what(),
            BackendIntegrity::Unknown);
    }
    catch (...)
    {
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            std::string(operation_name) + " threw",
            BackendIntegrity::Unknown);
    }
}

[[nodiscard]] BackendResult FromStopPointLifecycle(
    const char* operation,
    const StopPointLifecycleReceipt& receipt)
{
    if (receipt.ok)
        return BackendResult::Success();
    std::string message = operation;
    message += " failed";
    if (!receipt.error.message.empty())
    {
        message += ": ";
        message += receipt.error.message;
    }
    return BackendResult::Failure(
        BackendErrorCode::OperationFailed,
        std::move(message),
        receipt.physical_integrity == PhysicalStopIntegrity::Unknown
            ? BackendIntegrity::Unknown
            : BackendIntegrity::Preserved);
}

[[nodiscard]] ExecutionError ExecutionFailure(
    ExecutionErrorCode code,
    std::string message)
{
    return {code, std::move(message), BackendIntegrity::Preserved};
}

[[nodiscard]] ProbeRouterAdapterConfig
ProductionCaptureAdapterConfig()
{
    return {
        .source = {
            .id = StopSourceId(0x5341564f5201ull),
            .stable_name = "savor.capture.service",
            .diagnostic_label = "session capture profile",
        },
        .group_id =
            StopSubscriptionGroupId(0x5341564f5202ull),
        .first_subscription_id =
            StopSubscriptionId(0x5341564f5300ull),
        .cpu_observer_descriptor_id = 0x5341564fu,
        .epoch_policy = StopEpochPolicy::RebindAfterRestore,
        .priority = -100,
    };
}

} // namespace

EmulationSession::EmulationSession(
    SessionId session_id,
    std::unique_ptr<IDolphinBackend> backend,
    ExecutionEngineConfig execution_engine_config)
    : session_id_(session_id),
      backend_(std::move(backend)),
      execution_engine_config_(std::move(execution_engine_config))
{
}

EmulationSession::~EmulationSession()
{
    if (!backend_)
        return;

    try
    {
        if (!shutdown_ &&
            (!owner_bound_ ||
             owner_thread_ == std::this_thread::get_id()))
        {
            (void)Shutdown();
            return;
        }
        if (!backend_shutdown_attempted_)
        {
            backend_shutdown_attempted_ = true;
            (void)backend_->Close();
        }
    }
    catch (...)
    {
    }
    backend_.reset();
}

SessionSnapshot EmulationSession::snapshot() const noexcept
{
    return {
        session_id_,
        disposition_,
        state_epoch_,
        core_state_,
        opened_};
}

bool EmulationSession::ConfigureStopPointIngressNotification(
    std::atomic<std::uint64_t>* counter,
    void* notifier_context,
    StopPointIngressNotifier notifier) noexcept
{
    if (stop_router_ || opened_ || owner_bound_)
        return false;
    if ((notifier_context == nullptr) != (notifier == nullptr))
        return false;
    stop_ingress_notification_counter_ = counter;
    stop_ingress_notifier_context_ = notifier_context;
    stop_ingress_notifier_ = notifier;
    return true;
}

SessionOperationReceipt EmulationSession::Open(const SessionOpenOptions& options)
{
    const StateEpoch origin = state_epoch_;
    if (!BindOrCheckOwner())
    {
        return Reject(
            SessionOperation::Open,
            BackendErrorCode::InvalidState,
            "EmulationSession called from outside its owner thread");
    }
    if (shutdown_ || !backend_)
    {
        return Reject(
            SessionOperation::Open,
            BackendErrorCode::InvalidState,
            "EmulationSession has already shut down");
    }
    if (disposition_ != SessionDisposition::Closed)
    {
        return Reject(
            SessionOperation::Open,
            BackendErrorCode::InvalidState,
            "EmulationSession is already open");
    }

    BackendResult composition = InitializeServiceComposition(options);
    if (!composition.ok)
    {
        ApplyBackendFailure(composition);
        return {
            SessionOperation::Open,
            false,
            origin,
            state_epoch_,
            disposition_,
            std::move(composition)};
    }

    BackendResult result;
    if (options.read_only_movie_path.has_value())
    {
        if (!movie_service_)
        {
            result = BackendResult::Failure(
                BackendErrorCode::Unavailable,
                "Dolphin backend does not provide MovieService playback");
        }
        else
        {
            const MovieOperationReceipt movie =
                movie_service_->StartReadOnlyPlayback({
                    .dtm_path = *options.read_only_movie_path,
                });
            state_epoch_ = movie.state_epoch;
            result = FromMovieService(movie.result);
        }
    }
    else
    {
        StateOperationReceipt state = state_service_->Boot();
        state_epoch_ = state.resulting_epoch;
        result = FromStateService(state.result);
    }
    if (!result.ok)
    {
        const BackendResult cleanup = CleanupRuntimeComposition();
        if (!cleanup.ok)
        {
            result.integrity = BackendIntegrity::Unknown;
            result.message += result.message.empty() ? "" : "; ";
            result.message += cleanup.message;
        }
    }
    return Complete(
        SessionOperation::Open,
        origin,
        std::move(result),
        false);
}

SessionOperationReceipt EmulationSession::Reboot()
{
    if (!BindOrCheckOwner())
    {
        return Reject(
            SessionOperation::Reboot,
            BackendErrorCode::InvalidState,
            "EmulationSession called from outside its owner thread");
    }
    if (!CanOperate())
    {
        return Reject(
            SessionOperation::Reboot,
            BackendErrorCode::InvalidState,
            "EmulationSession is not in a reusable state");
    }

    if (!state_service_)
    {
        return Reject(
            SessionOperation::Reboot,
            BackendErrorCode::Unavailable,
            "StateService is unavailable");
    }
    return CompleteStateOperation(
        SessionOperation::Reboot,
        state_service_->Reboot());
}

SessionOperationReceipt EmulationSession::RestoreStateFile(
    const std::filesystem::path& path)
{
    if (!BindOrCheckOwner() || !CanOperate())
    {
        return Reject(
            SessionOperation::RestoreStateFile,
            BackendErrorCode::InvalidState,
            "EmulationSession cannot restore state in its current state");
    }
    try
    {
        StateFileImportRequest request;
        request.path = path;
        request.expected_sha256 =
            hash::sha256_of_file(path.string());
        request.compatibility =
            state_service_->compatibility();
        request.movie_mode = ExternalMovieImportMode::NoMovie;
        StateFileArtifactReceipt imported =
            ImportStateArtifact(request);
        if (!imported.result.ok)
        {
            return Reject(
                SessionOperation::RestoreStateFile,
                BackendErrorCode::InvalidArgument,
                imported.result.message);
        }
        return CompleteStateOperation(
            SessionOperation::RestoreStateFile,
            state_service_->RestoreFileArtifact(
                imported.artifact));
    }
    catch (const std::exception& ex)
    {
        return Reject(
            SessionOperation::RestoreStateFile,
            BackendErrorCode::InvalidArgument,
            ex.what());
    }
}

SessionOperationReceipt EmulationSession::RestoreStateBuffer(
    const std::vector<std::uint8_t>& bytes)
{
    if (!BindOrCheckOwner() || !CanOperate())
    {
        return Reject(
            SessionOperation::RestoreStateBuffer,
            BackendErrorCode::InvalidState,
            "EmulationSession cannot restore state in its current state");
    }
    (void)bytes;
    return Reject(
        SessionOperation::RestoreStateBuffer,
        BackendErrorCode::Unavailable,
        "Raw state-buffer restore is disconnected; use a StateService handle");
}

SessionOperationReceipt EmulationSession::SaveStateFile(
    const std::filesystem::path& path)
{
    const StateEpoch origin = state_epoch_;
    if (!BindOrCheckOwner() || !CanOperate())
    {
        return Reject(
            SessionOperation::SaveStateFile,
            BackendErrorCode::InvalidState,
            "EmulationSession cannot save state in its current state");
    }
    if (!execution_engine_ ||
        execution_engine_->has_active_operation() ||
        execution_engine_->snapshot().activity !=
            ExecutionActivity::IdlePaused ||
        !execution_engine_->snapshot().evidence.pause_confirmed)
    {
        return Reject(
            SessionOperation::SaveStateFile,
            BackendErrorCode::InvalidState,
            "EmulationSession can save state only while execution is idle-paused");
    }
    StateFileCaptureRequest request;
    request.path = path;
    const StateFileArtifactReceipt captured =
        CaptureStateArtifact(request);
    return Complete(
        SessionOperation::SaveStateFile,
        origin,
        FromStateService(captured.result),
        false);
}

SessionBufferReceipt EmulationSession::SaveStateBuffer()
{
    const StateEpoch origin = state_epoch_;
    if (!BindOrCheckOwner() || !CanOperate())
    {
        return {
            Reject(
                SessionOperation::SaveStateBuffer,
                BackendErrorCode::InvalidState,
                "EmulationSession cannot save state in its current state"),
            {}};
    }
    if (!execution_engine_ ||
        execution_engine_->has_active_operation() ||
        execution_engine_->snapshot().activity !=
            ExecutionActivity::IdlePaused ||
        !execution_engine_->snapshot().evidence.pause_confirmed)
    {
        return {
            Reject(
                SessionOperation::SaveStateBuffer,
                BackendErrorCode::InvalidState,
                "EmulationSession can save state only while execution is idle-paused"),
            {}};
    }

    return {
        Reject(
            SessionOperation::SaveStateBuffer,
            BackendErrorCode::Unavailable,
            "Raw state-buffer export is disconnected; use a StateService handle"),
        {}};
}

StateHandleReceipt EmulationSession::CaptureStateHandle()
{
    StateHandleReceipt receipt;
    if (!BindOrCheckOwner() || !CanOperate() ||
        !state_service_)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::InvalidState,
            "State handle capture requires a clean open session");
        return receipt;
    }
    if (!execution_engine_ ||
        execution_engine_->has_active_operation() ||
        execution_engine_->snapshot().activity !=
            ExecutionActivity::IdlePaused ||
        !execution_engine_->snapshot().evidence.pause_confirmed)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::InvalidState,
            "State handle capture requires idle-paused execution");
        return receipt;
    }
    StateHandleCaptureRequest request;
    if (movie_service_ &&
        movie_service_->activity() != MovieActivity::Inactive)
    {
        MovieCheckpointReceipt movie =
            movie_service_->CaptureCheckpoint();
        if (!movie.result.ok)
        {
            receipt.result = StateServiceResult::Failure(
                StateServiceErrorCode::ParticipantFailure,
                movie.result.message,
                movie.result.integrity);
            return receipt;
        }
        request.movie = std::move(movie.checkpoint);
    }
    return state_service_->CaptureMemoryHandle(request);
}

StateOperationReceipt EmulationSession::RestoreStateHandle(
    StateHandleId handle)
{
    if (!BindOrCheckOwner() || !CanOperate() ||
        !state_service_)
    {
        return {
            .result = StateServiceResult::Failure(
                StateServiceErrorCode::InvalidState,
                "State handle restore requires a clean open session"),
            .operation =
                StateReplacementKind::RestoreMemoryHandle,
            .origin_epoch = state_epoch_,
            .resulting_epoch = state_epoch_,
        };
    }
    return FinalizeStateReplacement(
        state_service_->RestoreMemoryHandle(handle));
}

StateFileArtifactReceipt
EmulationSession::CaptureStateArtifact(
    const StateFileCaptureRequest& request)
{
    StateFileArtifactReceipt receipt;
    receipt.path = request.path;
    if (!BindOrCheckOwner() || !CanOperate() ||
        !state_service_)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::InvalidState,
            "State artifact capture requires a clean open session");
        return receipt;
    }
    if (!execution_engine_ ||
        execution_engine_->has_active_operation() ||
        execution_engine_->snapshot().activity !=
            ExecutionActivity::IdlePaused ||
        !execution_engine_->snapshot().evidence.pause_confirmed)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::InvalidState,
            "State artifact capture requires idle-paused execution");
        return receipt;
    }
    StateFileCaptureRequest normalized = request;
    if (movie_service_ &&
        movie_service_->activity() != MovieActivity::Inactive)
    {
        MovieCheckpointReceipt movie =
            movie_service_->CaptureCheckpoint();
        if (!movie.result.ok)
        {
            receipt.result = StateServiceResult::Failure(
                StateServiceErrorCode::ParticipantFailure,
                movie.result.message,
                movie.result.integrity);
            return receipt;
        }
        normalized.movie = std::move(movie.checkpoint);
    }
    return state_service_->CaptureFileArtifact(normalized);
}

ImmutableStateArtifactCaptureReceipt
EmulationSession::CaptureImmutableStateArtifact(
    const StateFileCaptureRequest& request)
{
    ImmutableStateArtifactCaptureReceipt receipt;
    receipt.final_path = request.path;
    if (!BindOrCheckOwner() || !CanOperate() ||
        !state_service_)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::InvalidState,
            "Immutable state capture requires a clean open session");
        return receipt;
    }
    if (!execution_engine_ ||
        execution_engine_->has_active_operation() ||
        execution_engine_->snapshot().activity !=
            ExecutionActivity::IdlePaused ||
        !execution_engine_->snapshot().evidence.pause_confirmed)
    {
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::InvalidState,
            "Immutable state capture requires idle-paused execution");
        return receipt;
    }
    StateFileCaptureRequest normalized = request;
    if (movie_service_ &&
        movie_service_->activity() != MovieActivity::Inactive)
    {
        MovieCheckpointReceipt movie =
            movie_service_->CaptureCheckpoint();
        if (!movie.result.ok)
        {
            receipt.result = StateServiceResult::Failure(
                StateServiceErrorCode::ParticipantFailure,
                movie.result.message,
                movie.result.integrity);
            return receipt;
        }
        normalized.movie = std::move(movie.checkpoint);
    }
    return state_service_->CaptureImmutableArtifact(normalized);
}

StateFileArtifactReceipt
EmulationSession::CommitImmutableStateArtifact(
    const ImmutableStateArtifactPublicationReceipt& publication)
{
    if (!BindOrCheckOwner() || !state_service_)
    {
        StateFileArtifactReceipt receipt;
        receipt.artifact = publication.artifact;
        receipt.path = publication.state_path;
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::InvalidState,
            "Immutable artifact commit requires an open state service");
        return receipt;
    }
    return state_service_->CommitImmutableArtifact(publication);
}

StateServiceResult EmulationSession::AbandonImmutableStateArtifact(
    StateArtifactId artifact) noexcept
{
    if (!BindOrCheckOwner() || !state_service_)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::InvalidState,
            "Immutable artifact abandonment requires an open state service");
    }
    return state_service_->AbandonImmutableArtifact(artifact);
}

StateServiceResult EmulationSession::ReleaseStateArtifact(
    StateArtifactId artifact) noexcept
{
    if (!BindOrCheckOwner() || !state_service_)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::InvalidState,
            "State artifact release requires an open state service");
    }
    return state_service_->ReleaseFileArtifact(artifact);
}

StateFileArtifactReceipt
EmulationSession::ImportStateArtifact(
    const StateFileImportRequest& request)
{
    if (!BindOrCheckOwner() || !CanOperate() ||
        !state_service_)
    {
        StateFileArtifactReceipt receipt;
        receipt.path = request.path;
        receipt.external = true;
        receipt.result = StateServiceResult::Failure(
            StateServiceErrorCode::InvalidState,
            "State artifact import requires a clean open session");
        return receipt;
    }
    return state_service_->ImportFileArtifact(request);
}

StateOperationReceipt EmulationSession::RestoreStateArtifact(
    StateArtifactId artifact)
{
    if (!BindOrCheckOwner() || !CanOperate() ||
        !state_service_)
    {
        return {
            .result = StateServiceResult::Failure(
                StateServiceErrorCode::InvalidState,
                "State artifact restore requires a clean open session"),
            .operation =
                StateReplacementKind::RestoreFileArtifact,
            .origin_epoch = state_epoch_,
            .resulting_epoch = state_epoch_,
        };
    }
    return FinalizeStateReplacement(
        state_service_->RestoreFileArtifact(artifact));
}

SessionOperationReceipt EmulationSession::CaptureScreenshot(
    const std::filesystem::path& path,
    std::chrono::milliseconds timeout)
{
    const StateEpoch origin = state_epoch_;
    if (!BindOrCheckOwner() || !CanOperate())
    {
        return Reject(
            SessionOperation::Screenshot,
            BackendErrorCode::InvalidState,
            "EmulationSession cannot capture a screenshot in its current state");
    }
    if (!screenshot_service_)
    {
        return Reject(
            SessionOperation::Screenshot,
            BackendErrorCode::Unavailable,
            "ScreenshotService is unavailable");
    }
    ScreenshotReceipt screenshot =
        screenshot_service_->Capture(path, timeout, state_epoch_);
    BackendResult result = screenshot.ok
        ? BackendResult::Success()
        : BackendResult::Failure(
              screenshot.status == ScreenshotStatus::Failed
                  ? BackendErrorCode::OperationFailed
                  : BackendErrorCode::InvalidState,
              std::move(screenshot.message),
              screenshot.integrity);
    return Complete(
        SessionOperation::Screenshot,
        origin,
        std::move(result),
        false);
}

std::vector<TelemetryEvent> EmulationSession::DrainTelemetry()
{
    if (!BindOrCheckOwner() || !telemetry_bus_)
        return {};
    return telemetry_bus_->Drain();
}

SessionOperationReceipt EmulationSession::RevalidateStopPointsAfterJit()
{
    const StateEpoch origin = state_epoch_;
    if (!BindOrCheckOwner() || !CanOperate())
    {
        return Reject(
            SessionOperation::JitRevalidation,
            BackendErrorCode::InvalidState,
            "EmulationSession cannot revalidate stop points in its current state");
    }
    if (!stop_router_)
    {
        return {
            SessionOperation::JitRevalidation,
            true,
            origin,
            state_epoch_,
            disposition_,
            BackendResult::Success()};
    }

    auto host_activity = host_activity_.Track();
    BackendResult result = FromStopPointLifecycle(
        "stop-point JIT revalidation",
        stop_router_->RevalidateAfterJit());
    if (!result.ok)
        result = TaintAndCloseAfterStopPointFailure(std::move(result));
    return {
        SessionOperation::JitRevalidation,
        result.ok,
        origin,
        state_epoch_,
        disposition_,
        std::move(result)};
}

SessionOperationReceipt EmulationSession::ValidateBreakpointChangeNotification()
{
    const StateEpoch origin = state_epoch_;
    if (!BindOrCheckOwner() || !CanOperate())
    {
        return Reject(
            SessionOperation::BreakpointReconciliation,
            BackendErrorCode::InvalidState,
            "EmulationSession cannot validate breakpoint changes in its current state");
    }
    if (!stop_router_)
    {
        return {
            SessionOperation::BreakpointReconciliation,
            true,
            origin,
            state_epoch_,
            disposition_,
            BackendResult::Success()};
    }

    auto host_activity = host_activity_.Track();
    BackendResult result = FromStopPointLifecycle(
        "breakpoint-change reconciliation",
        stop_router_->ValidateBreakpointChangeNotification());
    if (!result.ok)
        result = TaintAndCloseAfterStopPointFailure(std::move(result));
    return {
        SessionOperation::BreakpointReconciliation,
        result.ok,
        origin,
        state_epoch_,
        disposition_,
        std::move(result)};
}

std::vector<StopRouteReceipt> EmulationSession::DrainStopPointEvents()
{
    if (!BindOrCheckOwner() || !stop_router_)
        return {};
    std::vector<StopRouteReceipt> receipts =
        stop_router_->DrainIngress();
    const bool reconcile = std::any_of(
        receipts.begin(),
        receipts.end(),
        [](const StopRouteReceipt& receipt) {
            return receipt.event.has_value() &&
                receipt.event->
                    requires_physical_reconcile_before_resume;
    });
    if (reconcile && capture_service_)
    {
        auto host_activity = host_activity_.Track();
        CaptureServiceReceipt capture =
            capture_service_->ReconcileBeforeResume();
        if (!capture.ok)
        {
            MarkTainted(
                capture.error.message.empty()
                    ? "CaptureService reconciliation failed"
                    : capture.error.message);
            StopRouteReceipt failure;
            failure.terminal = StopRouteTerminal::Failed;
            failure.error = {
                capture.requires_session_taint
                    ? StopPointErrorCode::PhysicalIntegrityUnknown
                    : StopPointErrorCode::PhysicalReconcileFailed,
                capture.error.message};
            failure.core_must_remain_stopped = true;
            receipts.push_back(std::move(failure));
        }
    }
    return receipts;
}

ExecutionSubmissionReceipt EmulationSession::SubmitExecution(
    ExecutionRequest request)
{
    if (!BindOrCheckOwner() || !CanOperate() || !execution_engine_)
    {
        return {
            false,
            {},
            ExecutionFailure(
                ExecutionErrorCode::InvalidState,
                "EmulationSession execution engine is unavailable")};
    }
    ExecutionSubmissionReceipt receipt =
        execution_engine_->Submit(std::move(request));
    if (!receipt.accepted &&
        receipt.error.integrity == BackendIntegrity::Unknown)
    {
        MarkTainted(
            receipt.error.message.empty()
                ? "execution submission could not preserve session integrity"
                : receipt.error.message);
    }
    return receipt;
}

ExecutionSubmissionReceipt EmulationSession::SubmitInterruptionChild(
    InterruptionFrameId frame_id,
    ExecutionRequest request)
{
    if (!BindOrCheckOwner() || !CanOperate() || !execution_engine_)
    {
        return {
            false,
            {},
            ExecutionFailure(
                ExecutionErrorCode::InvalidState,
                "EmulationSession execution engine is unavailable")};
    }
    ExecutionSubmissionReceipt receipt =
        execution_engine_->SubmitInterruptionChild(
        frame_id,
        std::move(request));
    if (!receipt.accepted &&
        receipt.error.integrity == BackendIntegrity::Unknown)
    {
        MarkTainted(
            receipt.error.message.empty()
                ? "interruption child submission could not preserve session integrity"
                : receipt.error.message);
    }
    return receipt;
}

ExecutionControlReceipt EmulationSession::CancelExecution(
    CancellationReason reason)
{
    if (!BindOrCheckOwner() || !CanOperate() || !execution_engine_)
    {
        return {
            false,
            {},
            ExecutionFailure(
                ExecutionErrorCode::InvalidState,
                "EmulationSession execution engine is unavailable")};
    }
    return execution_engine_->Cancel(reason);
}

ExecutionControlReceipt EmulationSession::CompleteInterruptionHandler(
    InterruptionFrameId frame_id,
    InterruptionHandlerOutcome outcome,
    std::string diagnostic)
{
    if (!BindOrCheckOwner() || !CanOperate() || !execution_engine_)
    {
        return {
            false,
            {},
            ExecutionFailure(
                ExecutionErrorCode::InvalidState,
                "EmulationSession execution engine is unavailable")};
    }
    return execution_engine_->CompleteInterruptionHandler(
        frame_id,
        outcome,
        std::move(diagnostic));
}

void EmulationSession::HandleStopPointReceipt(StopRouteReceipt receipt)
{
    if (!BindOrCheckOwner() || !execution_engine_)
        return;
    execution_engine_->HandleStopPointReceipt(std::move(receipt));
}

void EmulationSession::PumpExecution()
{
    if (!BindOrCheckOwner() || !execution_engine_)
        return;
    execution_engine_->Pump();
    RefreshCoreState();
}

std::vector<ExecutionEvent> EmulationSession::DrainExecutionEvents()
{
    if (!BindOrCheckOwner())
        return {};
    std::vector<ExecutionEvent> events;
    events.swap(retained_execution_events_);
    if (execution_engine_)
    {
        std::vector<ExecutionEvent> current =
            execution_engine_->DrainEvents();
        events.insert(
            events.end(),
            std::make_move_iterator(current.begin()),
            std::make_move_iterator(current.end()));
    }
    for (const ExecutionEvent& event : events)
    {
        if (!event.terminal)
            continue;
        if (event.terminal->integrity == BackendIntegrity::Unknown ||
            event.terminal->status ==
                ExecutionTerminalStatus::CleanupFailure)
        {
            MarkTainted(event.terminal->error.message.empty()
                ? "ExecutionEngine could not prove session integrity"
                : event.terminal->error.message);
            break;
        }
        if (event.terminal->status ==
            ExecutionTerminalStatus::CoreStalled)
        {
            MarkCleanWithDiagnostics(
                event.terminal->error.message.empty()
                    ? "core_stalled"
                    : event.terminal->error.message);
        }
    }
    return events;
}

ExecutionSnapshot EmulationSession::execution_snapshot() const
{
    if (!execution_engine_)
        return {};
    return execution_engine_->snapshot();
}

std::optional<std::chrono::steady_clock::time_point>
EmulationSession::next_execution_wake() const
{
    if (!execution_engine_)
        return std::nullopt;
    return execution_engine_->next_wake();
}

BackendExecutionCapabilityMask
EmulationSession::execution_capabilities() const noexcept
{
    IExecutionBackendPort* port =
        backend_ ? backend_->Execution() : nullptr;
    return port ? port->Capabilities() : 0;
}

SessionOperationReceipt EmulationSession::CheckHealth()
{
    const StateEpoch origin = state_epoch_;
    if (!BindOrCheckOwner() || !backend_ || shutdown_)
    {
        return Reject(
            SessionOperation::HealthCheck,
            BackendErrorCode::InvalidState,
            "EmulationSession is closed");
    }

    BackendHealthReport health;
    try
    {
        health = backend_->CheckHealth();
    }
    catch (const std::exception& ex)
    {
        MarkTainted(std::string("Dolphin backend health check threw: ") + ex.what());
        return {
            SessionOperation::HealthCheck,
            false,
            origin,
            state_epoch_,
            disposition_,
            BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                taint_diagnostic_,
                BackendIntegrity::Unknown)};
    }
    catch (...)
    {
        MarkTainted("Dolphin backend health check threw");
        return {
            SessionOperation::HealthCheck,
            false,
            origin,
            state_epoch_,
            disposition_,
            BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                taint_diagnostic_,
                BackendIntegrity::Unknown)};
    }
    core_state_ = health.core_state;
    if (health.healthy)
    {
        return {
            SessionOperation::HealthCheck,
            true,
            origin,
            state_epoch_,
            disposition_,
            BackendResult::Success()};
    }

    MarkTainted(health.diagnostic.empty()
        ? "Dolphin health check failed"
        : health.diagnostic);
    return {
        SessionOperation::HealthCheck,
        false,
        origin,
        state_epoch_,
        disposition_,
        BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            taint_diagnostic_,
            BackendIntegrity::Unknown)};
}

SessionOperationReceipt EmulationSession::Shutdown()
{
    const StateEpoch origin = state_epoch_;
    if (!BindOrCheckOwner())
    {
        return Reject(
            SessionOperation::Shutdown,
            BackendErrorCode::InvalidState,
            "EmulationSession called from outside its owner thread");
    }
    if (shutdown_)
        return *shutdown_receipt_;

    shutdown_ = true;
    BackendResult result = BackendResult::Success();
    if (execution_engine_)
    {
        result = execution_engine_->Shutdown();
        std::vector<ExecutionEvent> final_events =
            execution_engine_->DrainEvents();
        retained_execution_events_.insert(
            retained_execution_events_.end(),
            std::make_move_iterator(final_events.begin()),
            std::make_move_iterator(final_events.end()));
    }
    BackendResult cleanup = CleanupRuntimeComposition();
    if (!cleanup.ok)
        result = std::move(cleanup);
    else if (!cleanup.message.empty())
    {
        if (!result.message.empty())
            result.message += "; ";
        result.message += cleanup.message;
    }
    if (!result.ok)
        ApplyBackendFailure(result);

    backend_.reset();
    opened_ = false;
    core_state_ = BackendCoreState::Closed;
    if (disposition_ == SessionDisposition::Clean)
        disposition_ = SessionDisposition::Closed;

    shutdown_receipt_ = SessionOperationReceipt{
        SessionOperation::Shutdown,
        result.ok,
        origin,
        state_epoch_,
        disposition_,
        std::move(result)};
    return *shutdown_receipt_;
}

void EmulationSession::MarkTainted(std::string diagnostic)
{
    if (!BindOrCheckOwner())
        return;
    disposition_ = SessionDisposition::Tainted;
    taint_diagnostic_ = std::move(diagnostic);
}

void EmulationSession::MarkCleanWithDiagnostics(std::string diagnostic)
{
    if (!BindOrCheckOwner())
        return;
    if (disposition_ == SessionDisposition::Tainted)
        return;
    disposition_ = SessionDisposition::CleanWithDiagnostics;
    taint_diagnostic_ = std::move(diagnostic);
}

bool EmulationSession::BindOrCheckOwner() noexcept
{
    const std::thread::id current = std::this_thread::get_id();
    if (!owner_bound_)
    {
        owner_thread_ = current;
        owner_bound_ = true;
        return true;
    }
    return owner_thread_ == current;
}

bool EmulationSession::CanOperate() const noexcept
{
    return backend_ &&
        opened_ &&
        !shutdown_ &&
        (!state_service_ || !state_service_->is_tainted()) &&
        HasReusableCoreState() &&
        (disposition_ == SessionDisposition::Clean ||
         disposition_ == SessionDisposition::CleanWithDiagnostics);
}

bool EmulationSession::HasReusableCoreState() const noexcept
{
    return core_state_ == BackendCoreState::Running ||
        core_state_ == BackendCoreState::Paused;
}

SessionOperationReceipt EmulationSession::Reject(
    SessionOperation operation,
    BackendErrorCode code,
    std::string message) const
{
    return {
        operation,
        false,
        state_epoch_,
        state_epoch_,
        disposition_,
        BackendResult::Failure(code, std::move(message))};
}

SessionOperationReceipt EmulationSession::CompleteStateOperation(
    SessionOperation operation,
    const StateOperationReceipt& state)
{
    StateOperationReceipt finalized =
        FinalizeStateReplacement(state);
    return Complete(
        operation,
        finalized.origin_epoch,
        FromStateService(finalized.result),
        false);
}

StateOperationReceipt EmulationSession::FinalizeStateReplacement(
    StateOperationReceipt state)
{
    state_epoch_ = state.resulting_epoch;
    if (state.result.ok ||
        state.result.integrity != StateIntegrity::Unknown)
    {
        return state;
    }

    MarkTainted(
        state.result.message.empty()
            ? "State replacement integrity could not be proven"
            : state.result.message);
    const BackendResult cleanup = CleanupRuntimeComposition();
    if (!cleanup.ok)
    {
        state.result.message +=
            state.result.message.empty() ? "" : "; ";
        state.result.message +=
            cleanup.message.empty()
                ? "session cleanup could not be proven"
                : cleanup.message;
    }
    state.result.integrity = StateIntegrity::Unknown;
    return state;
}

SessionOperationReceipt EmulationSession::Complete(
    SessionOperation operation,
    StateEpoch origin,
    BackendResult result,
    bool)
{
    if (!result.ok)
    {
        if (operation == SessionOperation::Open ||
            (operation == SessionOperation::Reboot &&
             result.integrity == BackendIntegrity::Unknown))
        {
            opened_ = false;
        }
        RefreshCoreState();
        if (opened_ && !HasReusableCoreState())
        {
            result.integrity = BackendIntegrity::Unknown;
            if (result.message.empty())
                result.message = "Backend operation left the Dolphin core unusable";
        }
        ApplyBackendFailure(result);
        return {
            operation,
            false,
            origin,
            state_epoch_,
            disposition_,
            std::move(result)};
    }

    if (operation == SessionOperation::Open)
    {
        opened_ = true;
        disposition_ = SessionDisposition::Clean;
    }

    RefreshCoreState();
    if (opened_ && !HasReusableCoreState())
    {
        result = BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "Backend operation left the Dolphin core stopped, closed, or unknown",
            BackendIntegrity::Unknown);
        ApplyBackendFailure(result);
        return {
            operation,
            false,
            origin,
            state_epoch_,
            disposition_,
            std::move(result)};
    }

    return {
        operation,
        true,
        origin,
        state_epoch_,
        disposition_,
        std::move(result)};
}

BackendResult EmulationSession::InitializeStopPoints(StateEpoch first_epoch)
{
    IPhysicalStopPointBackendPort* port =
        backend_ ? backend_->PhysicalStopPoints() : nullptr;
    if (!port)
    {
        return BackendResult::Failure(
            BackendErrorCode::Unavailable,
            "Dolphin backend does not provide the required physical "
            "stop-point ownership facet");
    }

    try
    {
        physical_stop_manager_ =
            std::make_unique<PhysicalStopPointManager>(*port);
        stop_router_ =
            std::make_unique<StopPointRouter>(
                *physical_stop_manager_,
                nullptr,
                capture_service_.get(),
                &host_activity_);
    }
    catch (const std::exception& ex)
    {
        stop_router_.reset();
        physical_stop_manager_.reset();
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            std::string("failed constructing session stop-point ownership: ") +
                ex.what());
    }
    catch (...)
    {
        stop_router_.reset();
        physical_stop_manager_.reset();
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "failed constructing session stop-point ownership");
    }

    if (StopPointError error =
            stop_router_->SetIngressNotificationCounter(
                stop_ingress_notification_counter_))
    {
        stop_router_.reset();
        physical_stop_manager_.reset();
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "failed configuring stop-point ingress notification: " +
                error.message);
    }
    if (StopPointError error =
            stop_router_->SetIngressNotifier(
                stop_ingress_notifier_context_,
                stop_ingress_notifier_))
    {
        stop_router_.reset();
        physical_stop_manager_.reset();
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "failed configuring stop-point ingress notifier: " +
                error.message);
    }

    const StopPointLifecycleReceipt initialized =
        stop_router_->Initialize(first_epoch);
    if (!initialized.ok)
    {
        BackendResult failure =
            FromStopPointLifecycle("stop-point initialization", initialized);
        stop_router_.reset();
        physical_stop_manager_.reset();
        return failure;
    }
    if (capture_service_)
    {
        const CaptureServiceReceipt bound =
            capture_service_->BindRouter(
                *stop_router_,
                first_epoch);
        if (!bound.ok)
        {
            BackendResult failure = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                bound.error.message.empty()
                    ? "CaptureService could not bind the session router"
                    : bound.error.message,
                bound.requires_session_taint
                    ? BackendIntegrity::Unknown
                    : BackendIntegrity::Preserved);
            (void)stop_router_->StopIngressDrainAndCleanup();
            stop_router_.reset();
            physical_stop_manager_.reset();
            return failure;
        }
    }
    return BackendResult::Success();
}

BackendResult EmulationSession::InitializeServiceComposition(
    const SessionOpenOptions& options)
{
    IInputBackendPort* input =
        backend_ ? backend_->Input() : nullptr;
    IGuestMemoryBackendPort* memory =
        backend_ ? backend_->GuestMemory() : nullptr;
    IScreenshotBackendPort* screenshots =
        backend_ ? backend_->Screenshots() : nullptr;
    if (!input || !memory || !screenshots)
    {
        return BackendResult::Failure(
            BackendErrorCode::Unavailable,
            "Dolphin backend does not provide all required session-service facets");
    }

    try
    {
        telemetry_bus_ = std::make_unique<TelemetryBus>();
        input_arbiter_ = std::make_unique<InputArbiter>(*input);
        guest_memory_ = std::make_unique<GuestMemory>(*memory);
        guest_mutations_ =
            std::make_unique<GuestMutationService>(
                *guest_memory_,
                *memory);
        screenshot_service_ =
            std::make_unique<ScreenshotService>(*screenshots);
        artifact_sink_ = std::make_unique<RuntimeArtifactSink>(
            options.runtime_artifact_root);
        if (ICaptureBackendPort* capture = backend_->Captures())
        {
            capture_service_ = std::make_unique<CaptureService>(
                *capture,
                ProductionCaptureAdapterConfig());
        }

        state_backend_adapter_ =
            std::make_unique<SessionStateBackendAdapter>(
                *backend_);
        state_backend_adapter_->ConfigureOpen(options.backend);
        state_service_ = std::make_unique<StateService>(
            *state_backend_adapter_);
        movie_input_reservations_ =
            std::make_unique<InputMovieReservationAdapter>(
                *input_arbiter_,
                [this] {
                    return state_service_
                        ? state_service_->current_epoch()
                        : StateEpoch{};
                });
        if (IMovieBackendPort* movies = backend_->Movies())
        {
            movie_service_ = std::make_unique<MovieService>(
                *movies,
                *movie_input_reservations_,
                *state_service_);
        }

        resource_bindings_ =
            std::make_unique<
                program::SessionResourceBindingTable>(
                std::this_thread::get_id());
        resource_ledger_ =
            std::make_unique<SessionResourceLedger>(
                std::this_thread::get_id());

        StateServiceResult participant =
            state_service_->RegisterParticipant(*this);
        if (!participant.ok)
        {
            BackendResult failure =
                FromStateService(participant);
            (void)CleanupServices();
            state_service_.reset();
            state_backend_adapter_.reset();
            return failure;
        }
        if (movie_service_)
        {
            MovieServiceResult movie =
                movie_service_->RegisterForStateReplacement();
            if (!movie.ok)
            {
                BackendResult failure =
                    BackendResult::Failure(
                    BackendErrorCode::OperationFailed,
                    movie.message,
                    movie.integrity == StateIntegrity::Unknown
                        ? BackendIntegrity::Unknown
                        : BackendIntegrity::Preserved);
                (void)CleanupServices();
                state_service_.reset();
                state_backend_adapter_.reset();
                return failure;
            }
        }
    }
    catch (const std::exception& ex)
    {
        (void)CleanupServices();
        state_service_.reset();
        state_backend_adapter_.reset();
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            std::string(
                "failed constructing session service composition: ") +
                ex.what());
    }
    catch (...)
    {
        (void)CleanupServices();
        state_service_.reset();
        state_backend_adapter_.reset();
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "failed constructing session service composition");
    }
    return BackendResult::Success();
}

BackendResult EmulationSession::InitializeServices(StateEpoch first_epoch)
{
    if (!telemetry_bus_ || !input_arbiter_ ||
        !guest_memory_ || !guest_mutations_ ||
        !screenshot_service_)
    {
        return BackendResult::Failure(
            BackendErrorCode::Unavailable,
            "Session services were not composed before boot");
    }
    const InputArbiterOperationReceipt input =
        input_arbiter_->CommitStateEpoch(first_epoch);
    if (!input.ok)
    {
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            input.message.empty()
                ? "InputArbiter rejected the initial state epoch"
                : std::string(input.message),
            BackendIntegrity::Unknown);
    }
    guest_mutations_->CommitStateEpoch(first_epoch);
    screenshot_service_->CommitStateEpoch(first_epoch);
    return BackendResult::Success();
}

BackendResult EmulationSession::InitializeExecution(StateEpoch first_epoch)
{
    IExecutionBackendPort* port =
        backend_ ? backend_->Execution() : nullptr;
    if (!port || !stop_router_)
    {
        return BackendResult::Failure(
            BackendErrorCode::Unavailable,
            "Dolphin backend does not provide the required execution facet");
    }
    try
    {
        ExecutionEngineConfig config = execution_engine_config_;
        config.input_advance = input_arbiter_.get();
        config.host_activity = &host_activity_;
        execution_engine_ =
            std::make_unique<ExecutionEngine>(
                *port,
                *stop_router_,
                std::move(config));
    }
    catch (const std::exception& ex)
    {
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            std::string("failed constructing session ExecutionEngine: ") +
                ex.what());
    }
    catch (...)
    {
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "failed constructing session ExecutionEngine");
    }
    BackendResult initialized = execution_engine_->Initialize(first_epoch);
    if (!initialized.ok)
        execution_engine_.reset();
    return initialized;
}

BackendResult EmulationSession::CleanupServices() noexcept
{
    BackendResult result = BackendResult::Success();
    const auto retain_failure =
        [&result](BackendResult failure) {
            if (failure.ok)
                return;
            if (result.ok)
            {
                result = std::move(failure);
                return;
            }
            if (!failure.message.empty())
            {
                if (!result.message.empty())
                    result.message += "; ";
                result.message += failure.message;
            }
            if (failure.integrity == BackendIntegrity::Unknown)
                result.integrity = BackendIntegrity::Unknown;
        };
    const auto retain_diagnostic =
        [this, &result](std::string diagnostic) {
            if (diagnostic.empty())
                diagnostic = "Session cleanup completed with diagnostics";
            MarkCleanWithDiagnostics(diagnostic);
            if (!result.message.empty())
                result.message += "; ";
            result.message += diagnostic;
        };

    // Invocation/action resources must unwind while every owning service is
    // still alive. The binding table is the concrete dispatcher for the
    // ledger's otherwise opaque external identities.
    if (resource_ledger_ && resource_bindings_)
    {
        const ResourceUnwindResult resources =
            resource_ledger_->Shutdown(*resource_bindings_);
        const ResourceLedgerSnapshot resource_snapshot =
            resource_ledger_->snapshot();
        const bool diagnostic_cleanup_failure =
            resources.outcome ==
                ResourceUnwindOutcome::CleanupFailed &&
            resources.disposition ==
                ResourceCleanupDisposition::CleanWithDiagnostics;
        if ((resources.completed() || diagnostic_cleanup_failure) &&
            resources.disposition ==
                ResourceCleanupDisposition::CleanWithDiagnostics)
        {
            retain_diagnostic(resource_snapshot.diagnostic);
        }
        else if (!resources.completed() ||
                 resources.disposition ==
                     ResourceCleanupDisposition::TaintRequired)
        {
            retain_failure(BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                resources.error.message.empty()
                    ? "Session resource cleanup did not complete"
                    : resources.error.message,
                resources.disposition ==
                        ResourceCleanupDisposition::TaintRequired
                    ? BackendIntegrity::Unknown
                    : BackendIntegrity::Preserved));
        }
    }
    resource_ledger_.reset();

    if (capture_service_)
    {
        const CaptureServiceReceipt capture =
            capture_service_->Shutdown();
        if (!capture.ok)
        {
            retain_failure(BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                capture.error.message.empty()
                    ? "CaptureService shutdown failed"
                    : capture.error.message,
                capture.requires_session_taint
                    ? BackendIntegrity::Unknown
                    : BackendIntegrity::Preserved));
        }
    }
    capture_service_.reset();
    if (movie_service_)
    {
        MovieServiceResult movie = MovieServiceResult::Success();
        if (movie_service_->activity() ==
            MovieActivity::ReadOnlyPlayback)
        {
            movie = movie_service_->StopPlayback().result;
        }
        else if (movie_service_->activity() ==
                 MovieActivity::Recording)
        {
            movie = movie_service_->CancelRecording().result;
        }
        if (!movie.ok)
        {
            retain_failure(BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                movie.message.empty()
                    ? "MovieService shutdown failed"
                    : movie.message,
                movie.integrity == StateIntegrity::Unknown
                    ? BackendIntegrity::Unknown
                    : BackendIntegrity::Preserved));
        }
    }
    movie_service_.reset();
    if (movie_input_reservations_)
    {
        const MovieServiceResult movie_input =
            movie_input_reservations_->Shutdown();
        if (!movie_input.ok)
        {
            retain_failure(BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                movie_input.message.empty()
                    ? "Movie input reservation shutdown failed"
                    : movie_input.message,
                movie_input.integrity == StateIntegrity::Unknown
                    ? BackendIntegrity::Unknown
                    : BackendIntegrity::Preserved));
        }
    }
    movie_input_reservations_.reset();
    resource_bindings_.reset();
    screenshot_service_.reset();
    artifact_sink_.reset();
    if (guest_mutations_)
    {
        const GuestMutationCleanupReceipt mutations =
            guest_mutations_->Shutdown();
        if (!mutations.ok)
        {
            retain_failure(BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                mutations.message.empty()
                    ? "Guest mutation cleanup failed"
                    : mutations.message,
                mutations.taint_required
                    ? BackendIntegrity::Unknown
                    : BackendIntegrity::Preserved));
        }
    }
    guest_mutations_.reset();
    guest_memory_.reset();
    if (input_arbiter_)
    {
        const InputArbiterShutdownReceipt input =
            input_arbiter_->Shutdown();
        if (!input.ok || !input.cleanup_complete)
        {
            retain_failure(BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                input.message.empty()
                    ? "InputArbiter cleanup failed"
                    : input.message,
                input.taint_required
                    ? BackendIntegrity::Unknown
                    : BackendIntegrity::Preserved));
        }
    }
    input_arbiter_.reset();
    telemetry_bus_.reset();
    return result;
}

StateServiceResult EmulationSession::PrepareStateReplacement(
    const StateReplacementContext& context)
{
    if (state_replacement_prepared_)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::ParticipantFailure,
            "Session runtime state replacement is already prepared");
    }
    state_replacement_prepared_ = true;
    if (context.kind == StateReplacementKind::Boot)
        return StateServiceResult::Success();

    const auto fail = [&](BackendResult failure) {
        StateServiceResult result = StateServiceResult::Failure(
            StateServiceErrorCode::ParticipantFailure,
            failure.message,
            failure.integrity == BackendIntegrity::Unknown
                ? StateIntegrity::Unknown
                : StateIntegrity::Preserved);
        (void)RollbackStateReplacement(context);
        return result;
    };

    if (!execution_engine_)
    {
        return fail(BackendResult::Failure(
            BackendErrorCode::Unavailable,
            "ExecutionEngine is unavailable for state replacement"));
    }
    BackendResult execution =
        execution_engine_->PrepareStateReplacement();
    if (!execution.ok)
        return fail(std::move(execution));
    state_prepare_execution_ = true;

    if (capture_service_)
    {
        CaptureServiceReceipt capture =
            capture_service_->PrepareStateReplacement(
                context.origin_epoch);
        if (!capture.ok)
        {
            return fail(BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                capture.error.message,
                capture.requires_session_taint
                    ? BackendIntegrity::Unknown
                    : BackendIntegrity::Preserved));
        }
        state_prepare_capture_ = true;
    }

    BackendResult router = PrepareStopPointStateReplacement();
    if (!router.ok)
        return fail(std::move(router));
    state_prepare_router_ = true;

    if (resource_ledger_)
    {
        ResourceOperationResult ledger =
            resource_ledger_->BeginStateTransition(
                context.origin_epoch);
        if (!ledger.success)
        {
            return fail(BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                ledger.error.message));
        }
        state_prepare_ledger_ = true;
    }
    return StateServiceResult::Success();
}

StateServiceResult EmulationSession::CommitStateReplacement(
    const StateReplacementContext& context)
{
    if (!state_replacement_prepared_)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::ParticipantFailure,
            "Session runtime state replacement was not prepared");
    }

    const auto failure = [](BackendResult result) {
        return StateServiceResult::Failure(
            StateServiceErrorCode::ParticipantFailure,
            result.message,
            StateIntegrity::Unknown);
    };

    if (context.kind == StateReplacementKind::Boot)
    {
        state_epoch_ = context.candidate_epoch;
        BackendResult result =
            InitializeServices(context.candidate_epoch);
        if (result.ok)
            result = InitializeStopPoints(context.candidate_epoch);
        if (result.ok)
            result = InitializeExecution(context.candidate_epoch);
        if (result.ok && resource_ledger_)
        {
            ResourceOperationResult initialized =
                resource_ledger_->Initialize(
                    session_id_,
                    context.candidate_epoch,
                    ResourceOwnerId(1));
            if (!initialized.success)
            {
                result = BackendResult::Failure(
                    BackendErrorCode::OperationFailed,
                    initialized.error.message);
            }
        }
        if (result.ok)
        {
            RefreshCoreState();
            if (!HasReusableCoreState())
            {
                result = BackendResult::Failure(
                    BackendErrorCode::OperationFailed,
                    "Dolphin boot completed without a reusable paused core",
                    BackendIntegrity::Unknown);
            }
        }
        state_replacement_prepared_ = false;
        if (!result.ok)
            return failure(std::move(result));
        return StateServiceResult::Success();
    }

    const StateEpoch old_epoch = context.origin_epoch;
    const StateEpoch new_epoch = context.candidate_epoch;
    state_epoch_ = new_epoch;

    BackendResult result = BackendResult::Success();
    if (movie_input_reservations_)
    {
        MovieServiceResult input =
            movie_input_reservations_->CommitStateEpoch(new_epoch);
        if (!input.ok)
        {
            result = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                input.message,
                input.integrity == StateIntegrity::Unknown
                    ? BackendIntegrity::Unknown
                    : BackendIntegrity::Preserved);
        }
    }
    else if (input_arbiter_)
    {
        const InputArbiterOperationReceipt input =
            input_arbiter_->InvalidateForStateReplacement(new_epoch);
        if (!input.ok)
        {
            result = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                input.message.empty()
                    ? "InputArbiter state replacement failed"
                    : std::string(input.message),
                BackendIntegrity::Unknown);
        }
    }
    if (result.ok && guest_mutations_)
    {
        const std::vector<GuestMutationReceipt> superseded =
            guest_mutations_->SupersedeForStateReplacement(
                old_epoch);
        const auto failed = std::find_if(
            superseded.begin(),
            superseded.end(),
            [](const GuestMutationReceipt& receipt) {
                return !receipt.ok || receipt.taint_required;
            });
        if (failed != superseded.end())
        {
            result = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                failed->message.empty()
                    ? "Guest mutation supersession failed"
                    : failed->message,
                failed->taint_required
                    ? BackendIntegrity::Unknown
                    : BackendIntegrity::Preserved);
        }
        guest_mutations_->CommitStateEpoch(new_epoch);
    }
    if (result.ok && screenshot_service_)
        screenshot_service_->CommitStateEpoch(new_epoch);
    if (result.ok && execution_engine_)
        result = execution_engine_->CommitStateEpoch(new_epoch);
    if (result.ok)
        result = CommitStopPointStateReplacement(new_epoch);
    if (result.ok && capture_service_ &&
        state_prepare_capture_)
    {
        const CaptureServiceReceipt capture =
            capture_service_->CommitStateReplacement(new_epoch);
        if (!capture.ok)
        {
            result = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                capture.error.message,
                BackendIntegrity::Unknown);
        }
    }
    // The service composition is authoritative for the new epoch before the
    // ledger retires and rebinds invocation-owned receipts. A failed rebind
    // cannot roll the guest back and therefore fails with unknown integrity.
    if (result.ok && state_prepare_ledger_ &&
        resource_ledger_ && resource_bindings_)
    {
        ResourceUnwindResult ledger =
            resource_ledger_->CommitStateTransition(
                new_epoch,
                *resource_bindings_);
        const bool clean_with_diagnostics =
            ledger.disposition ==
                ResourceCleanupDisposition::CleanWithDiagnostics;
        const bool diagnostic_cleanup_failure =
            ledger.outcome ==
                ResourceUnwindOutcome::CleanupFailed &&
            clean_with_diagnostics;
        if ((!ledger.completed() && !diagnostic_cleanup_failure) ||
            ledger.disposition ==
                ResourceCleanupDisposition::TaintRequired)
        {
            result = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                ledger.error.message.empty()
                    ? "Resource ledger state transition failed"
                    : ledger.error.message,
                BackendIntegrity::Unknown);
        }
        else if (!ledger.rebind_requests.empty())
        {
            std::vector<ResourceRebindCompletion> completions;
            completions.reserve(ledger.rebind_requests.size());
            const auto compensate_rebinds =
                [this, new_epoch](
                    const std::vector<ResourceRebindCompletion>&
                        rebound) noexcept {
                    bool clean = true;
                    for (const ResourceRebindCompletion& completion :
                        rebound)
                    {
                        ResourceReceipt receipt;
                        receipt.id = completion.prior_receipt;
                        receipt.owner =
                            completion.replacement.owner;
                        receipt.service =
                            completion.replacement.service;
                        receipt.release =
                            completion.replacement.release;
                        receipt.acquisition_epoch = new_epoch;
                        receipt.epoch_policy =
                            completion.replacement.epoch_policy;
                        receipt.promotion =
                            completion.replacement.promotion;
                        receipt.cleanup =
                            completion.replacement.cleanup;
                        receipt.rebind_key =
                            completion.replacement.rebind_key;
                        receipt.diagnostic_label =
                            completion.replacement.diagnostic_label;
                        const ResourceReleaseResult released =
                            resource_bindings_->Release({
                                .receipt = std::move(receipt),
                                .reason =
                                    ResourceReleaseReason::Shutdown,
                                .current_epoch = new_epoch,
                                .cleanup_only = true,
                            });
                        clean = clean &&
                            (released.status ==
                                    ResourceReleaseStatus::Released ||
                             released.status ==
                                    ResourceReleaseStatus::
                                        SupersededByStateReplacement);
                    }
                    return clean;
                };
            for (const ResourceRebindRequest& request :
                ledger.rebind_requests)
            {
                program::SessionResourceRebindReceipt rebound =
                    resource_bindings_->Rebind(request, new_epoch);
                if (!rebound.success)
                {
                    (void)resource_ledger_->
                        FailStateTransitionRebinds(
                            rebound.diagnostic.empty()
                                ? "Session resource rebind failed"
                                : rebound.diagnostic);
                    result = BackendResult::Failure(
                        BackendErrorCode::OperationFailed,
                        rebound.diagnostic.empty()
                            ? "Session resource rebind failed"
                            : rebound.diagnostic,
                        BackendIntegrity::Unknown);
                    if (!compensate_rebinds(completions))
                    {
                        result.message +=
                            "; rebound resource compensation failed";
                    }
                    break;
                }
                completions.push_back(
                    std::move(rebound.completion));
            }
            if (result.ok)
            {
                ResourceAcquisitionResult rebound =
                    resource_ledger_->
                        CompleteStateTransitionRebinds(
                            completions);
                if (!rebound.success)
                {
                    const bool compensated =
                        compensate_rebinds(completions);
                    result = BackendResult::Failure(
                        BackendErrorCode::OperationFailed,
                        rebound.error.message.empty()
                            ? "Resource ledger rejected rebound resources"
                            : rebound.error.message,
                        BackendIntegrity::Unknown);
                    if (!compensated)
                    {
                        result.message +=
                            "; rebound resource compensation failed";
                    }
                }
            }
        }
        if (result.ok && clean_with_diagnostics)
        {
            std::string diagnostic =
                resource_ledger_->snapshot().diagnostic;
            if (diagnostic.empty())
            {
                diagnostic =
                    "State replacement resource cleanup completed with diagnostics";
            }
            MarkCleanWithDiagnostics(std::move(diagnostic));
        }
    }

    state_replacement_prepared_ = false;
    state_prepare_execution_ = false;
    state_prepare_capture_ = false;
    state_prepare_router_ = false;
    state_prepare_ledger_ = false;
    if (!result.ok)
        return failure(std::move(result));
    return StateServiceResult::Success();
}

StateServiceResult EmulationSession::RollbackStateReplacement(
    const StateReplacementContext& context) noexcept
{
    if (!state_replacement_prepared_)
        return StateServiceResult::Success();
    if (context.kind == StateReplacementKind::Boot)
    {
        state_replacement_prepared_ = false;
        return StateServiceResult::Success();
    }

    BackendResult first = BackendResult::Success();
    if (state_prepare_ledger_ && resource_ledger_)
    {
        ResourceOperationResult ledger =
            resource_ledger_->RollbackStateTransition(
                context.origin_epoch);
        if (!ledger.success)
        {
            first = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                ledger.error.message,
                BackendIntegrity::Unknown);
        }
    }
    if (state_prepare_router_)
    {
        BackendResult router =
            RollbackStopPointStateReplacement();
        if (!router.ok && first.ok)
            first = std::move(router);
    }
    if (state_prepare_capture_ && capture_service_)
    {
        CaptureServiceReceipt capture =
            capture_service_->RollbackStateReplacement(
                context.origin_epoch);
        if (!capture.ok && first.ok)
        {
            first = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                capture.error.message,
                capture.requires_session_taint
                    ? BackendIntegrity::Unknown
                    : BackendIntegrity::Preserved);
        }
    }
    if (state_prepare_execution_ && execution_engine_)
    {
        BackendResult execution =
            execution_engine_->RollbackStateReplacement(
                context.origin_epoch);
        if (!execution.ok && first.ok)
            first = std::move(execution);
    }

    state_replacement_prepared_ = false;
    state_prepare_execution_ = false;
    state_prepare_capture_ = false;
    state_prepare_router_ = false;
    state_prepare_ledger_ = false;
    if (!first.ok)
    {
        return StateServiceResult::Failure(
            StateServiceErrorCode::IntegrityFailure,
            first.message,
            StateIntegrity::Unknown);
    }
    return StateServiceResult::Success();
}

BackendResult EmulationSession::PrepareStopPointStateReplacement()
{
    if (!stop_router_)
        return BackendResult::Success();
    return FromStopPointLifecycle(
        "stop-point state-replacement preparation",
        stop_router_->PrepareStateReplacement(state_epoch_));
}

BackendResult EmulationSession::CommitStopPointStateReplacement(
    StateEpoch new_epoch)
{
    if (!stop_router_)
        return BackendResult::Success();
    return FromStopPointLifecycle(
        "stop-point state-replacement commit",
        stop_router_->CommitStateReplacement(new_epoch));
}

BackendResult EmulationSession::RollbackStopPointStateReplacement()
{
    if (!stop_router_)
        return BackendResult::Success();
    return FromStopPointLifecycle(
        "stop-point state-replacement rollback",
        stop_router_->RollbackStateReplacement(state_epoch_));
}

BackendResult EmulationSession::CleanupStopPoints()
{
    if (!stop_router_)
        return BackendResult::Success();
    return FromStopPointLifecycle(
        "stop-point cleanup",
        stop_router_->StopIngressDrainAndCleanup());
}

BackendResult EmulationSession::TaintAndCloseAfterStopPointFailure(
    BackendResult failure)
{
    failure.integrity = BackendIntegrity::Unknown;
    if (failure.message.empty())
        failure.message = "Stop-point integrity could not be proven";
    MarkTainted(failure.message);

    const BackendResult cleanup = CleanupRuntimeComposition();
    if (!cleanup.ok)
    {
        failure.message += "; ";
        failure.message += cleanup.message.empty()
            ? "session cleanup could not be proven"
            : cleanup.message;
    }
    opened_ = false;
    core_state_ = BackendCoreState::Closed;
    return failure;
}

BackendResult EmulationSession::CleanupRuntimeComposition() noexcept
{
    BackendResult result = BackendResult::Success();
    if (execution_engine_)
    {
        BackendResult execution = execution_engine_->Shutdown();
        if (!execution.ok)
            result = std::move(execution);
    }
    execution_engine_.reset();

    // Resource bindings release through their owning services. Unwind the
    // ledger and shut those services down while the stop-point router is
    // still alive; capture attachments in particular detach through both
    // CaptureService and the router.
    const BackendResult services = CleanupServices();
    if (!services.ok && result.ok)
        result = services;
    else if (!services.message.empty())
    {
        if (!result.message.empty())
            result.message += "; ";
        result.message += services.message;
    }

    const BackendResult stop_points = CleanupStopPoints();
    if (!stop_points.ok && result.ok)
        result = stop_points;
    stop_router_.reset();
    physical_stop_manager_.reset();

    if (state_service_)
    {
        backend_shutdown_attempted_ = true;
        StateServiceResult state = state_service_->Shutdown();
        if (!state.ok)
        {
            BackendResult state_failure = FromStateService(state);
            if (result.ok)
            {
                std::string diagnostic = std::move(result.message);
                result = std::move(state_failure);
                if (!diagnostic.empty())
                {
                    if (!result.message.empty())
                        result.message += "; ";
                    result.message += diagnostic;
                }
            }
            else if (!state_failure.message.empty())
            {
                if (!result.message.empty())
                    result.message += "; ";
                result.message += state_failure.message;
                if (state_failure.integrity ==
                    BackendIntegrity::Unknown)
                {
                    result.integrity = BackendIntegrity::Unknown;
                }
            }
        }
    }
    else if (backend_ && !backend_shutdown_attempted_)
    {
        backend_shutdown_attempted_ = true;
        BackendResult close = CallBackend(
            "Dolphin backend shutdown",
            [&] { return backend_->Close(); });
        if (!close.ok && result.ok)
            result = std::move(close);
    }
    state_service_.reset();
    state_backend_adapter_.reset();

    opened_ = false;
    core_state_ = BackendCoreState::Closed;
    return result;
}

BackendResult EmulationSession::FromStateService(
    const StateServiceResult& result)
{
    if (result.ok)
        return BackendResult::Success();
    BackendErrorCode code = BackendErrorCode::OperationFailed;
    switch (result.code)
    {
    case StateServiceErrorCode::InvalidArgument:
    case StateServiceErrorCode::CompatibilityMismatch:
    case StateServiceErrorCode::ArtifactFailure:
        code = BackendErrorCode::InvalidArgument;
        break;
    case StateServiceErrorCode::InvalidState:
    case StateServiceErrorCode::StaleEpoch:
    case StateServiceErrorCode::NotFound:
        code = BackendErrorCode::InvalidState;
        break;
    case StateServiceErrorCode::Unsupported:
        code = BackendErrorCode::Unavailable;
        break;
    case StateServiceErrorCode::None:
    case StateServiceErrorCode::CapacityExceeded:
    case StateServiceErrorCode::ParticipantFailure:
    case StateServiceErrorCode::BackendFailure:
    case StateServiceErrorCode::IntegrityFailure:
        code = BackendErrorCode::OperationFailed;
        break;
    }
    return BackendResult::Failure(
        code,
        result.message,
        result.integrity == StateIntegrity::Unknown
            ? BackendIntegrity::Unknown
            : BackendIntegrity::Preserved);
}

BackendResult EmulationSession::FromMovieService(
    const MovieServiceResult& result)
{
    if (result.ok)
        return BackendResult::Success();
    BackendErrorCode code = BackendErrorCode::OperationFailed;
    switch (result.code)
    {
    case MovieServiceErrorCode::InvalidArgument:
    case MovieServiceErrorCode::ArtifactFailure:
        code = BackendErrorCode::InvalidArgument;
        break;
    case MovieServiceErrorCode::InvalidState:
        code = BackendErrorCode::InvalidState;
        break;
    case MovieServiceErrorCode::Unsupported:
        code = BackendErrorCode::Unavailable;
        break;
    case MovieServiceErrorCode::None:
    case MovieServiceErrorCode::ReservationFailure:
    case MovieServiceErrorCode::StateFailure:
    case MovieServiceErrorCode::BackendFailure:
    case MovieServiceErrorCode::IntegrityFailure:
        break;
    }
    return BackendResult::Failure(
        code,
        result.message,
        result.integrity == StateIntegrity::Unknown
            ? BackendIntegrity::Unknown
            : BackendIntegrity::Preserved);
}

void EmulationSession::ApplyBackendFailure(const BackendResult& result)
{
    if (result.integrity == BackendIntegrity::Unknown)
    {
        MarkTainted(result.message.empty()
            ? "Backend failure left session integrity unknown"
            : result.message);
    }
}

void EmulationSession::RefreshCoreState() noexcept
{
    core_state_ = backend_ ? backend_->QueryCoreState() : BackendCoreState::Closed;
}

} // namespace savor::runtime
