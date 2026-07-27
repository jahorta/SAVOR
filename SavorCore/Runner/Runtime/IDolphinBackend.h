#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace savor::runtime {

class IPhysicalStopPointBackendPort;
class IExecutionBackendPort;

enum class BackendErrorCode : std::uint16_t
{
    None,
    InvalidArgument,
    InvalidState,
    BootFailed,
    GameLoadFailed,
    OperationFailed,
    Timeout,
    Unavailable,
};

enum class BackendIntegrity : std::uint8_t
{
    Preserved,
    Unknown,
};

enum class BackendCoreState : std::uint8_t
{
    Closed,
    Running,
    Paused,
    Stopped,
    Unknown,
};

struct BackendResult
{
    bool ok = false;
    BackendErrorCode code = BackendErrorCode::OperationFailed;
    BackendIntegrity integrity = BackendIntegrity::Preserved;
    std::string message;

    [[nodiscard]] static BackendResult Success()
    {
        return {true, BackendErrorCode::None, BackendIntegrity::Preserved, {}};
    }

    [[nodiscard]] static BackendResult Failure(
        BackendErrorCode code,
        std::string message,
        BackendIntegrity integrity = BackendIntegrity::Preserved)
    {
        return {false, code, integrity, std::move(message)};
    }
};

struct BackendOpenOptions
{
    std::filesystem::path runtime_root;
    std::filesystem::path user_directory;
    std::filesystem::path dolphin_base_directory;
    std::filesystem::path iso_path;
    bool force_resync_from_base = true;
    bool visual = false;
    std::uintptr_t render_window_handle = 0;
};

struct BackendBufferResult
{
    BackendResult result;
    std::vector<std::uint8_t> bytes;
};

struct BackendHealthReport
{
    bool healthy = false;
    BackendCoreState core_state = BackendCoreState::Unknown;
    std::string diagnostic;
};

class IDolphinBackend
{
public:
    virtual ~IDolphinBackend() = default;

    IDolphinBackend(const IDolphinBackend&) = delete;
    IDolphinBackend& operator=(const IDolphinBackend&) = delete;

    virtual BackendResult Open(const BackendOpenOptions& options) = 0;
    virtual BackendResult Reboot() = 0;
    virtual BackendResult Close() = 0;

    [[nodiscard]] virtual BackendCoreState QueryCoreState() const noexcept = 0;
    [[nodiscard]] virtual BackendHealthReport CheckHealth() const = 0;

    virtual BackendResult RestoreStateFile(const std::filesystem::path& path) = 0;
    virtual BackendResult SaveStateFile(const std::filesystem::path& path) = 0;
    virtual BackendBufferResult SaveStateBuffer() = 0;
    virtual BackendResult RestoreStateBuffer(const std::vector<std::uint8_t>& bytes) = 0;

    virtual BackendResult CaptureScreenshot(
        const std::filesystem::path& path,
        std::chrono::milliseconds timeout) = 0;

    // The session gives this facet only to its PhysicalStopPointManager. Other
    // runtime layers never receive Dolphin's physical debugging surface.
    [[nodiscard]] virtual IPhysicalStopPointBackendPort*
    PhysicalStopPoints() noexcept = 0;
    // The session gives this facet only to its ExecutionEngine. Session
    // callers and program layers never receive primitive advancement access.
    [[nodiscard]] virtual IExecutionBackendPort* Execution() noexcept = 0;

protected:
    IDolphinBackend() = default;
};

} // namespace savor::runtime
