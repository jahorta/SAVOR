#include "DolphinWrapperBackend.h"

#include "../../Boot/Boot.h"
#include "../../Core/DolphinWrapper.h"

#include "Common/Buffer.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <utility>

namespace savor::runtime {
namespace {

[[nodiscard]] int ClampTimeout(std::chrono::milliseconds timeout) noexcept
{
    const auto count = timeout.count();
    if (count <= 0)
        return 1;
    return static_cast<int>(std::min<std::int64_t>(
        count,
        std::numeric_limits<int>::max()));
}

[[nodiscard]] std::uint32_t ClampUnsignedTimeout(std::chrono::milliseconds timeout) noexcept
{
    const auto count = timeout.count();
    if (count <= 0)
        return 1;
    return static_cast<std::uint32_t>(std::min<std::int64_t>(
        count,
        std::numeric_limits<std::uint32_t>::max()));
}

[[nodiscard]] bool IsRegularFile(const std::filesystem::path& path) noexcept
{
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

} // namespace

struct DolphinWrapperBackend::Impl
{
    std::unique_ptr<DolphinWrapper> wrapper;
    BackendOpenOptions last_open_options;
    bool has_open_options = false;
    bool open = false;

    [[nodiscard]] BackendResult RequireOpen() const
    {
        if (!open || !wrapper)
        {
            return BackendResult::Failure(
                BackendErrorCode::InvalidState,
                "Dolphin backend is not open");
        }
        return BackendResult::Success();
    }
};

DolphinWrapperBackend::DolphinWrapperBackend()
    : impl_(std::make_unique<Impl>())
{
}

DolphinWrapperBackend::~DolphinWrapperBackend() = default;

BackendResult DolphinWrapperBackend::Open(const BackendOpenOptions& options)
{
    if (impl_->open)
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidState,
            "Dolphin backend is already open");
    }
    if (options.user_directory.empty())
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidArgument,
            "Dolphin user directory is required");
    }
    if (options.dolphin_base_directory.empty())
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidArgument,
            "Dolphin base directory is required");
    }
    if (options.iso_path.empty() || !IsRegularFile(options.iso_path))
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidArgument,
            "A readable game ISO path is required");
    }

    auto wrapper = std::make_unique<DolphinWrapper>();

    simboot::BootOptions boot_options;
    boot_options.user_dir = options.user_directory;
    boot_options.dolphin_qt_base = options.dolphin_base_directory;
    boot_options.force_resync_from_base = options.force_resync_from_base;
    boot_options.visual = options.visual;
    boot_options.render_widget_handle =
        reinterpret_cast<void*>(options.render_window_handle);
    boot_options.save_config_on_success = false;

    std::string error;
    if (!simboot::BootDolphinWrapper(*wrapper, boot_options, &error))
    {
        return BackendResult::Failure(
            BackendErrorCode::BootFailed,
            error.empty() ? "Dolphin boot failed" : std::move(error));
    }

    if (!wrapper->loadGame(options.iso_path.string()))
    {
        wrapper.reset();
        return BackendResult::Failure(
            BackendErrorCode::GameLoadFailed,
            "Dolphin failed to load the requested game",
            BackendIntegrity::Unknown);
    }

    wrapper->ConfigurePortsStandardPadP1();

    impl_->wrapper = std::move(wrapper);
    impl_->last_open_options = options;
    impl_->has_open_options = true;
    impl_->open = true;
    return BackendResult::Success();
}

BackendResult DolphinWrapperBackend::Reboot()
{
    if (!impl_->has_open_options)
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidState,
            "Dolphin backend has no prior open configuration");
    }

    const BackendOpenOptions options = impl_->last_open_options;
    const BackendResult close_result = Close();
    if (!close_result.ok)
        return close_result;

    BackendResult result = Open(options);
    if (!result.ok)
        result.integrity = BackendIntegrity::Unknown;
    return result;
}

BackendResult DolphinWrapperBackend::Close()
{
    if (!impl_->wrapper)
    {
        impl_->open = false;
        return BackendResult::Success();
    }

    try
    {
        impl_->wrapper.reset();
        impl_->open = false;
        return BackendResult::Success();
    }
    catch (const std::exception& ex)
    {
        impl_->open = false;
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            std::string("Dolphin shutdown failed: ") + ex.what(),
            BackendIntegrity::Unknown);
    }
    catch (...)
    {
        impl_->open = false;
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "Dolphin shutdown failed",
            BackendIntegrity::Unknown);
    }
}

BackendCoreState DolphinWrapperBackend::QueryCoreState() const noexcept
{
    if (!impl_->open || !impl_->wrapper)
        return BackendCoreState::Closed;
    if (impl_->wrapper->isEmulationPaused())
        return BackendCoreState::Paused;
    if (impl_->wrapper->isRunning())
        return BackendCoreState::Running;
    return BackendCoreState::Stopped;
}

BackendHealthReport DolphinWrapperBackend::CheckHealth() const
{
    const BackendCoreState state = QueryCoreState();
    if (state == BackendCoreState::Closed)
        return {false, state, "Dolphin backend is closed"};
    if (state == BackendCoreState::Unknown)
        return {false, state, "Dolphin core state is unknown"};
    if (state == BackendCoreState::Stopped)
        return {false, state, "Dolphin core is stopped"};
    return {true, state, {}};
}

BackendResult DolphinWrapperBackend::Pause(std::chrono::milliseconds timeout)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (impl_->wrapper->pauseEmulationBlocking(ClampUnsignedTimeout(timeout)))
        return BackendResult::Success();
    return BackendResult::Failure(
        BackendErrorCode::Timeout,
        "Timed out while pausing Dolphin");
}

BackendResult DolphinWrapperBackend::Resume()
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (impl_->wrapper->resumeEmulation())
        return BackendResult::Success();
    return BackendResult::Failure(
        BackendErrorCode::OperationFailed,
        "Dolphin failed to resume emulation");
}

BackendResult DolphinWrapperBackend::StepInstruction(std::chrono::milliseconds timeout)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (impl_->wrapper->stepOneOpcodeBlocking(ClampTimeout(timeout)))
        return BackendResult::Success();
    return BackendResult::Failure(
        BackendErrorCode::Timeout,
        "Timed out while stepping one guest instruction",
        BackendIntegrity::Unknown);
}

BackendResult DolphinWrapperBackend::StepFrame(std::chrono::milliseconds timeout)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (impl_->wrapper->stepOneFrameBlocking(ClampTimeout(timeout)))
        return BackendResult::Success();
    return BackendResult::Failure(
        BackendErrorCode::Timeout,
        "Timed out while stepping one guest frame",
        BackendIntegrity::Unknown);
}

BackendResult DolphinWrapperBackend::RestoreStateFile(const std::filesystem::path& path)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (path.empty() || !IsRegularFile(path))
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidArgument,
            "A readable savestate path is required");
    }
    if (impl_->wrapper->loadSavestate(path.string()))
        return BackendResult::Success();
    return BackendResult::Failure(
        BackendErrorCode::OperationFailed,
        "Dolphin failed to restore the savestate",
        BackendIntegrity::Unknown);
}

BackendResult DolphinWrapperBackend::SaveStateFile(const std::filesystem::path& path)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (path.empty())
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidArgument,
            "A savestate output path is required");
    }
    if (impl_->wrapper->saveSavestateBlocking(path.string()))
        return BackendResult::Success();
    return BackendResult::Failure(
        BackendErrorCode::OperationFailed,
        "Dolphin failed to save the savestate");
}

BackendBufferResult DolphinWrapperBackend::SaveStateBuffer()
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return {std::move(open), {}};

    Common::UniqueBuffer<u8> buffer;
    if (!impl_->wrapper->saveStateToBuffer(buffer))
    {
        return {
            BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "Dolphin failed to save state to a buffer"),
            {}};
    }

    std::vector<std::uint8_t> bytes(buffer.size());
    if (!bytes.empty())
        std::memcpy(bytes.data(), buffer.data(), bytes.size());
    return {BackendResult::Success(), std::move(bytes)};
}

BackendResult DolphinWrapperBackend::RestoreStateBuffer(
    const std::vector<std::uint8_t>& bytes)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (bytes.empty())
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidArgument,
            "A non-empty state buffer is required");
    }

    Common::UniqueBuffer<u8> buffer(bytes.size());
    std::memcpy(buffer.data(), bytes.data(), bytes.size());
    if (impl_->wrapper->loadStateFromBuffer(buffer))
        return BackendResult::Success();
    return BackendResult::Failure(
        BackendErrorCode::OperationFailed,
        "Dolphin failed to restore state from a buffer",
        BackendIntegrity::Unknown);
}

BackendResult DolphinWrapperBackend::CaptureScreenshot(
    const std::filesystem::path& path,
    std::chrono::milliseconds timeout)
{
    if (BackendResult open = impl_->RequireOpen(); !open.ok)
        return open;
    if (path.empty())
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidArgument,
            "A screenshot output path is required");
    }
    if (impl_->wrapper->saveScreenshotBlocking(
            path.string(),
            ClampUnsignedTimeout(timeout)))
    {
        return BackendResult::Success();
    }
    return BackendResult::Failure(
        BackendErrorCode::Timeout,
        "Dolphin failed to capture a screenshot before the deadline");
}

std::unique_ptr<IDolphinBackend> MakeDolphinWrapperBackend()
{
    return std::make_unique<DolphinWrapperBackend>();
}

} // namespace savor::runtime
