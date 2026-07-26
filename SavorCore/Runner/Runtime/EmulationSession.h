#pragma once

#include "IDolphinBackend.h"
#include "RuntimeTypes.h"

#include <chrono>
#include <filesystem>
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
    SessionOperationReceipt CheckHealth();
    SessionOperationReceipt Shutdown();

    void MarkTainted(std::string diagnostic);
    void MarkCleanWithDiagnostics(std::string diagnostic);

    [[nodiscard]] const std::string& taint_diagnostic() const noexcept
    {
        return taint_diagnostic_;
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
    [[nodiscard]] bool AdvanceEpoch() noexcept;
    void ApplyBackendFailure(const BackendResult& result);
    void RefreshCoreState() noexcept;

    SessionId session_id_;
    std::unique_ptr<IDolphinBackend> backend_;
    SessionDisposition disposition_ = SessionDisposition::Closed;
    StateEpoch state_epoch_;
    BackendCoreState core_state_ = BackendCoreState::Closed;
    std::thread::id owner_thread_;
    bool owner_bound_ = false;
    bool opened_ = false;
    bool shutdown_ = false;
    std::optional<SessionOperationReceipt> shutdown_receipt_;
    std::string taint_diagnostic_;
};

} // namespace savor::runtime
