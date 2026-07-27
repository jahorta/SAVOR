#pragma once

#include "IDolphinBackend.h"
#include "RuntimeTypes.h"
#include "StopPoints/StopPointRouter.h"

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
    Reboot,
    Pause,
    Resume,
    StepInstruction,
    StepFrame,
    RestoreStateFile,
    RestoreStateBuffer,
    SaveStateFile,
    SaveStateBuffer,
    Screenshot,
    JitRevalidation,
    BreakpointReconciliation,
    HealthCheck,
    Shutdown,
};

struct SessionOpenOptions
{
    BackendOpenOptions backend;
    std::filesystem::path screenshot_directory;
    std::chrono::milliseconds screenshot_timeout{3000};
    bool screenshot_on_terminal = false;
};

struct SessionSnapshot
{
    SessionId session_id;
    SessionDisposition disposition = SessionDisposition::Closed;
    StateEpoch state_epoch;
    BackendCoreState core_state = BackendCoreState::Closed;
    bool open = false;
};

struct SessionOperationReceipt
{
    SessionOperation operation = SessionOperation::HealthCheck;
    bool ok = false;
    StateEpoch origin_epoch;
    StateEpoch resulting_epoch;
    SessionDisposition disposition = SessionDisposition::Closed;
    BackendResult backend;
};

struct SessionBufferReceipt
{
    SessionOperationReceipt operation;
    std::vector<std::uint8_t> bytes;
};

class EmulationSession final
{
public:
    EmulationSession(SessionId session_id, std::unique_ptr<IDolphinBackend> backend);
    ~EmulationSession();

    EmulationSession(const EmulationSession&) = delete;
    EmulationSession& operator=(const EmulationSession&) = delete;

    [[nodiscard]] SessionSnapshot snapshot() const noexcept;
    [[nodiscard]] bool ConfigureStopPointIngressNotification(
        std::atomic<std::uint64_t>* counter,
        void* notifier_context,
        StopPointIngressNotifier notifier) noexcept;

    SessionOperationReceipt Open(const SessionOpenOptions& options);
    SessionOperationReceipt Reboot();

    SessionOperationReceipt Pause(std::chrono::milliseconds timeout);
    SessionOperationReceipt Resume();
    SessionOperationReceipt StepInstruction(std::chrono::milliseconds timeout);
    SessionOperationReceipt StepFrame(std::chrono::milliseconds timeout);

    SessionOperationReceipt RestoreStateFile(const std::filesystem::path& path);
    SessionOperationReceipt RestoreStateBuffer(const std::vector<std::uint8_t>& bytes);
    SessionOperationReceipt SaveStateFile(const std::filesystem::path& path);
    SessionBufferReceipt SaveStateBuffer();

    SessionOperationReceipt CaptureScreenshot(
        const std::filesystem::path& path,
        std::chrono::milliseconds timeout);
    SessionOperationReceipt RevalidateStopPointsAfterJit();
    SessionOperationReceipt ValidateBreakpointChangeNotification();
    [[nodiscard]] std::vector<StopRouteReceipt> DrainStopPointEvents();
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

private:
    [[nodiscard]] bool BindOrCheckOwner() noexcept;
    [[nodiscard]] bool CanOperate() const noexcept;
    [[nodiscard]] bool HasReusableCoreState() const noexcept;
    [[nodiscard]] SessionOperationReceipt Reject(
        SessionOperation operation,
        BackendErrorCode code,
        std::string message) const;
    [[nodiscard]] SessionOperationReceipt Complete(
        SessionOperation operation,
        StateEpoch origin,
        BackendResult result,
        bool advances_epoch);
    [[nodiscard]] SessionOperationReceipt PerformStateReplacement(
        SessionOperation operation,
        const std::function<BackendResult()>& replace);
    [[nodiscard]] bool AdvanceEpoch() noexcept;
    void ApplyBackendFailure(const BackendResult& result);
    void RefreshCoreState() noexcept;
    [[nodiscard]] BackendResult InitializeStopPoints(StateEpoch first_epoch);
    [[nodiscard]] BackendResult PrepareStopPointStateReplacement();
    [[nodiscard]] BackendResult CommitStopPointStateReplacement(
        StateEpoch new_epoch);
    [[nodiscard]] BackendResult RollbackStopPointStateReplacement();
    [[nodiscard]] BackendResult CleanupStopPoints();
    [[nodiscard]] BackendResult TaintAndCloseAfterStopPointFailure(
        BackendResult failure);

    SessionId session_id_;
    std::unique_ptr<IDolphinBackend> backend_;
    std::unique_ptr<PhysicalStopPointManager> physical_stop_manager_;
    std::unique_ptr<StopPointRouter> stop_router_;
    SessionDisposition disposition_ = SessionDisposition::Closed;
    StateEpoch state_epoch_;
    BackendCoreState core_state_ = BackendCoreState::Closed;
    std::thread::id owner_thread_;
    bool owner_bound_ = false;
    bool opened_ = false;
    bool shutdown_ = false;
    std::optional<SessionOperationReceipt> shutdown_receipt_;
    std::string taint_diagnostic_;
    std::atomic<std::uint64_t>* stop_ingress_notification_counter_ = nullptr;
    void* stop_ingress_notifier_context_ = nullptr;
    StopPointIngressNotifier stop_ingress_notifier_ = nullptr;
};

} // namespace savor::runtime
