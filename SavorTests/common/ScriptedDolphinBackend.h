#pragma once

#include "Runner/Runtime/IDolphinBackend.h"
#include "Runner/Runtime/StopPoints/IPhysicalStopPointBackendPort.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace savor::test_support {

struct ScriptedDolphinBackendControl
{
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::optional<std::thread::id> owner_thread;
    bool owner_thread_violation = false;
    std::vector<std::string> calls;
    std::vector<std::filesystem::path> screenshots;

    runtime::BackendResult open_result = runtime::BackendResult::Success();
    runtime::BackendResult reboot_result = runtime::BackendResult::Success();
    runtime::BackendResult close_result = runtime::BackendResult::Success();
    runtime::BackendResult pause_result = runtime::BackendResult::Success();
    runtime::BackendResult resume_result = runtime::BackendResult::Success();
    runtime::BackendResult step_instruction_result = runtime::BackendResult::Success();
    runtime::BackendResult step_frame_result = runtime::BackendResult::Success();
    runtime::BackendResult restore_file_result = runtime::BackendResult::Success();
    runtime::BackendResult restore_buffer_result = runtime::BackendResult::Success();
    runtime::BackendResult save_file_result = runtime::BackendResult::Success();
    runtime::BackendResult save_buffer_result = runtime::BackendResult::Success();
    runtime::BackendResult screenshot_result = runtime::BackendResult::Success();
    runtime::BackendCoreState core_state = runtime::BackendCoreState::Closed;
    runtime::BackendCoreState open_core_state = runtime::BackendCoreState::Running;
    std::vector<std::uint8_t> save_buffer_bytes{0x10, 0x20, 0x30};

    int open_count = 0;
    int reboot_count = 0;
    int close_count = 0;
    int screenshot_count = 0;
    int restore_file_count = 0;
    int restore_buffer_count = 0;
    int destruction_count = 0;
    std::function<void()> destruction_observer;

    void RecordLocked(const char* call);

    void SetOpenResult(
        runtime::BackendResult result,
        runtime::BackendCoreState state = runtime::BackendCoreState::Running);
    void SetRebootResult(runtime::BackendResult result);
    void SetCloseResult(runtime::BackendResult result);
    void SetPauseResult(runtime::BackendResult result);
    void SetResumeResult(runtime::BackendResult result);
    void SetStepInstructionResult(runtime::BackendResult result);
    void SetStepFrameResult(runtime::BackendResult result);
    void SetRestoreFileResult(runtime::BackendResult result);
    void SetRestoreBufferResult(runtime::BackendResult result);
    void SetSaveFileResult(runtime::BackendResult result);
    void SetSaveBufferResult(
        runtime::BackendResult result,
        std::vector<std::uint8_t> bytes = {0x10, 0x20, 0x30});
    void SetScreenshotResult(runtime::BackendResult result);
    void SetCoreState(runtime::BackendCoreState state);
    void SetDestructionObserver(std::function<void()> observer);

    [[nodiscard]] int CloseCount() const;
    [[nodiscard]] int OpenCount() const;
    [[nodiscard]] int DestructionCount() const;
    [[nodiscard]] int ScreenshotCount() const;
    [[nodiscard]] std::vector<std::filesystem::path> ScreenshotPaths() const;
    [[nodiscard]] std::vector<std::string> Calls() const;
    [[nodiscard]] bool HasOwnerViolation() const;
    [[nodiscard]] std::optional<std::thread::id> OwnerThread() const;
    [[nodiscard]] bool WaitForCallCount(
        std::size_t count,
        std::chrono::milliseconds timeout = std::chrono::seconds(5));
};

class ScriptedDolphinBackend final : public runtime::IDolphinBackend
{
public:
    explicit ScriptedDolphinBackend(
        std::shared_ptr<ScriptedDolphinBackendControl> control,
        std::unique_ptr<runtime::IPhysicalStopPointBackendPort>
            physical_stop_points = {});
    ~ScriptedDolphinBackend() override;

    runtime::BackendResult Open(
        const runtime::BackendOpenOptions& options) override;
    runtime::BackendResult Reboot() override;
    runtime::BackendResult Close() override;

    [[nodiscard]] runtime::BackendCoreState QueryCoreState() const noexcept override;
    [[nodiscard]] runtime::BackendHealthReport CheckHealth() const override;

    runtime::BackendResult Pause(std::chrono::milliseconds timeout) override;
    runtime::BackendResult Resume() override;
    runtime::BackendResult StepInstruction(
        std::chrono::milliseconds timeout) override;
    runtime::BackendResult StepFrame(std::chrono::milliseconds timeout) override;

    runtime::BackendResult RestoreStateFile(
        const std::filesystem::path& path) override;
    runtime::BackendResult SaveStateFile(
        const std::filesystem::path& path) override;
    runtime::BackendBufferResult SaveStateBuffer() override;
    runtime::BackendResult RestoreStateBuffer(
        const std::vector<std::uint8_t>& bytes) override;

    runtime::BackendResult CaptureScreenshot(
        const std::filesystem::path& path,
        std::chrono::milliseconds timeout) override;
    [[nodiscard]] runtime::IPhysicalStopPointBackendPort*
    PhysicalStopPoints() noexcept override;

private:
    std::shared_ptr<ScriptedDolphinBackendControl> control_;
    std::unique_ptr<runtime::IPhysicalStopPointBackendPort>
        physical_stop_points_;
};

[[nodiscard]] std::unique_ptr<runtime::IDolphinBackend>
MakeScriptedDolphinBackend(
    std::shared_ptr<ScriptedDolphinBackendControl> control,
    std::unique_ptr<runtime::IPhysicalStopPointBackendPort>
        physical_stop_points = {});

} // namespace savor::test_support
