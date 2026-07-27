#include "EmulationSession.h"

#include <exception>
#include <limits>
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

template <typename Operation>
BackendBufferResult CallBackendBuffer(
    const char* operation_name,
    Operation&& operation) noexcept
{
    try
    {
        return operation();
    }
    catch (const std::exception& ex)
    {
        return {
            BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                std::string(operation_name) + " threw: " + ex.what(),
                BackendIntegrity::Unknown),
            {}};
    }
    catch (...)
    {
        return {
            BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                std::string(operation_name) + " threw",
                BackendIntegrity::Unknown),
            {}};
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

[[nodiscard]] std::optional<StateEpoch> NextEpoch(StateEpoch current) noexcept
{
    if (current.value() == std::numeric_limits<std::uint64_t>::max())
        return std::nullopt;
    return StateEpoch(current.value() + 1);
}

} // namespace

EmulationSession::EmulationSession(
    SessionId session_id,
    std::unique_ptr<IDolphinBackend> backend)
    : session_id_(session_id),
      backend_(std::move(backend))
{
}

EmulationSession::~EmulationSession()
{
    if (!backend_)
        return;

    try
    {
        if (!owner_bound_ || owner_thread_ == std::this_thread::get_id())
            (void)CleanupStopPoints();
        stop_router_.reset();
        physical_stop_manager_.reset();
        (void)backend_->Close();
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

    BackendResult result = CallBackend(
        "Dolphin backend open",
        [&] { return backend_->Open(options.backend); });
    if (result.ok)
    {
        const auto first_epoch = NextEpoch(state_epoch_);
        if (!first_epoch)
        {
            result = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "StateEpoch exhausted before stop-point initialization",
                BackendIntegrity::Unknown);
        }
        else
        {
            result = InitializeStopPoints(*first_epoch);
            if (result.ok)
            {
                RefreshCoreState();
                if (!HasReusableCoreState())
                {
                    result = BackendResult::Failure(
                        BackendErrorCode::OperationFailed,
                        "Dolphin boot completed without a reusable core state",
                        BackendIntegrity::Unknown);
                    const BackendResult cleanup = CleanupStopPoints();
                    stop_router_.reset();
                    physical_stop_manager_.reset();
                    if (!cleanup.ok)
                    {
                        result.integrity = BackendIntegrity::Unknown;
                        result.message += "; ";
                        result.message += cleanup.message.empty()
                            ? "stop-point cleanup could not be proven"
                            : cleanup.message;
                    }
                }
            }
        }
        if (!result.ok)
        {
            const BackendResult close = CallBackend(
                "Dolphin backend open rollback",
                [&] { return backend_->Close(); });
            if (!close.ok || close.integrity == BackendIntegrity::Unknown)
                result.integrity = BackendIntegrity::Unknown;
        }
    }
    return Complete(SessionOperation::Open, origin, std::move(result), true);
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

    return PerformStateReplacement(
        SessionOperation::Reboot,
        [&] {
            return CallBackend(
                "Dolphin backend reboot",
                [&] { return backend_->Reboot(); });
        });
}

SessionOperationReceipt EmulationSession::Pause(std::chrono::milliseconds timeout)
{
    const StateEpoch origin = state_epoch_;
    if (!BindOrCheckOwner() || !CanOperate())
    {
        return Reject(
            SessionOperation::Pause,
            BackendErrorCode::InvalidState,
            "EmulationSession cannot pause in its current state");
    }
    return Complete(
        SessionOperation::Pause,
        origin,
        CallBackend(
            "Dolphin backend pause",
            [&] { return backend_->Pause(timeout); }),
        false);
}

SessionOperationReceipt EmulationSession::Resume()
{
    const StateEpoch origin = state_epoch_;
    if (!BindOrCheckOwner() || !CanOperate())
    {
        return Reject(
            SessionOperation::Resume,
            BackendErrorCode::InvalidState,
            "EmulationSession cannot resume in its current state");
    }
    BackendResult result = CallBackend(
        "Dolphin backend resume",
        [&] { return backend_->Resume(); });
    if (result.ok && stop_router_)
    {
        if (StopPointError error = stop_router_->DepartCurrentPoint())
        {
            result = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "failed departing retained stop point after resume: " +
                    error.message,
                BackendIntegrity::Unknown);
        }
    }
    return Complete(
        SessionOperation::Resume,
        origin,
        std::move(result),
        false);
}

SessionOperationReceipt EmulationSession::StepInstruction(std::chrono::milliseconds timeout)
{
    const StateEpoch origin = state_epoch_;
    if (!BindOrCheckOwner() || !CanOperate())
    {
        return Reject(
            SessionOperation::StepInstruction,
            BackendErrorCode::InvalidState,
            "EmulationSession cannot step in its current state");
    }
    BackendResult result = CallBackend(
        "Dolphin backend instruction step",
        [&] { return backend_->StepInstruction(timeout); });
    if (result.ok && stop_router_)
    {
        if (StopPointError error = stop_router_->DepartCurrentPoint())
        {
            result = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "failed departing retained stop point after instruction step: " +
                    error.message,
                BackendIntegrity::Unknown);
        }
    }
    return Complete(
        SessionOperation::StepInstruction,
        origin,
        std::move(result),
        false);
}

SessionOperationReceipt EmulationSession::StepFrame(std::chrono::milliseconds timeout)
{
    const StateEpoch origin = state_epoch_;
    if (!BindOrCheckOwner() || !CanOperate())
    {
        return Reject(
            SessionOperation::StepFrame,
            BackendErrorCode::InvalidState,
            "EmulationSession cannot step in its current state");
    }
    BackendResult result = CallBackend(
        "Dolphin backend frame step",
        [&] { return backend_->StepFrame(timeout); });
    if (result.ok && stop_router_)
    {
        if (StopPointError error = stop_router_->DepartCurrentPoint())
        {
            result = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "failed departing retained stop point after frame step: " +
                    error.message,
                BackendIntegrity::Unknown);
        }
    }
    return Complete(
        SessionOperation::StepFrame,
        origin,
        std::move(result),
        false);
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
    return PerformStateReplacement(
        SessionOperation::RestoreStateFile,
        [&] {
            return CallBackend(
                "Dolphin backend file-state restore",
                [&] { return backend_->RestoreStateFile(path); });
        });
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
    return PerformStateReplacement(
        SessionOperation::RestoreStateBuffer,
        [&] {
            return CallBackend(
                "Dolphin backend buffer-state restore",
                [&] { return backend_->RestoreStateBuffer(bytes); });
        });
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
    return Complete(
        SessionOperation::SaveStateFile,
        origin,
        CallBackend(
            "Dolphin backend file-state save",
            [&] { return backend_->SaveStateFile(path); }),
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

    BackendBufferResult result = CallBackendBuffer(
        "Dolphin backend buffer-state save",
        [&] { return backend_->SaveStateBuffer(); });
    SessionOperationReceipt receipt = Complete(
        SessionOperation::SaveStateBuffer,
        origin,
        std::move(result.result),
        false);
    return {std::move(receipt), std::move(result.bytes)};
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
    return Complete(
        SessionOperation::Screenshot,
        origin,
        CallBackend(
            "Dolphin backend screenshot",
            [&] { return backend_->CaptureScreenshot(path, timeout); }),
        false);
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
    return stop_router_->DrainIngress();
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
    BackendResult result = CleanupStopPoints();
    stop_router_.reset();
    physical_stop_manager_.reset();
    try
    {
        BackendResult close =
            backend_ ? backend_->Close() : BackendResult::Success();
        if (!close.ok)
            result = std::move(close);
    }
    catch (const std::exception& ex)
    {
        result = BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            std::string("Dolphin backend shutdown threw: ") + ex.what(),
            BackendIntegrity::Unknown);
    }
    catch (...)
    {
        result = BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "Dolphin backend shutdown threw",
            BackendIntegrity::Unknown);
    }
    if (!result.ok)
        ApplyBackendFailure(result);

    backend_.reset();
    opened_ = false;
    core_state_ = BackendCoreState::Closed;
    if (disposition_ != SessionDisposition::Tainted)
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

SessionOperationReceipt EmulationSession::PerformStateReplacement(
    SessionOperation operation,
    const std::function<BackendResult()>& replace)
{
    const StateEpoch origin = state_epoch_;
    BackendResult preparation = PrepareStopPointStateReplacement();
    if (!preparation.ok)
    {
        if (preparation.integrity == BackendIntegrity::Unknown)
        {
            preparation =
                TaintAndCloseAfterStopPointFailure(std::move(preparation));
        }
        else
        {
            ApplyBackendFailure(preparation);
        }
        return {
            operation,
            false,
            origin,
            state_epoch_,
            disposition_,
            std::move(preparation)};
    }

    BackendResult result = replace();
    if (!result.ok)
    {
        if (result.integrity == BackendIntegrity::Preserved)
        {
            BackendResult rollback = RollbackStopPointStateReplacement();
            if (!rollback.ok)
            {
                result.message += result.message.empty() ? "" : "; ";
                result.message += rollback.message;
                result.integrity = BackendIntegrity::Unknown;
            }
        }
        else
        {
            (void)CleanupStopPoints();
        }
        if (result.integrity == BackendIntegrity::Unknown)
        {
            result = TaintAndCloseAfterStopPointFailure(std::move(result));
            return {
                operation,
                false,
                origin,
                state_epoch_,
                disposition_,
                std::move(result)};
        }
        return Complete(operation, origin, std::move(result), false);
    }

    const auto next_epoch = NextEpoch(state_epoch_);
    if (!next_epoch)
    {
        result = BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "StateEpoch exhausted after Dolphin state replacement",
            BackendIntegrity::Unknown);
        MarkTainted(result.message);
        (void)CleanupStopPoints();
        (void)CallBackend(
            "Dolphin backend shutdown after epoch exhaustion",
            [&] { return backend_->Close(); });
        opened_ = false;
        core_state_ = BackendCoreState::Closed;
        return {
            operation,
            false,
            origin,
            state_epoch_,
            disposition_,
            std::move(result)};
    }

    state_epoch_ = *next_epoch;
    BackendResult committed = CommitStopPointStateReplacement(state_epoch_);
    if (!committed.ok)
    {
        committed.integrity = BackendIntegrity::Unknown;
        MarkTainted(committed.message.empty()
            ? "stop-point reconciliation failed after state replacement"
            : committed.message);
        (void)CleanupStopPoints();
        (void)CallBackend(
            "Dolphin backend shutdown after stop-point reconciliation failure",
            [&] { return backend_->Close(); });
        opened_ = false;
        core_state_ = BackendCoreState::Closed;
        return {
            operation,
            false,
            origin,
            state_epoch_,
            disposition_,
            std::move(committed)};
    }

    return Complete(operation, origin, std::move(result), false);
}

SessionOperationReceipt EmulationSession::Complete(
    SessionOperation operation,
    StateEpoch origin,
    BackendResult result,
    bool advances_epoch)
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

    if (advances_epoch && !AdvanceEpoch())
    {
        result = BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "StateEpoch exhausted",
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
            std::make_unique<StopPointRouter>(*physical_stop_manager_);
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
    return BackendResult::Success();
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

    const BackendResult cleanup = CleanupStopPoints();
    if (!cleanup.ok)
    {
        failure.message += "; ";
        failure.message += cleanup.message.empty()
            ? "stop-point cleanup could not be proven"
            : cleanup.message;
    }

    const BackendResult close = backend_
        ? CallBackend(
              "Dolphin backend shutdown after stop-point failure",
              [&] { return backend_->Close(); })
        : BackendResult::Success();
    if (!close.ok)
    {
        failure.message += "; ";
        failure.message += close.message.empty()
            ? "Dolphin backend shutdown could not be proven"
            : close.message;
    }
    opened_ = false;
    core_state_ = BackendCoreState::Closed;
    return failure;
}

bool EmulationSession::AdvanceEpoch() noexcept
{
    const std::uint64_t current = state_epoch_.value();
    if (current == std::numeric_limits<std::uint64_t>::max())
        return false;
    state_epoch_ = StateEpoch(current + 1);
    return true;
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
