#pragma once

#include "Runner/Runtime/IDolphinBackend.h"
#include "Runner/Runtime/Execution/IExecutionBackendPort.h"
#include "Runner/Runtime/Services/Input/IInputBackendPort.h"
#include "Runner/Runtime/Services/Memory/IGuestMemoryBackendPort.h"
#include "Runner/Runtime/Services/Movie/IMovieBackendPort.h"
#include "Runner/Runtime/Services/Screenshot/IScreenshotBackendPort.h"
#include "Runner/Runtime/StopPoints/IPhysicalStopPointBackendPort.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <map>
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
    runtime::MovieBackendResult core_stop_result =
        runtime::MovieBackendResult::Success();
    runtime::MovieBackendResult core_start_result =
        runtime::MovieBackendResult::Success();
    runtime::BackendResult close_result = runtime::BackendResult::Success();
    runtime::BackendResult pause_result = runtime::BackendResult::Success();
    runtime::BackendResult resume_result = runtime::BackendResult::Success();
    runtime::BackendResult step_frame_result = runtime::BackendResult::Success();
    runtime::BackendResult restore_file_result = runtime::BackendResult::Success();
    runtime::BackendResult restore_buffer_result = runtime::BackendResult::Success();
    std::optional<std::uint32_t> restore_file_pc;
    std::optional<runtime::BackendCoreState> restore_file_core_state;
    std::optional<std::uint32_t> restore_buffer_pc;
    std::optional<runtime::BackendCoreState> restore_buffer_core_state;
    runtime::BackendResult save_file_result = runtime::BackendResult::Success();
    runtime::BackendResult save_buffer_result = runtime::BackendResult::Success();
    runtime::BackendResult screenshot_result = runtime::BackendResult::Success();
    runtime::MovieBackendResult movie_result =
        runtime::MovieBackendResult::Success();
    runtime::MovieBackendResult movie_activation_result =
        runtime::MovieBackendResult::Success();
    runtime::BackendCoreState core_state = runtime::BackendCoreState::Closed;
    runtime::BackendCoreState open_core_state = runtime::BackendCoreState::Paused;
    std::uint32_t pc = 0x80000000u;
    std::uint64_t vi_count = 0;
    runtime::BackendMovieState movie_state = runtime::BackendMovieState::Inactive;
    std::uint64_t movie_input_count = 0;
    bool throttle_disabled = false;
    std::uint64_t input_publication_epoch = 0;
    std::uint32_t input_callback_count = 0;
    savor::GCInputFrame input_frame{};
    std::map<std::uint32_t, std::uint8_t> guest_memory;
    std::vector<std::pair<std::uint32_t, std::size_t>> invalidations;
    std::vector<std::uint8_t> save_buffer_bytes{0x10, 0x20, 0x30};
    std::vector<std::uint8_t> save_file_bytes{0x10, 0x20, 0x30};
    runtime::MovieSnapshot movie_snapshot;
    std::optional<std::filesystem::path> movie_startup_savestate;
    std::filesystem::path prepared_movie_path;
    bool movie_available = true;

    int open_count = 0;
    int core_stop_count = 0;
    int core_start_count = 0;
    int movie_activation_count = 0;
    int close_count = 0;
    int screenshot_count = 0;
    int restore_file_count = 0;
    int restore_buffer_count = 0;
    int destruction_count = 0;
    std::function<void()> destruction_observer;

    void RecordLocked(const char* call);

    void SetOpenResult(
        runtime::BackendResult result,
        runtime::BackendCoreState state = runtime::BackendCoreState::Paused);
    void SetCoreStopResult(runtime::MovieBackendResult result);
    void SetCoreStartResult(runtime::MovieBackendResult result);
    void SetMovieActivationResult(runtime::MovieBackendResult result);
    void SetCloseResult(runtime::BackendResult result);
    void SetPauseResult(runtime::BackendResult result);
    void SetResumeResult(runtime::BackendResult result);
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

class ScriptedDolphinBackend final
    : public runtime::IDolphinBackend,
      private runtime::IExecutionBackendPort,
      private runtime::IInputBackendPort,
      private runtime::IGuestMemoryBackendPort,
      private runtime::IScreenshotBackendPort,
      private runtime::IMovieBackendPort
{
public:
    explicit ScriptedDolphinBackend(
        std::shared_ptr<ScriptedDolphinBackendControl> control,
        std::unique_ptr<runtime::IPhysicalStopPointBackendPort>
            physical_stop_points = {});
    ~ScriptedDolphinBackend() override;

    runtime::BackendResult Open(
        const runtime::BackendOpenOptions& options) override;
    runtime::BackendResult Close() override;

    [[nodiscard]] runtime::BackendCoreState QueryCoreState() const noexcept override;
    [[nodiscard]] runtime::BackendHealthReport CheckHealth() const override;
    [[nodiscard]] runtime::ArtifactCompatibilityToken
    SavestateCompatibility() const override;

    runtime::BackendResult Pause(std::chrono::milliseconds timeout);
    runtime::BackendResult Resume() override;
    runtime::BackendResult StepFrame(std::chrono::milliseconds timeout);

    runtime::BackendResult RestoreStateFile(
        const std::filesystem::path& path) override;
    runtime::BackendResult SaveStateFile(
        const std::filesystem::path& path) override;
    runtime::BackendBufferResult SaveStateFileBytes() override;
    runtime::BackendBufferResult SaveStateBuffer() override;
    runtime::BackendResult RestoreStateBuffer(
        const std::vector<std::uint8_t>& bytes) override;

    runtime::BackendResult CaptureScreenshot(
        const std::filesystem::path& path,
        std::chrono::milliseconds timeout) override;
    [[nodiscard]] runtime::IPhysicalStopPointBackendPort*
    PhysicalStopPoints() noexcept override;
    [[nodiscard]] runtime::IExecutionBackendPort* Execution() noexcept override;
    [[nodiscard]] runtime::IInputBackendPort* Input() noexcept override;
    [[nodiscard]] runtime::IGuestMemoryBackendPort* GuestMemory() noexcept override;
    [[nodiscard]] runtime::IScreenshotBackendPort* Screenshots() noexcept override;
    [[nodiscard]] runtime::IMovieBackendPort* Movies() noexcept override;
    [[nodiscard]] runtime::ICaptureBackendPort* Captures() noexcept override;

private:
    [[nodiscard]] runtime::BackendExecutionCapabilityMask
    Capabilities() const noexcept override;
    [[nodiscard]] runtime::BackendExecutionSnapshot
    QueryExecutionSnapshot() const override;
    runtime::BackendResult RequestPause() override;
    runtime::BackendResult BeginFrameStep() override;
    runtime::BackendResult SetThrottleDisabled(bool disabled) override;

    [[nodiscard]] bool IsAvailable(std::uint8_t port) const noexcept override;
    [[nodiscard]] runtime::BackendInputPublication Publish(
        std::uint8_t port,
        const savor::GCInputFrame& frame) override;
    [[nodiscard]] runtime::BackendInputPoll QueryPoll(
        std::uint8_t port) const override;

    [[nodiscard]] bool IsPaused() const noexcept override;
    [[nodiscard]] runtime::GuestBytesResult Read(
        std::uint32_t address,
        std::size_t size) const override;
    runtime::BackendResult Write(
        std::uint32_t address,
        const std::vector<std::uint8_t>& bytes) override;
    runtime::BackendResult InvalidateExecutableRange(
        std::uint32_t address,
        std::size_t size) override;

    runtime::BackendResult Capture(
        const std::filesystem::path& path,
        std::chrono::milliseconds timeout) override;

    runtime::MoviePlaybackPrepareResult
    PrepareReadOnlyPlaybackForRestart(
        const std::filesystem::path& dtm_path) override;
    runtime::MovieBackendResult StopCoreForPreparedReadOnlyMovie() override;
    runtime::MovieBackendResult StartPreparedReadOnlyMovieCorePaused() override;
    runtime::MovieBackendResult ActivatePreparedReadOnlyMoviePlayback() override;
    runtime::MovieBackendResult DiscardPreparedReadOnlyMovie() noexcept override;
    runtime::MovieBackendResult StopMovie() noexcept override;
    runtime::MovieBackendResult BeginRecording() override;
    runtime::MovieRecordingFinalizeResult FinalizeRecording(
        const std::filesystem::path& dtm_path) override;
    runtime::MovieBackendResult CancelRecording() noexcept override;
    [[nodiscard]] runtime::MovieSnapshot Snapshot() const override;
    runtime::MovieCheckpointBackendResult
    CaptureRecordingCheckpoint() override;
    runtime::MovieBackendResult PrepareSavestateRestore(
        const runtime::SavestateMovieRestoreContext& context) override;
    runtime::MovieBackendResult CommitSavestateRestore(
        const runtime::SavestateMovieRestoreContext& context) override;
    runtime::MovieBackendResult RollbackSavestateRestore(
        const runtime::SavestateMovieRestoreContext& context) noexcept override;

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
