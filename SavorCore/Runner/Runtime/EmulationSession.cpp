#include "EmulationSession.h"
#include "../../Boot/Boot.h"

#include "../../Utils/Hash.h"

#include <algorithm>
#include <exception>
#include <iterator>
#include <sstream>
#include <stdexcept>
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
        .cpu_observer_descriptor_id = CanonicalStopCpuObserverId(
            CanonicalStopCpuObserver::CaptureProfile),
        .priority = -100,
    };
}

} // namespace

EmulationSession::EmulationSession(
    SessionId session_id,
    std::unique_ptr<IDolphinBackend> backend,
    ExecutionControlCoreConfig execution_control_core_config)
    : session_id_(session_id),
      backend_(std::move(backend)),
      execution_control_core_config_(std::move(execution_control_core_config))
{
}

[[nodiscard]] SavestateServiceResult SavestateMovieFailure(
    const MovieServiceResult& result,
    std::string_view fallback)
{
    return SavestateServiceResult::Failure(
        result.integrity == GuestIntegrity::Unknown
            ? SavestateServiceErrorCode::IntegrityFailure
            : SavestateServiceErrorCode::BackendFailure,
        result.message.empty() ? std::string(fallback) : result.message,
        result.integrity);
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
        workset_epoch_,
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

    if (!options.backend.session_filesystem_preparation_id.empty())
    {
        std::string preparation_error;
        if (!simboot::SessionFilesystemPreparer::Validate(
                options.backend.user_directory,
                options.backend.session_filesystem_preparation_id,
                options.backend.process_generation,
                &preparation_error))
        {
            return Reject(
                SessionOperation::Open,
                BackendErrorCode::InvalidArgument,
                preparation_error.empty()
                    ? "prepared session filesystem is invalid"
                    : preparation_error);
        }
    }

    open_options_ = options;
    BackendResult result = CallBackend(
        "Dolphin infrastructure boot",
        [&] { return backend_->Open(options.backend); });
    if (result.ok && backend_->HitTimeGuestMemory() == nullptr)
    {
        result = BackendResult::Failure(
            BackendErrorCode::Unavailable,
            "Dolphin backend does not provide the required hit-time guest-memory facet",
            BackendIntegrity::Preserved);
    }
    if (result.ok && backend_->Movies() == nullptr)
    {
        result = BackendResult::Failure(
            BackendErrorCode::Unavailable,
            "Dolphin backend does not provide the required movie facet",
            BackendIntegrity::Preserved);
    }
    if (result.ok)
    {
        opened_ = true;
        disposition_ = SessionDisposition::Clean;
        workset_epoch_ = {};
        active_workset_id_ = {};
        RefreshCoreState();
    }
    return Complete(SessionOperation::Open, std::move(result));
}

SessionOperationReceipt EmulationSession::OpenWorksetInitialization(
    WorkerWorksetId workset_id)
{
    if (!BindOrCheckOwner() || !opened_ || shutdown_ || !backend_ ||
        disposition_ == SessionDisposition::Tainted)
    {
        return Reject(
            SessionOperation::OpenWorksetInitialization,
            BackendErrorCode::InvalidState,
            "EmulationSession cannot open workset initialization in its current state");
    }
    if (!workset_id || active_workset_id_ || workset_epoch_ ||
        execution_control_core_ || stop_router_ || savestate_service_)
    {
        return Reject(
            SessionOperation::OpenWorksetInitialization,
            BackendErrorCode::InvalidState,
            "Another workset runtime is already active");
    }
    if (next_workset_epoch_ == 0)
    {
        MarkTainted("WorksetEpoch is exhausted");
        return Reject(
            SessionOperation::OpenWorksetInitialization,
            BackendErrorCode::InvalidState,
            taint_diagnostic_);
    }

    const WorksetEpoch activated(next_workset_epoch_++);
    workset_epoch_ = activated;
    active_workset_id_ = workset_id;
    guest_state_transaction_ = GuestStateTransaction::Initializing;

    BackendResult result = InitializeServiceComposition();
    if (result.ok)
        result = InitializeServices(activated);
    if (result.ok)
        result = InitializeStopPoints(activated);
    if (result.ok && resource_ledger_)
    {
        const ResourceOperationResult initialized = resource_ledger_->Initialize(
            session_id_, activated, ResourceOwnerId(1));
        if (!initialized.success)
        {
            result = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                initialized.error.message.empty()
                    ? "Workset resource ledger initialization failed"
                    : initialized.error.message,
                BackendIntegrity::Unknown);
        }
    }
    if (!result.ok)
    {
        const BackendResult cleanup = CleanupRuntimeComposition();
        if (!cleanup.ok && !cleanup.message.empty())
        {
            result.integrity = BackendIntegrity::Unknown;
            result.message += (result.message.empty() ? "" : "; ") +
                cleanup.message;
        }
        active_workset_id_ = {};
        workset_epoch_ = {};
        guest_state_transaction_ = GuestStateTransaction::None;
        ApplyBackendFailure(result);
        return {
            SessionOperation::OpenWorksetInitialization,
            false,
            activated,
            disposition_,
            std::move(result)};
    }
    return Complete(
        SessionOperation::OpenWorksetInitialization,
        BackendResult::Success());
}

SessionOperationReceipt EmulationSession::CommitWorksetInitialization(
    WorkerWorksetId workset_id)
{
    if (!BindOrCheckOwner() || !active_workset_id_ ||
        workset_id != active_workset_id_ || !workset_epoch_ ||
        execution_control_core_ || guest_state_transaction_ !=
            GuestStateTransaction::Initializing)
    {
        return Reject(
            SessionOperation::CommitWorksetInitialization,
            BackendErrorCode::InvalidState,
            "EmulationSession has no open workset initialization to commit");
    }
    BackendResult result = InitializeExecution(workset_epoch_);
    if (result.ok)
        guest_state_transaction_ = GuestStateTransaction::None;
    if (!result.ok)
        ApplyBackendFailure(result);
    return {
        SessionOperation::CommitWorksetInitialization,
        result.ok,
        workset_epoch_,
        disposition_,
        std::move(result)};
}

SessionOperationReceipt EmulationSession::AbortWorksetInitialization(
    WorkerWorksetId workset_id)
{
    if (!BindOrCheckOwner() || !active_workset_id_ ||
        workset_id != active_workset_id_ || !workset_epoch_)
    {
        return Reject(
            SessionOperation::AbortWorksetInitialization,
            BackendErrorCode::InvalidState,
            "EmulationSession has no matching workset initialization to abort");
    }
    const WorksetEpoch aborted = workset_epoch_;
    BackendResult result = CleanupRuntimeComposition();
    active_workset_id_ = {};
    workset_epoch_ = {};
    guest_state_transaction_ = GuestStateTransaction::None;
    if (!result.ok)
        ApplyBackendFailure(result);
    return {
        SessionOperation::AbortWorksetInitialization,
        result.ok,
        aborted,
        disposition_,
        std::move(result)};
}

SessionOperationReceipt EmulationSession::BeginWorksetItemReset(
    WorkerWorksetId workset_id)
{
    if (!BindOrCheckOwner() || !active_workset_id_ ||
        workset_id != active_workset_id_ || !workset_epoch_ ||
        !execution_control_core_ || guest_state_transaction_ !=
            GuestStateTransaction::None)
    {
        return Reject(
            SessionOperation::BeginWorksetItemReset,
            BackendErrorCode::InvalidState,
            "EmulationSession cannot begin an item reset in its current state");
    }
    BackendResult result = RemoveExecutionControlCore();
    if (result.ok)
        guest_state_transaction_ = GuestStateTransaction::ResettingItem;
    if (!result.ok)
        ApplyBackendFailure(result);
    return {
        SessionOperation::BeginWorksetItemReset,
        result.ok,
        workset_epoch_,
        disposition_,
        std::move(result)};
}

SessionOperationReceipt EmulationSession::CommitWorksetItemReset(
    WorkerWorksetId workset_id)
{
    if (!BindOrCheckOwner() || !active_workset_id_ ||
        workset_id != active_workset_id_ || !workset_epoch_ ||
        execution_control_core_ || guest_state_transaction_ !=
            GuestStateTransaction::ResettingItem)
    {
        return Reject(
            SessionOperation::CommitWorksetItemReset,
            BackendErrorCode::InvalidState,
            "EmulationSession has no open item reset to commit");
    }
    BackendResult result = InitializeExecution(workset_epoch_);
    if (result.ok)
        guest_state_transaction_ = GuestStateTransaction::None;
    if (!result.ok)
        ApplyBackendFailure(result);
    return {
        SessionOperation::CommitWorksetItemReset,
        result.ok,
        workset_epoch_,
        disposition_,
        std::move(result)};
}

SessionOperationReceipt EmulationSession::EndWorkset(
    WorkerWorksetId workset_id)
{
    if (!BindOrCheckOwner() || !active_workset_id_ ||
        workset_id != active_workset_id_ || !workset_epoch_)
    {
        return Reject(
            SessionOperation::EndWorkset,
            BackendErrorCode::InvalidState,
            "EmulationSession does not own the requested active workset");
    }
    const WorksetEpoch ended = workset_epoch_;
    BackendResult result = CleanupRuntimeComposition();
    active_workset_id_ = {};
    workset_epoch_ = {};
    guest_state_transaction_ = GuestStateTransaction::None;
    if (!result.ok)
        ApplyBackendFailure(result);
    return {
        SessionOperation::EndWorkset,
        result.ok,
        ended,
        disposition_,
        std::move(result)};
}


ImmutableSavestateArtifactCaptureReceipt
EmulationSession::CaptureImmutableSavestateArtifact(
    const SavestateCaptureRequest& request)
{
    ImmutableSavestateArtifactCaptureReceipt receipt;
    receipt.final_path = request.path;
    if (!BindOrCheckOwner() || !CanOperate() ||
        !savestate_service_)
    {
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::InvalidState,
            "Immutable state capture requires a clean open session");
        return receipt;
    }
    if (!execution_control_core_ ||
        execution_control_core_->has_active_operation() ||
        execution_control_core_->snapshot().activity !=
            ExecutionActivity::IdlePaused ||
        !execution_control_core_->snapshot().evidence.pause_confirmed)
    {
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::InvalidState,
            "Immutable state capture requires idle-paused execution");
        return receipt;
    }
    SavestateCaptureRequest normalized = request;
    if (normalized.movie_artifact_mode ==
        SavestateMovieArtifactMode::DeferredFinalRecordingPair)
    {
        if (!movie_service_ ||
            movie_service_->state() != MovieState::Recording)
        {
            receipt.result = SavestateServiceResult::Failure(
                SavestateServiceErrorCode::InvalidState,
                "Deferred recording-pair capture requires an active recording");
            return receipt;
        }
        MovieCheckpointReceipt movie =
            movie_service_->CaptureDeferredRecordingPairCheckpoint();
        if (!movie.result.ok)
        {
            receipt.result = SavestateServiceResult::Failure(
                SavestateServiceErrorCode::BackendFailure,
                movie.result.message,
                movie.result.integrity);
            return receipt;
        }
        // The exact movie bytes remain owned by MovieService as the prefix
        // witness. The immutable artifact published here is deliberately the
        // SAV alone; coordination later pairs it with the extended final DTM.
        normalized.movie.reset();
    }
    else if (movie_service_ &&
             movie_service_->state() != MovieState::Inactive)
    {
        MovieCheckpointReceipt movie =
            movie_service_->CaptureCheckpoint();
        if (!movie.result.ok)
        {
            receipt.result = SavestateServiceResult::Failure(
                SavestateServiceErrorCode::BackendFailure,
                movie.result.message,
                movie.result.integrity);
            return receipt;
        }
        normalized.movie = std::move(movie.checkpoint);
    }
    return savestate_service_->CaptureImmutableArtifact(normalized);
}

SavestateFileArtifactReceipt
EmulationSession::CommitImmutableSavestateArtifact(
    const ImmutableSavestateArtifactPublicationReceipt& publication)
{
    if (!BindOrCheckOwner() || !savestate_service_)
    {
        SavestateFileArtifactReceipt receipt;
        receipt.artifact = publication.artifact;
        receipt.path = publication.state_path;
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::InvalidState,
            "Immutable artifact commit requires an open savestate service");
        return receipt;
    }
    return savestate_service_->CommitImmutableArtifact(publication);
}

SavestateServiceResult EmulationSession::AbandonImmutableSavestateArtifact(
    SavestateArtifactId artifact) noexcept
{
    if (!BindOrCheckOwner() || !savestate_service_)
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::InvalidState,
            "Immutable artifact abandonment requires an open savestate service");
    }
    return savestate_service_->AbandonImmutableArtifact(artifact);
}

SavestateServiceResult EmulationSession::ReleaseSavestateArtifact(
    SavestateArtifactId artifact) noexcept
{
    if (!BindOrCheckOwner() || !savestate_service_)
    {
        return SavestateServiceResult::Failure(
            SavestateServiceErrorCode::InvalidState,
            "Savestate artifact release requires an open savestate service");
    }
    return savestate_service_->ReleaseFileArtifact(artifact);
}

SavestateFileArtifactReceipt
EmulationSession::ImportWorksetBaselineArtifact(
    const SavestateImportRequest& request)
{
    if (!BindOrCheckOwner() || !CanOperate() ||
        !savestate_service_)
    {
        SavestateFileArtifactReceipt receipt;
        receipt.path = request.path;
        receipt.external = true;
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::InvalidState,
            "State artifact import requires a clean open session");
        return receipt;
    }
    return savestate_service_->ImportFileArtifact(request);
}

SavestateRestoreReceipt EmulationSession::RestoreWorksetBaselineArtifact(
    SavestateArtifactId artifact)
{
    if (!BindOrCheckOwner() || !CanOperate() ||
        !savestate_service_)
    {
        return {
            .result = SavestateServiceResult::Failure(
                SavestateServiceErrorCode::InvalidState,
                "Savestate artifact restore requires an active workset"),
            .workset_epoch = workset_epoch_};
    }
    const auto description = savestate_service_->DescribeFileArtifact(artifact);
    if (!description)
    {
        return {
            .result = SavestateServiceResult::Failure(
                SavestateServiceErrorCode::NotFound,
                "Savestate artifact was not found"),
            .workset_epoch = workset_epoch_};
    }
    const SavestateMovieRestoreContext context{
        workset_epoch_, description->movie, description->external};
    return RestoreWorksetBaselineTransaction(
        context,
        [this, artifact]
        {
            return savestate_service_->RestoreFileArtifact(artifact);
        });
}

SavestateHandleReceipt EmulationSession::CaptureWorksetBaselineHandle()
{
    SavestateHandleReceipt receipt;
    if (!BindOrCheckOwner() || !CanOperate() || !savestate_service_)
    {
        receipt.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::InvalidState,
            "Workset baseline capture requires an active workset");
        return receipt;
    }
    SavestateHandleCaptureRequest request;
    if (movie_service_ && movie_service_->state() != MovieState::Inactive)
    {
        MovieCheckpointReceipt movie = movie_service_->CaptureCheckpoint();
        if (!movie.result.ok)
        {
            receipt.result = SavestateServiceResult::Failure(
                SavestateServiceErrorCode::BackendFailure,
                movie.result.message,
                movie.result.integrity);
            return receipt;
        }
        request.movie = std::move(movie.checkpoint);
    }
    request.lineage.edge = "workset.baseline.capture";
    request.lineage.producer = "WorksetStateCoordinator";
    return savestate_service_->CaptureMemoryHandle(request);
}

SavestateRestoreReceipt EmulationSession::RestoreWorksetBaselineHandle(
    SavestateHandleId handle)
{
    if (!BindOrCheckOwner() || !CanOperate() || !savestate_service_)
    {
        return {
            .result = SavestateServiceResult::Failure(
                SavestateServiceErrorCode::InvalidState,
                "Workset baseline restore requires an active workset"),
            .workset_epoch = workset_epoch_};
    }
    const auto description = savestate_service_->DescribeMemoryHandle(handle);
    if (!description)
    {
        return {
            .result = SavestateServiceResult::Failure(
                SavestateServiceErrorCode::NotFound,
                "Workset baseline handle was not found"),
            .workset_epoch = workset_epoch_};
    }
    const SavestateMovieRestoreContext context{
        workset_epoch_, description->movie, false};
    return RestoreWorksetBaselineTransaction(
        context,
        [this, handle]
        {
            return savestate_service_->RestoreMemoryHandle(handle);
        });
}

SavestateRestoreReceipt EmulationSession::RestoreWorksetBaselineTransaction(
    const SavestateMovieRestoreContext& context,
    const std::function<SavestateRestoreReceipt()>& restore)
{
    SavestateRestoreReceipt restored{
        .workset_epoch = workset_epoch_};
    const auto append_failure =
        [&restored](std::string_view prefix, std::string_view diagnostic)
        {
            if (!restored.result.message.empty())
                restored.result.message += "; ";
            restored.result.message += prefix;
            if (!diagnostic.empty())
            {
                restored.result.message += ": ";
                restored.result.message += diagnostic;
            }
            restored.result.integrity = GuestIntegrity::Unknown;
        };
    if (!stop_router_ || execution_control_core_ ||
        guest_state_transaction_ == GuestStateTransaction::None)
    {
        restored.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::InvalidState,
            "Workset baseline restoration requires an open guest-state transaction without execution evidence");
        return restored;
    }
    if ((resource_ledger_ &&
         resource_ledger_->snapshot().active_resource_count != 0) ||
        (resource_relationships_ && resource_relationships_->size() != 0) ||
        (capture_service_ && capture_service_->snapshot().attached))
    {
        restored.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::InvalidState,
            "Workset baseline restoration requires complete invocation-resource cleanup");
        return restored;
    }

    const StopPointLifecycleReceipt quiesced =
        stop_router_->QuiesceForWorksetBaselineRestore();
    if (!quiesced.ok)
    {
        const BackendResult failure = FromStopPointLifecycle(
            "workset baseline ingress quiescence", quiesced);
        restored.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::IntegrityFailure,
            failure.message,
            failure.integrity == BackendIntegrity::Unknown
                ? GuestIntegrity::Unknown
                : GuestIntegrity::Preserved);
        return ReconcileRestoredSavestate(
            std::move(restored), context.movie);
    }

    bool movie_prepared = false;
    bool preparation_failed = false;
    try
    {
        if (movie_service_)
        {
            const MovieServiceResult prepared =
                movie_service_->PrepareSavestateRestore(context);
            if (!prepared.ok)
            {
                restored.result = SavestateMovieFailure(
                    prepared,
                    "Movie history preparation failed");
                preparation_failed = true;
            }
            else
            {
                movie_prepared = true;
            }
        }
        if (!preparation_failed)
            restored = restore();
    }
    catch (const std::exception& ex)
    {
        restored.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::BackendFailure,
            std::string("Savestate backend operation threw: ") + ex.what(),
            GuestIntegrity::Unknown);
    }
    catch (...)
    {
        restored.result = SavestateServiceResult::Failure(
            SavestateServiceErrorCode::BackendFailure,
            "Savestate backend operation threw",
            GuestIntegrity::Unknown);
    }

    if (!restored.result.ok)
    {
        if (movie_prepared)
        {
            const MovieServiceResult rollback =
                movie_service_->RollbackSavestateRestore(context);
            if (!rollback.ok)
                append_failure("movie rollback failed", rollback.message);
        }
        const bool preserved =
            restored.result.integrity == GuestIntegrity::Preserved;
        const StopPointLifecycleReceipt ingress = preserved
            ? stop_router_->ResumeAfterFailedWorksetBaselineRestore()
            : stop_router_->ReconcileAfterWorksetBaselineRestore();
        if (!ingress.ok)
        {
            append_failure(
                "stop-point rollback failed",
                ingress.error.message);
        }
        return ReconcileRestoredSavestate(
            std::move(restored), context.movie);
    }

    if (movie_prepared)
    {
        const MovieServiceResult committed =
            movie_service_->CommitSavestateRestore(context);
        if (!committed.ok)
        {
            restored.result = SavestateMovieFailure(
                committed,
                "Restored movie cursor reconciliation failed");
            restored.result.integrity = GuestIntegrity::Unknown;
        }
    }
    const StopPointLifecycleReceipt reconciled =
        stop_router_->ReconcileAfterWorksetBaselineRestore();
    if (!reconciled.ok)
    {
        if (restored.result.ok)
        {
            restored.result = SavestateServiceResult::Failure(
                SavestateServiceErrorCode::IntegrityFailure,
                "Restored stop-point reconciliation failed",
                GuestIntegrity::Unknown);
        }
        append_failure(
            "physical stop-point reconciliation failed",
            reconciled.error.message);
    }
    return ReconcileRestoredSavestate(
        std::move(restored), context.movie);
}

SavestateServiceResult EmulationSession::ReleaseWorksetBaselineHandle(
    SavestateHandleId handle) noexcept
{
    return savestate_service_
        ? savestate_service_->ReleaseMemoryHandle(handle)
        : SavestateServiceResult::Failure(
              SavestateServiceErrorCode::InvalidState,
              "SavestateService is unavailable");
}

SavestateServiceResult EmulationSession::ReleaseWorksetBaselineArtifact(
    SavestateArtifactId artifact) noexcept
{
    return savestate_service_
        ? savestate_service_->ReleaseFileArtifact(artifact)
        : SavestateServiceResult::Failure(
              SavestateServiceErrorCode::InvalidState,
              "SavestateService is unavailable");
}

SavestateRestoreReceipt EmulationSession::ReconcileRestoredSavestate(
    SavestateRestoreReceipt receipt,
    const std::optional<MovieCheckpointMetadata>& expected_movie)
{
    if (!receipt.result.ok)
    {
        if (receipt.result.integrity == GuestIntegrity::Unknown)
            MarkTainted(receipt.result.message);
        return receipt;
    }
    receipt.workset_epoch = workset_epoch_;
    receipt.movie = expected_movie;
    return receipt;
}

SessionOperationReceipt EmulationSession::CaptureScreenshot(
    const std::filesystem::path& path,
    std::chrono::milliseconds timeout)
{
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
        screenshot_service_->Capture(path, timeout, workset_epoch_);
    BackendResult result = screenshot.ok
        ? BackendResult::Success()
        : BackendResult::Failure(
              screenshot.status == ScreenshotStatus::Failed
                  ? BackendErrorCode::OperationFailed
                  : BackendErrorCode::InvalidState,
              std::move(screenshot.message),
              screenshot.integrity);
    return Complete(SessionOperation::Screenshot, std::move(result));
}

std::vector<TelemetryEvent> EmulationSession::DrainTelemetry()
{
    if (!BindOrCheckOwner() || !telemetry_bus_)
        return {};
    return telemetry_bus_->Drain();
}

SessionOperationReceipt EmulationSession::RevalidateStopPointsAfterJit()
{
    const WorksetEpoch origin = workset_epoch_;
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
            workset_epoch_,
            disposition_,
            BackendResult::Success()};
    }

    auto host_activity = host_activity_.Track();
    BackendResult result = FromStopPointLifecycle(
        "stop-point JIT revalidation",
        stop_router_->RevalidateAfterJit());
    if (!result.ok)
        result = TaintAndRetireSessionAfterStopPointFailure(std::move(result));
    return {
        SessionOperation::JitRevalidation,
        result.ok,
        workset_epoch_,
        disposition_,
        std::move(result)};
}

SessionOperationReceipt EmulationSession::ValidateBreakpointChangeNotification()
{
    const WorksetEpoch origin = workset_epoch_;
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
            workset_epoch_,
            disposition_,
            BackendResult::Success()};
    }

    auto host_activity = host_activity_.Track();
    BackendResult result = FromStopPointLifecycle(
        "breakpoint-change reconciliation",
        stop_router_->ValidateBreakpointChangeNotification());
    if (!result.ok)
        result = TaintAndRetireSessionAfterStopPointFailure(std::move(result));
    return {
        SessionOperation::BreakpointReconciliation,
        result.ok,
        workset_epoch_,
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
            failure.terminal = StopRouteTerminal::RoutingFailure;
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
    if (!BindOrCheckOwner() || !CanOperate() || !execution_control_core_)
    {
        return {
            false,
            {},
            ExecutionFailure(
                ExecutionErrorCode::InvalidState,
                "EmulationSession execution engine is unavailable")};
    }
    ExecutionSubmissionReceipt receipt =
        execution_control_core_->Submit(std::move(request));
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
    if (!BindOrCheckOwner() || !CanOperate() || !execution_control_core_)
    {
        return {
            false,
            {},
            ExecutionFailure(
                ExecutionErrorCode::InvalidState,
                "EmulationSession execution engine is unavailable")};
    }
    ExecutionSubmissionReceipt receipt =
        execution_control_core_->SubmitInterruptionChild(
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
    if (!BindOrCheckOwner() || !CanOperate() || !execution_control_core_)
    {
        return {
            false,
            {},
            ExecutionFailure(
                ExecutionErrorCode::InvalidState,
                "EmulationSession execution engine is unavailable")};
    }
    return execution_control_core_->Cancel(reason);
}

ExecutionControlReceipt EmulationSession::CompleteInterruptionHandler(
    InterruptionFrameId frame_id,
    InterruptionHandlerOutcome outcome,
    std::string diagnostic)
{
    if (!BindOrCheckOwner() || !CanOperate() || !execution_control_core_)
    {
        return {
            false,
            {},
            ExecutionFailure(
                ExecutionErrorCode::InvalidState,
                "EmulationSession execution engine is unavailable")};
    }
    return execution_control_core_->CompleteInterruptionHandler(
        frame_id,
        outcome,
        std::move(diagnostic));
}

void EmulationSession::PumpExecution()
{
    if (!BindOrCheckOwner() || !execution_control_core_)
        return;
    execution_control_core_->Pump();
    RefreshCoreState();
}

std::vector<ExecutionEvent> EmulationSession::DrainExecutionEvents()
{
    if (!BindOrCheckOwner())
        return {};
    std::vector<ExecutionEvent> events;
    events.swap(retained_execution_events_);
    if (execution_control_core_)
    {
        std::vector<ExecutionEvent> current =
            execution_control_core_->DrainEvents();
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
                ? "ExecutionControlCore could not prove session integrity"
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

std::optional<ExecutionSnapshot> EmulationSession::execution_snapshot() const
{
    if (!execution_control_core_)
        return std::nullopt;
    return execution_control_core_->snapshot();
}

std::optional<std::chrono::steady_clock::time_point>
EmulationSession::next_execution_wake() const
{
    if (!execution_control_core_)
        return std::nullopt;
    return execution_control_core_->next_wake();
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
    const WorksetEpoch origin = workset_epoch_;
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
            workset_epoch_,
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
            workset_epoch_,
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
            workset_epoch_,
            disposition_,
            BackendResult::Success()};
    }

    MarkTainted(health.diagnostic.empty()
        ? "Dolphin health check failed"
        : health.diagnostic);
    return {
        SessionOperation::HealthCheck,
        false,
        workset_epoch_,
        disposition_,
        BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            taint_diagnostic_,
            BackendIntegrity::Unknown)};
}

SessionOperationReceipt EmulationSession::Shutdown()
{
    const WorksetEpoch active_epoch = workset_epoch_;
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
    if (execution_control_core_)
    {
        result = execution_control_core_->Shutdown();
        std::vector<ExecutionEvent> final_events =
            execution_control_core_->DrainEvents();
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
    if (backend_ && !backend_shutdown_attempted_)
    {
        backend_shutdown_attempted_ = true;
        BackendResult close = CallBackend(
            "Dolphin backend shutdown",
            [&] { return backend_->Close(); });
        if (!close.ok && result.ok)
            result = std::move(close);
        else if (!close.message.empty())
        {
            if (!result.message.empty())
                result.message += "; ";
            result.message += close.message;
        }
    }
    if (!result.ok)
        ApplyBackendFailure(result);

    backend_.reset();
    active_workset_id_ = {};
    workset_epoch_ = {};
    opened_ = false;
    core_state_ = BackendCoreState::Closed;
    if (disposition_ == SessionDisposition::Clean)
        disposition_ = SessionDisposition::Closed;

    shutdown_receipt_ = SessionOperationReceipt{
        SessionOperation::Shutdown,
        result.ok,
        {},
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
        workset_epoch_ &&
        active_workset_id_ &&
        HasHealthyCoreState() &&
        (disposition_ == SessionDisposition::Clean ||
         disposition_ == SessionDisposition::CleanWithDiagnostics);
}

bool EmulationSession::HasHealthyCoreState() const noexcept
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
        workset_epoch_,
        disposition_,
        BackendResult::Failure(code, std::move(message))};
}

MovieOperationReceipt EmulationSession::StartPreparedReadOnlyPlayback(
    MoviePreparationId preparation)
{
    MovieOperationReceipt receipt;
    receipt.operation = MovieOperation::StartPlayback;
    receipt.workset_epoch = workset_epoch_;
    if (!BindOrCheckOwner() || !CanOperate() || !movie_service_ ||
        !execution_control_core_ || !preparation)
    {
        receipt.result = MovieServiceResult::Failure(
            MovieServiceErrorCode::InvalidState,
            "Prepared movie start requires committed workset execution evidence");
        return receipt;
    }
    // Initialization already booted and proved the exact movie core while it
    // was paused. Activation only transfers the prepared playback into the
    // invocation after subscriptions are installed; it must not replace the
    // guest or invalidate the committed execution engine.
    receipt = movie_service_->StartPreparedReadOnlyPlayback(preparation);
    if (!receipt.result.ok &&
        receipt.result.integrity == GuestIntegrity::Unknown)
        MarkTainted(receipt.result.message);
    return receipt;
}


SessionOperationReceipt EmulationSession::Complete(
    SessionOperation operation,
    BackendResult result)
{
    if (!result.ok)
    {
        if (operation == SessionOperation::Open)
        {
            opened_ = false;
        }
        RefreshCoreState();
        if (opened_ && !HasHealthyCoreState())
        {
            result.integrity = BackendIntegrity::Unknown;
            if (result.message.empty())
                result.message = "Backend operation left the Dolphin core unusable";
        }
        ApplyBackendFailure(result);
        return {
            operation,
            false,
            workset_epoch_,
            disposition_,
            std::move(result)};
    }

    if (operation == SessionOperation::Open)
    {
        opened_ = true;
        disposition_ = SessionDisposition::Clean;
    }

    RefreshCoreState();
    if (opened_ && !HasHealthyCoreState())
    {
        result = BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "Backend operation left the Dolphin core stopped, closed, or unknown",
            BackendIntegrity::Unknown);
        ApplyBackendFailure(result);
        return {
            operation,
            false,
            workset_epoch_,
            disposition_,
            std::move(result)};
    }

    return {
        operation,
        true,
        workset_epoch_,
        disposition_,
        std::move(result)};
}

BackendResult EmulationSession::InitializeStopPoints(WorksetEpoch first_epoch)
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
        stop_cpu_evaluator_ =
            program::BuildCanonicalStopPointCpuEvaluator();
        if (!stop_cpu_evaluator_)
        {
            return BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "canonical stop-point CPU sampler registry is invalid");
        }
        physical_stop_manager_ =
            std::make_unique<PhysicalStopPointManager>(*port);
        stop_cpu_observers_ =
            std::make_unique<StopCpuObserverDispatcher>();
        if (capture_service_)
        {
            std::string error;
            if (!stop_cpu_observers_->Register(
                CanonicalStopCpuObserver::CaptureProfile,
                *capture_service_,
                &error))
            {
                throw std::logic_error(error);
            }
        }
        if (!derived_state_)
        {
            return BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "DerivedStateService was not constructed before stop-point ingress");
        }
        std::string derived_error;
        if (!stop_cpu_observers_->Register(
            CanonicalStopCpuObserver::DerivedState,
            *derived_state_,
            &derived_error))
        {
            throw std::logic_error(derived_error);
        }
        std::string freeze_error;
        if (!stop_cpu_observers_->Freeze(&freeze_error))
        {
            throw std::logic_error(freeze_error);
        }
        stop_router_ =
            std::make_unique<StopPointRouter>(
                *physical_stop_manager_,
                stop_cpu_evaluator_.get(),
                stop_cpu_observers_.get(),
                &host_activity_);
    }
    catch (const std::exception& ex)
    {
        stop_router_.reset();
        stop_cpu_observers_.reset();
        physical_stop_manager_.reset();
        stop_cpu_evaluator_.reset();
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            std::string("failed constructing session stop-point ownership: ") +
                ex.what());
    }
    catch (...)
    {
        stop_router_.reset();
        stop_cpu_observers_.reset();
        physical_stop_manager_.reset();
        stop_cpu_evaluator_.reset();
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "failed constructing session stop-point ownership");
    }

    if (StopPointError error =
            stop_router_->SetIngressNotificationCounter(
                stop_ingress_notification_counter_))
    {
        stop_router_.reset();
        stop_cpu_observers_.reset();
        physical_stop_manager_.reset();
        stop_cpu_evaluator_.reset();
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
        stop_cpu_observers_.reset();
        physical_stop_manager_.reset();
        stop_cpu_evaluator_.reset();
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
        stop_cpu_observers_.reset();
        physical_stop_manager_.reset();
        stop_cpu_evaluator_.reset();
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
            stop_cpu_observers_.reset();
            physical_stop_manager_.reset();
            stop_cpu_evaluator_.reset();
            return failure;
        }
    }
    const derived::DerivedStateReceipt derived_bound =
        derived_state_->BindRouter(*stop_router_, first_epoch);
    if (!derived_bound.ok)
    {
        BackendResult failure = BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            derived_bound.message.empty()
                ? "DerivedStateService could not bind the session router"
                : derived_bound.message,
            BackendIntegrity::Preserved);
        (void)stop_router_->StopIngressDrainAndCleanup();
        stop_router_.reset();
        stop_cpu_observers_.reset();
        physical_stop_manager_.reset();
        stop_cpu_evaluator_.reset();
        return failure;
    }
    return BackendResult::Success();
}

BackendResult EmulationSession::InitializeServiceComposition()
{
    IInputBackendPort* input =
        backend_ ? backend_->Input() : nullptr;
    IGuestMemoryBackendPort* memory =
        backend_ ? backend_->GuestMemory() : nullptr;
    IHitTimeGuestMemoryBackendPort* hit_time_memory =
        backend_ ? backend_->HitTimeGuestMemory() : nullptr;
    IScreenshotBackendPort* screenshots =
        backend_ ? backend_->Screenshots() : nullptr;
    IMovieBackendPort* movies =
        backend_ ? backend_->Movies() : nullptr;
    if (!input || !memory || !hit_time_memory || !screenshots || !movies)
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
        derived_state_ = std::make_unique<derived::DerivedStateService>(
            *guest_memory_,
            *hit_time_memory);
        guest_mutations_ =
            std::make_unique<GuestMutationService>(
                *guest_memory_,
                *memory);
        screenshot_service_ =
            std::make_unique<ScreenshotService>(*screenshots);
        artifact_sink_ = std::make_unique<RuntimeArtifactSink>(
            open_options_.runtime_artifact_root);
        if (ICaptureBackendPort* capture = backend_->Captures())
        {
            capture_service_ = std::make_unique<CaptureService>(
                *capture,
                ProductionCaptureAdapterConfig());
        }

        savestate_backend_adapter_ =
            std::make_unique<SessionSavestateBackendAdapter>(
                *backend_);
        savestate_service_ = std::make_unique<SavestateService>(
            *savestate_backend_adapter_,
            workset_epoch_,
            backend_->SavestateCompatibility());
        movie_input_reservations_ =
            std::make_unique<InputMovieReservationAdapter>(
                *input_arbiter_,
                [this] {
                    return workset_epoch_;
                });
        movie_service_ = std::make_unique<MovieService>(
            *movies,
            *movie_input_reservations_,
            [this] { return workset_epoch_; },
            [this] {
                const BackendResult result =
                    ValidateStopPointsBeforeMovieCoreStop();
                return result.ok
                    ? MovieServiceResult::Success()
                    : MovieServiceResult::Failure(
                          MovieServiceErrorCode::IntegrityFailure,
                          result.message,
                          result.integrity == BackendIntegrity::Unknown
                              ? GuestIntegrity::Unknown
                              : GuestIntegrity::Preserved);
            },
            [this] {
                const BackendResult result =
                    SettleStopPointsAfterMovieCoreStop();
                return result.ok
                    ? MovieServiceResult::Success()
                    : MovieServiceResult::Failure(
                          MovieServiceErrorCode::IntegrityFailure,
                          result.message,
                          result.integrity == BackendIntegrity::Unknown
                              ? GuestIntegrity::Unknown
                              : GuestIntegrity::Preserved);
            },
            [this] {
                const BackendResult result =
                    ValidateStopPointsAfterMovieCoreStart();
                return result.ok
                    ? MovieServiceResult::Success()
                    : MovieServiceResult::Failure(
                          MovieServiceErrorCode::IntegrityFailure,
                          result.message,
                          result.integrity == BackendIntegrity::Unknown
                              ? GuestIntegrity::Unknown
                              : GuestIntegrity::Preserved);
            });

        resource_relationships_ =
            std::make_unique<
                program::SessionResourceBindingTable>(
                std::this_thread::get_id());
        resource_ledger_ =
            std::make_unique<SessionResourceLedger>(
                std::this_thread::get_id());

    }
    catch (const std::exception& ex)
    {
        (void)CleanupServices();
        savestate_service_.reset();
        savestate_backend_adapter_.reset();
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            std::string(
                "failed constructing session service composition: ") +
                ex.what());
    }
    catch (...)
    {
        (void)CleanupServices();
        savestate_service_.reset();
        savestate_backend_adapter_.reset();
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "failed constructing session service composition");
    }
    return BackendResult::Success();
}

BackendResult EmulationSession::InitializeServices(WorksetEpoch first_epoch)
{
    if (!telemetry_bus_ || !input_arbiter_ ||
        !guest_memory_ || !guest_mutations_ ||
        !screenshot_service_ || !movie_service_)
    {
        return BackendResult::Failure(
            BackendErrorCode::Unavailable,
            "Session services were not composed before boot");
    }
    const InputArbiterOperationReceipt input =
        input_arbiter_->InitializeWorksetEpoch(first_epoch);
    if (!input.ok)
    {
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            input.message.empty()
                ? "InputArbiter rejected the initial state epoch"
                : std::string(input.message),
            BackendIntegrity::Unknown);
    }
    guest_mutations_->InitializeWorksetEpoch(first_epoch);
    screenshot_service_->InitializeWorksetEpoch(first_epoch);
    return BackendResult::Success();
}

BackendResult EmulationSession::InitializeExecution(WorksetEpoch first_epoch)
{
    IExecutionBackendPort* port =
        backend_ ? backend_->Execution() : nullptr;
    if (!port || !stop_router_ || !movie_service_)
    {
        return BackendResult::Failure(
            BackendErrorCode::Unavailable,
            "Dolphin backend does not provide the required execution or movie service");
    }
    try
    {
        ExecutionControlCoreConfig config = execution_control_core_config_;
        config.input_relationships = input_arbiter_.get();
        config.host_activity = &host_activity_;
        execution_control_core_ =
            std::make_unique<ExecutionControlCore>(
                *port,
                *movie_service_,
                *stop_router_,
                std::move(config));
    }
    catch (const std::exception& ex)
    {
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            std::string("failed constructing session ExecutionControlCore: ") +
                ex.what());
    }
    catch (...)
    {
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "failed constructing session ExecutionControlCore");
    }
    BackendResult initialized = execution_control_core_->Initialize(first_epoch);
    if (!initialized.ok)
        execution_control_core_.reset();
    return initialized;
}

BackendResult EmulationSession::RemoveExecutionControlCore() noexcept
{
    if (!execution_control_core_)
        return BackendResult::Success();
    BackendResult result = execution_control_core_->Shutdown();
    execution_control_core_.reset();
    retained_execution_events_.clear();
    return result;
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

    const BackendResult derived_state = CloseDerivedStateItem();
    if (!derived_state.ok)
        retain_failure(derived_state);
    derived_state_.reset();

    // Invocation/action resources must unwind while every owning service is
    // still alive. The binding table is the concrete dispatcher for the
    // ledger's otherwise opaque external identities.
    if (resource_ledger_ && resource_relationships_)
    {
        const ResourceUnwindResult resources =
            resource_ledger_->Shutdown(*resource_relationships_);
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
        if (movie_service_->is_tainted())
        {
            retain_failure(BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "MovieService ended the workset with unknown guest integrity",
                BackendIntegrity::Unknown));
        }
        if (movie_service_->state() == MovieState::ReadOnlyPlayback ||
            movie_service_->state() == MovieState::PlaybackEnded)
        {
            movie = movie_service_->StopPlayback().result;
        }
        else if (movie_service_->state() ==
                 MovieState::PreparedReadOnlyPlayback)
        {
            movie = movie_service_->AbandonPreparedReadOnlyPlayback(
                movie_service_->preparation()).result;
        }
        else if (movie_service_->state() == MovieState::Recording)
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
                movie.integrity == GuestIntegrity::Unknown
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
                movie_input.integrity == GuestIntegrity::Unknown
                    ? BackendIntegrity::Unknown
                    : BackendIntegrity::Preserved));
        }
    }
    movie_input_reservations_.reset();
    resource_relationships_.reset();
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

BackendResult EmulationSession::ActivateDerivedStateForItem(
    const derived::WorksetDerivedStateBindingV1& binding,
    WorkerWorksetItemId item_id)
{
    if (!derived_state_ || !execution_control_core_ || guest_state_transaction_ !=
            GuestStateTransaction::None || !workset_epoch_)
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidState,
            "Derived state requires committed workset execution evidence");
    }
    const ExecutionSnapshot execution = execution_control_core_->snapshot();
    if (!binding.blocks.empty())
    {
        if (execution.activity != ExecutionActivity::IdlePaused ||
            execution.evidence.core_state != BackendCoreState::Paused ||
            !execution.evidence.pause_confirmed || execution.evidence.pc == 0)
        {
            return BackendResult::Failure(
                BackendErrorCode::InvalidState,
                "Derived state requires an authoritative paused item-start point");
        }
        if (StopPointError error =
                stop_router_->EstablishPausedCurrentPoint(execution.evidence.pc))
        {
            return BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                error.message.empty()
                    ? "Derived state could not establish the paused item-start point"
                    : std::move(error.message),
                BackendIntegrity::Preserved);
        }
    }
    const auto receipt = derived_state_->ActivateItem(
        binding,
        workset_epoch_,
        item_id);
    return receipt.ok
        ? BackendResult::Success()
        : BackendResult::Failure(
              receipt.code == derived::DerivedStateErrorCode::InvalidArgument
                  ? BackendErrorCode::InvalidArgument
                  : BackendErrorCode::OperationFailed,
              receipt.message,
              BackendIntegrity::Preserved);
}

BackendResult EmulationSession::CloseDerivedStateItem() noexcept
{
    if (!derived_state_ || !derived_state_->active())
        return BackendResult::Success();
    const auto receipt = derived_state_->CloseItem();
    return receipt.ok
        ? BackendResult::Success()
        : BackendResult::Failure(
              BackendErrorCode::OperationFailed,
              receipt.message,
              BackendIntegrity::Unknown);
}


BackendResult EmulationSession::ValidateStopPointsBeforeMovieCoreStop()
{
    if (!stop_router_ || !workset_epoch_)
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidState,
            "Movie core stop validation requires an active workset router",
            BackendIntegrity::Unknown);
    }
    return FromStopPointLifecycle(
        "pre-stop movie stop-point validation",
        stop_router_->ValidateEmptyForMovieCoreStop());
}

BackendResult EmulationSession::SettleStopPointsAfterMovieCoreStop()
{
    if (!stop_router_ || !workset_epoch_)
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidState,
            "Stopped movie core boundary requires an active workset router",
            BackendIntegrity::Unknown);
    }
    return FromStopPointLifecycle(
        "stopped movie-core ingress boundary",
        stop_router_->EnterStoppedMovieCoreBoundary());
}

BackendResult EmulationSession::ValidateStopPointsAfterMovieCoreStart()
{
    if (!stop_router_ || !workset_epoch_)
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidState,
            "Movie core start validation requires an active workset router",
            BackendIntegrity::Unknown);
    }
    return FromStopPointLifecycle(
        "post-start movie stop-point validation",
        stop_router_->RevalidateAfterJit());
}

BackendResult EmulationSession::CleanupStopPoints()
{
    if (!stop_router_)
        return BackendResult::Success();
    return FromStopPointLifecycle(
        "stop-point cleanup",
        stop_router_->StopIngressDrainAndCleanup());
}

BackendResult EmulationSession::TaintAndRetireSessionAfterStopPointFailure(
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
    BackendResult execution = RemoveExecutionControlCore();
    if (!execution.ok)
        result = std::move(execution);

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
    stop_cpu_observers_.reset();
    physical_stop_manager_.reset();
    stop_cpu_evaluator_.reset();

    savestate_service_.reset();
    savestate_backend_adapter_.reset();
    return result;
}

BackendResult EmulationSession::FromSavestateService(
    const SavestateServiceResult& result)
{
    if (result.ok)
        return BackendResult::Success();
    BackendErrorCode code = BackendErrorCode::OperationFailed;
    switch (result.code)
    {
    case SavestateServiceErrorCode::InvalidArgument:
    case SavestateServiceErrorCode::CompatibilityMismatch:
    case SavestateServiceErrorCode::ArtifactFailure:
        code = BackendErrorCode::InvalidArgument;
        break;
    case SavestateServiceErrorCode::InvalidState:
    case SavestateServiceErrorCode::WorksetMismatch:
    case SavestateServiceErrorCode::NotFound:
        code = BackendErrorCode::InvalidState;
        break;
    case SavestateServiceErrorCode::Unsupported:
        code = BackendErrorCode::Unavailable;
        break;
    case SavestateServiceErrorCode::None:
    case SavestateServiceErrorCode::CapacityExceeded:
    case SavestateServiceErrorCode::BackendFailure:
    case SavestateServiceErrorCode::IntegrityFailure:
        code = BackendErrorCode::OperationFailed;
        break;
    }
    return BackendResult::Failure(
        code,
        result.message,
        result.integrity == GuestIntegrity::Unknown
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
    case MovieServiceErrorCode::SavestateFailure:
    case MovieServiceErrorCode::BackendFailure:
    case MovieServiceErrorCode::IntegrityFailure:
        break;
    }
    return BackendResult::Failure(
        code,
        result.message,
        result.integrity == GuestIntegrity::Unknown
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
