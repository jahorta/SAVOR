#pragma once

#include "Services/Savestate/SavestateTypes.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace savor::runtime {

class IPhysicalStopPointBackendPort;
class IExecutionBackendPort;
class IInputBackendPort;
class IGuestMemoryBackendPort;
class IHitTimeGuestMemoryBackendPort;
class IScreenshotBackendPort;
class IMovieBackendPort;
class ICaptureBackendPort;

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
    virtual BackendResult Close() = 0;

    [[nodiscard]] virtual BackendCoreState QueryCoreState() const noexcept = 0;
    [[nodiscard]] virtual BackendHealthReport CheckHealth() const = 0;
    [[nodiscard]] virtual ArtifactCompatibilityToken
    SavestateCompatibility() const = 0;

    virtual BackendResult RestoreStateFile(const std::filesystem::path& path) = 0;
    virtual BackendResult SaveStateFile(const std::filesystem::path& path) = 0;
    // Serialize through Dolphin's native file writer, wait for the file to be
    // complete, then read those exact file bytes back for transactional
    // artifact ownership. This is deliberately distinct from SaveStateBuffer,
    // whose raw bytes are only valid for process-local memory-handle restores.
    virtual BackendBufferResult SaveStateFileBytes() = 0;
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
    // The session gives these facets only to their corresponding Slice 4
    // services. Program and worker layers never receive them directly.
    [[nodiscard]] virtual IInputBackendPort* Input() noexcept = 0;
    [[nodiscard]] virtual IGuestMemoryBackendPort* GuestMemory() noexcept = 0;
    [[nodiscard]] virtual IHitTimeGuestMemoryBackendPort*
    HitTimeGuestMemory() noexcept = 0;
    [[nodiscard]] virtual IScreenshotBackendPort* Screenshots() noexcept = 0;
    [[nodiscard]] virtual IMovieBackendPort* Movies() noexcept = 0;
    [[nodiscard]] virtual ICaptureBackendPort* Captures() noexcept = 0;

protected:
    IDolphinBackend() = default;
};

} // namespace savor::runtime
