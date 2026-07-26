#include "EmulationSession.h"

#include <exception>
#include <limits>
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
    return Complete(SessionOperation::Open, origin, std::move(result), true);
}

SessionOperationReceipt EmulationSession::Reboot()
{
    const StateEpoch origin = state_epoch_;
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

    BackendResult result = CallBackend(
        "Dolphin backend reboot",
        [&] { return backend_->Reboot(); });
    return Complete(SessionOperation::Reboot, origin, std::move(result), true);
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
    return Complete(
        SessionOperation::Resume,
        origin,
        CallBackend(
            "Dolphin backend resume",
            [&] { return backend_->Resume(); }),
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
    return Complete(
        SessionOperation::StepInstruction,
        origin,
        CallBackend(
            "Dolphin backend instruction step",
            [&] { return backend_->StepInstruction(timeout); }),
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
    return Complete(
        SessionOperation::StepFrame,
        origin,
        CallBackend(
            "Dolphin backend frame step",
            [&] { return backend_->StepFrame(timeout); }),
        false);
}

SessionOperationReceipt EmulationSession::RestoreStateFile(
    const std::filesystem::path& path)
{
    const StateEpoch origin = state_epoch_;
    if (!BindOrCheckOwner() || !CanOperate())
    {
        return Reject(
            SessionOperation::RestoreStateFile,
            BackendErrorCode::InvalidState,
            "EmulationSession cannot restore state in its current state");
    }
    return Complete(
        SessionOperation::RestoreStateFile,
        origin,
        CallBackend(
            "Dolphin backend file-state restore",
            [&] { return backend_->RestoreStateFile(path); }),
        true);
}

SessionOperationReceipt EmulationSession::RestoreStateBuffer(
    const std::vector<std::uint8_t>& bytes)
{
    const StateEpoch origin = state_epoch_;
    if (!BindOrCheckOwner() || !CanOperate())
    {
        return Reject(
            SessionOperation::RestoreStateBuffer,
            BackendErrorCode::InvalidState,
            "EmulationSession cannot restore state in its current state");
    }
    return Complete(
        SessionOperation::RestoreStateBuffer,
        origin,
        CallBackend(
            "Dolphin backend buffer-state restore",
            [&] { return backend_->RestoreStateBuffer(bytes); }),
        true);
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
    BackendResult result;
    try
    {
        result = backend_ ? backend_->Close() : BackendResult::Success();
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
