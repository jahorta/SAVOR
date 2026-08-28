#include "ScriptedDolphinBackend.h"

#include "FakePhysicalStopBackend.h"
#include "Runner/Runtime/StopPoints/IPhysicalStopPointBackendPort.h"

#include <fstream>
#include <limits>
#include <utility>

namespace savor::test_support {

void ScriptedDolphinBackendControl::RecordLocked(const char* call)
{
    const std::thread::id current = std::this_thread::get_id();
    if (!owner_thread)
        owner_thread = current;
    else if (*owner_thread != current)
        owner_thread_violation = true;
    calls.emplace_back(call);
}

void ScriptedDolphinBackendControl::SetOpenResult(
    runtime::BackendResult result,
    runtime::BackendCoreState state)
{
    std::lock_guard lock(mutex);
    open_result = std::move(result);
    open_core_state = state;
}

void ScriptedDolphinBackendControl::SetCoreStopResult(
    runtime::MovieBackendResult result)
{
    std::lock_guard lock(mutex);
    core_stop_result = std::move(result);
}

void ScriptedDolphinBackendControl::SetCoreStartResult(
    runtime::MovieBackendResult result)
{
    std::lock_guard lock(mutex);
    core_start_result = std::move(result);
}

void ScriptedDolphinBackendControl::SetMovieActivationResult(
    runtime::MovieBackendResult result)
{
    std::lock_guard lock(mutex);
    movie_activation_result = std::move(result);
}

void ScriptedDolphinBackendControl::SetCloseResult(
    runtime::BackendResult result)
{
    std::lock_guard lock(mutex);
    close_result = std::move(result);
}

void ScriptedDolphinBackendControl::SetPauseResult(
    runtime::BackendResult result)
{
    std::lock_guard lock(mutex);
    pause_result = std::move(result);
}

void ScriptedDolphinBackendControl::SetResumeResult(
    runtime::BackendResult result)
{
    std::lock_guard lock(mutex);
    resume_result = std::move(result);
}

void ScriptedDolphinBackendControl::SetStepFrameResult(
    runtime::BackendResult result)
{
    std::lock_guard lock(mutex);
    step_frame_result = std::move(result);
}

void ScriptedDolphinBackendControl::SetRestoreFileResult(
    runtime::BackendResult result)
{
    std::lock_guard lock(mutex);
    restore_file_result = std::move(result);
}

void ScriptedDolphinBackendControl::SetRestoreBufferResult(
    runtime::BackendResult result)
{
    std::lock_guard lock(mutex);
    restore_buffer_result = std::move(result);
}

void ScriptedDolphinBackendControl::SetSaveFileResult(
    runtime::BackendResult result)
{
    std::lock_guard lock(mutex);
    save_file_result = std::move(result);
}

void ScriptedDolphinBackendControl::SetSaveBufferResult(
    runtime::BackendResult result,
    std::vector<std::uint8_t> bytes)
{
    std::lock_guard lock(mutex);
    save_buffer_result = std::move(result);
    save_buffer_bytes = std::move(bytes);
}

void ScriptedDolphinBackendControl::SetScreenshotResult(
    runtime::BackendResult result)
{
    std::lock_guard lock(mutex);
    screenshot_result = std::move(result);
}

void ScriptedDolphinBackendControl::SetCoreState(
    runtime::BackendCoreState state)
{
    std::lock_guard lock(mutex);
    core_state = state;
}

void ScriptedDolphinBackendControl::SetDestructionObserver(
    std::function<void()> observer)
{
    std::lock_guard lock(mutex);
    destruction_observer = std::move(observer);
}

int ScriptedDolphinBackendControl::CloseCount() const
{
    std::lock_guard lock(mutex);
    return close_count;
}

int ScriptedDolphinBackendControl::OpenCount() const
{
    std::lock_guard lock(mutex);
    return open_count;
}

int ScriptedDolphinBackendControl::DestructionCount() const
{
    std::lock_guard lock(mutex);
    return destruction_count;
}

int ScriptedDolphinBackendControl::ScreenshotCount() const
{
    std::lock_guard lock(mutex);
    return screenshot_count;
}

std::vector<std::filesystem::path>
ScriptedDolphinBackendControl::ScreenshotPaths() const
{
    std::lock_guard lock(mutex);
    return screenshots;
}

std::vector<std::string> ScriptedDolphinBackendControl::Calls() const
{
    std::lock_guard lock(mutex);
    return calls;
}

bool ScriptedDolphinBackendControl::HasOwnerViolation() const
{
    std::lock_guard lock(mutex);
    return owner_thread_violation;
}

std::optional<std::thread::id>
ScriptedDolphinBackendControl::OwnerThread() const
{
    std::lock_guard lock(mutex);
    return owner_thread;
}

bool ScriptedDolphinBackendControl::WaitForCallCount(
    std::size_t count,
    std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, timeout, [&] {
        return calls.size() >= count;
    });
}

ScriptedDolphinBackend::ScriptedDolphinBackend(
    std::shared_ptr<ScriptedDolphinBackendControl> control,
    std::unique_ptr<runtime::IPhysicalStopPointBackendPort>
        physical_stop_points)
    : control_(std::move(control)),
      physical_stop_points_(std::move(physical_stop_points))
{
    if (!physical_stop_points_)
    {
        physical_stop_points_ = MakeFakePhysicalStopBackend(
            std::make_shared<FakePhysicalStopBackendControl>());
    }
}

ScriptedDolphinBackend::~ScriptedDolphinBackend()
{
    try
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("destroy");
        ++control_->destruction_count;
        if (control_->destruction_observer)
            control_->destruction_observer();
        control_->changed.notify_all();
    }
    catch (...)
    {
    }
}

runtime::BackendResult ScriptedDolphinBackend::Open(
    const runtime::BackendOpenOptions&)
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("open");
    ++control_->open_count;
    runtime::BackendResult result = control_->open_result;
    if (result.ok)
    {
        control_->core_state = control_->open_core_state;
        control_->movie_observation = {
            .result = runtime::MovieBackendResult::Success()};
    }
    control_->changed.notify_all();
    return result;
}

runtime::MovieBackendResult
ScriptedDolphinBackend::StopCoreForPreparedReadOnlyMovie()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("movie.stop-core-for-playback");
    ++control_->core_stop_count;
    runtime::MovieBackendResult result = control_->core_stop_result;
    if (result.ok)
        control_->core_state = runtime::BackendCoreState::Closed;
    else if (result.integrity == runtime::GuestIntegrity::Unknown)
        control_->core_state = runtime::BackendCoreState::Unknown;
    control_->changed.notify_all();
    return result;
}

runtime::MovieBackendResult
ScriptedDolphinBackend::StartPreparedReadOnlyMovieCorePaused()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("movie.start-prepared-core-paused");
    ++control_->core_start_count;
    runtime::MovieBackendResult result = control_->core_start_result;
    if (result.ok)
    {
        control_->core_state = runtime::BackendCoreState::Paused;
        control_->movie_observation = {
            .result = runtime::MovieBackendResult::Success(),
            .playing = true,
            .recording = false,
            .read_only = true};
    }
    else if (result.integrity == runtime::GuestIntegrity::Unknown)
        control_->core_state = runtime::BackendCoreState::Unknown;
    control_->changed.notify_all();
    return result;
}

runtime::MovieBackendResult
ScriptedDolphinBackend::ActivatePreparedReadOnlyMoviePlayback()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("movie.activate-prepared-playback");
    ++control_->movie_activation_count;
    return control_->movie_activation_result;
}

runtime::MovieBackendResult
ScriptedDolphinBackend::DiscardPreparedReadOnlyMovie() noexcept
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("movie.discard-prepared-playback");
    control_->prepared_movie_path.clear();
    return runtime::MovieBackendResult::Success();
}

runtime::BackendResult ScriptedDolphinBackend::Close()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("close");
    ++control_->close_count;
    runtime::BackendResult result = control_->close_result;
    control_->core_state = result.ok
        ? runtime::BackendCoreState::Closed
        : runtime::BackendCoreState::Unknown;
    if (result.ok)
    {
        control_->movie_observation = {
            .result = runtime::MovieBackendResult::Success()};
    }
    control_->changed.notify_all();
    return result;
}

runtime::BackendCoreState ScriptedDolphinBackend::QueryCoreState() const noexcept
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("query_core_state");
    control_->changed.notify_all();
    return control_->core_state;
}

runtime::BackendHealthReport ScriptedDolphinBackend::CheckHealth() const
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("check_health");
    const bool healthy =
        control_->core_state == runtime::BackendCoreState::Running ||
        control_->core_state == runtime::BackendCoreState::Paused;
    control_->changed.notify_all();
    return {
        healthy,
        control_->core_state,
        healthy ? std::string{} : std::string{"scripted backend is unhealthy"}};
}

runtime::ArtifactCompatibilityToken
ScriptedDolphinBackend::SavestateCompatibility() const
{
    return {
        .game_id = "TEST00",
        .iso_sha256 = std::string(64, '0'),
        .emulator_build = "scripted-dolphin-backend",
        .runtime_revision = "slice4",
    };
}

runtime::BackendResult ScriptedDolphinBackend::Pause(
    std::chrono::milliseconds)
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("pause");
    runtime::BackendResult result = control_->pause_result;
    if (result.ok)
        control_->core_state = runtime::BackendCoreState::Paused;
    control_->changed.notify_all();
    return result;
}

runtime::BackendResult ScriptedDolphinBackend::Resume()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("resume");
    runtime::BackendResult result = control_->resume_result;
    if (result.ok)
        control_->core_state = runtime::BackendCoreState::Running;
    control_->changed.notify_all();
    return result;
}

runtime::BackendResult ScriptedDolphinBackend::StepFrame(
    std::chrono::milliseconds)
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("step_frame");
    control_->changed.notify_all();
    return control_->step_frame_result;
}

runtime::BackendResult ScriptedDolphinBackend::RestoreStateFile(
    const std::filesystem::path&)
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("restore_file");
    ++control_->restore_file_count;
    runtime::BackendResult result = control_->restore_file_result;
    if (result.ok)
    {
        if (control_->restore_file_pc)
            control_->pc = *control_->restore_file_pc;
        if (control_->restore_file_core_state)
            control_->core_state = *control_->restore_file_core_state;
    }
    else if (result.integrity == runtime::BackendIntegrity::Unknown)
        control_->core_state = runtime::BackendCoreState::Unknown;
    control_->changed.notify_all();
    return result;
}

runtime::BackendResult ScriptedDolphinBackend::SaveStateFile(
    const std::filesystem::path& path)
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("save_file");
    if (control_->save_file_result.ok)
    {
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            control_->changed.notify_all();
            return runtime::BackendResult::Failure(
                runtime::BackendErrorCode::OperationFailed,
                "scripted state file could not be created");
        }
        output.write(
            reinterpret_cast<const char*>(
                control_->save_file_bytes.data()),
            static_cast<std::streamsize>(
                control_->save_file_bytes.size()));
        if (!output.good())
        {
            control_->changed.notify_all();
            return runtime::BackendResult::Failure(
                runtime::BackendErrorCode::OperationFailed,
                "scripted state file could not be written");
        }
    }
    control_->changed.notify_all();
    return control_->save_file_result;
}

runtime::BackendBufferResult ScriptedDolphinBackend::SaveStateFileBytes()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("save_file_bytes");
    control_->changed.notify_all();
    return {control_->save_file_result, control_->save_file_bytes};
}

runtime::BackendBufferResult ScriptedDolphinBackend::SaveStateBuffer()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("save_buffer");
    control_->changed.notify_all();
    return {control_->save_buffer_result, control_->save_buffer_bytes};
}

runtime::BackendResult ScriptedDolphinBackend::RestoreStateBuffer(
    const std::vector<std::uint8_t>&)
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("restore_buffer");
    ++control_->restore_buffer_count;
    runtime::BackendResult result = control_->restore_buffer_result;
    if (result.ok)
    {
        if (control_->restore_buffer_pc)
            control_->pc = *control_->restore_buffer_pc;
        if (control_->restore_buffer_core_state)
            control_->core_state = *control_->restore_buffer_core_state;
    }
    else if (result.integrity == runtime::BackendIntegrity::Unknown)
        control_->core_state = runtime::BackendCoreState::Unknown;
    control_->changed.notify_all();
    return result;
}

runtime::BackendResult ScriptedDolphinBackend::CaptureScreenshot(
    const std::filesystem::path& path,
    std::chrono::milliseconds)
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("screenshot");
    ++control_->screenshot_count;
    control_->screenshots.push_back(path);
    runtime::BackendResult result = control_->screenshot_result;
    if (!result.ok && result.integrity == runtime::BackendIntegrity::Unknown)
        control_->core_state = runtime::BackendCoreState::Unknown;
    control_->changed.notify_all();
    return result;
}

runtime::IPhysicalStopPointBackendPort*
ScriptedDolphinBackend::PhysicalStopPoints() noexcept
{
    return physical_stop_points_.get();
}

runtime::IExecutionBackendPort*
ScriptedDolphinBackend::Execution() noexcept
{
    return this;
}

runtime::IInputBackendPort* ScriptedDolphinBackend::Input() noexcept
{
    return this;
}

runtime::IGuestMemoryBackendPort*
ScriptedDolphinBackend::GuestMemory() noexcept
{
    return this;
}

runtime::IHitTimeGuestMemoryBackendPort*
ScriptedDolphinBackend::HitTimeGuestMemory() noexcept
{
    std::lock_guard lock(control_->mutex);
    return control_->hit_time_memory_available ? this : nullptr;
}

runtime::IScreenshotBackendPort*
ScriptedDolphinBackend::Screenshots() noexcept
{
    return this;
}

runtime::IMovieBackendPort*
ScriptedDolphinBackend::Movies() noexcept
{
    std::lock_guard lock(control_->mutex);
    return control_->movie_available ? this : nullptr;
}

runtime::ICaptureBackendPort*
ScriptedDolphinBackend::Captures() noexcept
{
    return nullptr;
}

runtime::IVisualMessageBackendPort*
ScriptedDolphinBackend::VisualMessages() noexcept
{
    return nullptr;
}

bool ScriptedDolphinBackend::IsAvailable(std::uint8_t port) const noexcept
{
    std::lock_guard lock(control_->mutex);
    return port == 0 &&
        control_->core_state != runtime::BackendCoreState::Closed;
}

runtime::BackendInputPublication ScriptedDolphinBackend::Publish(
    std::uint8_t port,
    const savor::GCInputFrame& frame)
{
    std::lock_guard lock(control_->mutex);
    if (port != 0 ||
        control_->core_state == runtime::BackendCoreState::Closed)
    {
        return {
            runtime::BackendResult::Failure(
                runtime::BackendErrorCode::Unavailable,
                "scripted input port is unavailable"),
            0};
    }
    control_->input_frame = frame;
    control_->input_callback_count = 0;
    control_->input_a_control_callback_count = 0;
    ++control_->input_publication_epoch;
    control_->RecordLocked("input.publish");
    return {runtime::BackendResult::Success(), control_->input_publication_epoch};
}

runtime::BackendInputPoll ScriptedDolphinBackend::QueryPoll(
    std::uint8_t port) const
{
    std::lock_guard lock(control_->mutex);
    if (port != 0)
    {
        return {
            runtime::BackendResult::Failure(
                runtime::BackendErrorCode::Unavailable,
                "scripted input port is unavailable")};
    }
    return {
        .result = runtime::BackendResult::Success(),
        .publication_epoch = control_->input_publication_epoch,
        .callback_count = control_->input_callback_count,
        .a_control_callback_count = control_->input_a_control_callback_count,
        .frame = control_->input_frame};
}

bool ScriptedDolphinBackend::IsPaused() const noexcept
{
    std::lock_guard lock(control_->mutex);
    return control_->core_state == runtime::BackendCoreState::Paused;
}

runtime::GuestBytesResult ScriptedDolphinBackend::Read(
    std::uint32_t address,
    std::size_t size) const
{
    std::lock_guard lock(control_->mutex);
    if (control_->core_state != runtime::BackendCoreState::Paused)
    {
        return {
            runtime::BackendResult::Failure(
                runtime::BackendErrorCode::InvalidState,
                "scripted guest memory is not paused"),
            {}};
    }
    std::vector<std::uint8_t> bytes(size, 0);
    for (std::size_t index = 0; index < size; ++index)
    {
        const auto found = control_->guest_memory.find(
            address + static_cast<std::uint32_t>(index));
        if (found != control_->guest_memory.end())
            bytes[index] = found->second;
    }
    return {runtime::BackendResult::Success(), std::move(bytes)};
}

runtime::HitTimeGuestReadReceipt
ScriptedDolphinBackend::ReadHitTimeBytes(
    std::uint32_t address,
    std::span<std::uint8_t> destination) const noexcept
{
    if (destination.empty() ||
        destination.size() > std::numeric_limits<std::uint32_t>::max() ||
        address > std::numeric_limits<std::uint32_t>::max() -
            static_cast<std::uint32_t>(destination.size() - 1))
    {
        return {
            false,
            runtime::HitTimeGuestReadError::InvalidArgument,
            address,
            destination.size()};
    }
    std::lock_guard lock(control_->mutex);
    for (std::size_t index = 0; index < destination.size(); ++index)
    {
        const auto found = control_->guest_memory.find(
            address + static_cast<std::uint32_t>(index));
        destination[index] = found == control_->guest_memory.end()
            ? 0
            : found->second;
    }
    return {
        true,
        runtime::HitTimeGuestReadError::None,
        address,
        destination.size()};
}

runtime::BackendResult ScriptedDolphinBackend::Write(
    std::uint32_t address,
    const std::vector<std::uint8_t>& bytes)
{
    std::lock_guard lock(control_->mutex);
    if (control_->core_state != runtime::BackendCoreState::Paused)
    {
        return runtime::BackendResult::Failure(
            runtime::BackendErrorCode::InvalidState,
            "scripted guest memory is not paused");
    }
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        control_->guest_memory[
            address + static_cast<std::uint32_t>(index)] = bytes[index];
    }
    control_->RecordLocked("memory.write");
    return runtime::BackendResult::Success();
}

runtime::BackendResult ScriptedDolphinBackend::InvalidateExecutableRange(
    std::uint32_t address,
    std::size_t size)
{
    std::lock_guard lock(control_->mutex);
    control_->invalidations.emplace_back(address, size);
    control_->RecordLocked("memory.invalidate");
    return runtime::BackendResult::Success();
}

runtime::BackendResult ScriptedDolphinBackend::Capture(
    const std::filesystem::path& path,
    std::chrono::milliseconds timeout)
{
    return CaptureScreenshot(path, timeout);
}

runtime::MoviePlaybackPrepareResult
ScriptedDolphinBackend::PrepareReadOnlyPlaybackForRestart(
    const std::filesystem::path& dtm_path)
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("movie.prepare-playback");
    control_->prepared_movie_path = dtm_path;
    return {
        control_->movie_result,
        control_->movie_startup_savestate};
}

runtime::MovieBackendResult
ScriptedDolphinBackend::StopMovie() noexcept
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("movie.stop");
    if (control_->movie_result.ok)
        control_->movie_observation = {
            .result = runtime::MovieBackendResult::Success()};
    return control_->movie_result;
}

runtime::MovieBackendResult
ScriptedDolphinBackend::BeginRecording()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("movie.begin-recording");
    if (control_->movie_result.ok)
    {
        control_->movie_observation.result =
            runtime::MovieBackendResult::Success();
        control_->movie_observation.playing = false;
        control_->movie_observation.recording = true;
        control_->movie_observation.read_only = false;
    }
    return control_->movie_result;
}

runtime::MovieBackendResult
ScriptedDolphinBackend::BranchReadOnlyPlaybackToRecording()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("movie.branch-playback-to-recording");
    if (!control_->movie_observation.playing ||
        control_->movie_observation.recording ||
        !control_->movie_observation.read_only)
    {
        return runtime::MovieBackendResult::Failure(
            "read-only playback is not active");
    }
    control_->movie_observation.playing = false;
    control_->movie_observation.recording = true;
    control_->movie_observation.read_only = false;
    control_->movie_observation.result =
        runtime::MovieBackendResult::Success();
    return runtime::MovieBackendResult::Success();
}

runtime::MovieRecordingFinalizeResult
ScriptedDolphinBackend::FinalizeRecording(
    const std::filesystem::path&)
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("movie.finalize-recording");
    return {
        runtime::MovieBackendResult::Failure(
            "Scripted movie finalization is unsupported"),
        std::nullopt};
}

runtime::MovieBackendResult
ScriptedDolphinBackend::CancelRecording() noexcept
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("movie.cancel-recording");
    if (control_->movie_result.ok)
        control_->movie_observation = {
            .result = runtime::MovieBackendResult::Success()};
    return control_->movie_result;
}

runtime::MovieBackendObservation
ScriptedDolphinBackend::ObserveMovieWhilePaused() const
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("movie.observe-paused");
    ++control_->movie_observation_count;
    if (control_->core_state != runtime::BackendCoreState::Paused)
    {
        return {
            .result = runtime::MovieBackendResult::Failure(
                "scripted movie inspection requires a paused core"),
        };
    }
    return control_->movie_observation;
}

runtime::MovieBackendResult
ScriptedDolphinBackend::AcquirePauseAtPlaybackEnd()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("movie.acquire-pause-at-end");
    if (!control_->pause_at_playback_end_available)
    {
        return runtime::MovieBackendResult::Failure(
            "scripted pause-at-playback-end is unavailable");
    }
    if (control_->pause_at_playback_end)
    {
        return runtime::MovieBackendResult::Failure(
            "scripted pause-at-playback-end is already owned");
    }
    control_->pause_at_playback_end = true;
    return runtime::MovieBackendResult::Success();
}

runtime::MovieBackendResult
ScriptedDolphinBackend::ReleasePauseAtPlaybackEnd() noexcept
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("movie.release-pause-at-end");
    control_->pause_at_playback_end = false;
    return runtime::MovieBackendResult::Success();
}

runtime::MovieCheckpointBackendResult
ScriptedDolphinBackend::CaptureRecordingCheckpoint()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("movie.capture-checkpoint");
    return {
        runtime::MovieBackendResult::Failure(
            "Scripted movie checkpoint capture is unsupported"),
        {}};
}

runtime::MovieBackendResult
ScriptedDolphinBackend::PrepareSavestateRestore(
    const runtime::SavestateMovieRestoreContext&)
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("movie.prepare-state");
    return control_->movie_result;
}

runtime::MovieBackendResult
ScriptedDolphinBackend::CommitSavestateRestore(
    const runtime::SavestateMovieRestoreContext& context)
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("movie.commit-state");
    if (!control_->movie_result.ok)
        return control_->movie_result;
    if (!context.movie.has_value())
    {
        control_->movie_observation = {
            .result = runtime::MovieBackendResult::Success()};
    }
    else
    {
        const bool recording = context.movie->mode ==
            runtime::MovieCheckpointMode::Recording;
        control_->movie_observation.result =
            runtime::MovieBackendResult::Success();
        control_->movie_observation.playing = !recording;
        control_->movie_observation.recording = recording;
        control_->movie_observation.read_only = !recording;
        if (context.movie->cursor_known)
        {
            control_->movie_observation.current_frame =
                context.movie->current_frame;
            control_->movie_observation.current_input_count =
                context.movie->current_input_count;
        }
    }
    return runtime::MovieBackendResult::Success();
}

runtime::MovieBackendResult
ScriptedDolphinBackend::RollbackSavestateRestore(
    const runtime::SavestateMovieRestoreContext&) noexcept
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("movie.rollback-state");
    return control_->movie_result;
}

runtime::BackendExecutionCapabilityMask
ScriptedDolphinBackend::Capabilities() const noexcept
{
    return runtime::BackendExecutionCapability::Pause |
        runtime::BackendExecutionCapability::Resume |
        runtime::BackendExecutionCapability::FrameStep |
        runtime::BackendExecutionCapability::ViObservation |
        runtime::BackendExecutionCapability::ThrottleControl;
}

runtime::BackendExecutionSnapshot
ScriptedDolphinBackend::QueryExecutionSnapshot() const
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("query_execution");
    control_->changed.notify_all();
    return {
        runtime::BackendResult::Success(),
        control_->core_state,
        control_->core_state == runtime::BackendCoreState::Paused,
        control_->pc,
        control_->vi_count,
        control_->throttle_disabled};
}

runtime::BackendResult ScriptedDolphinBackend::SubmitControlTask(
    runtime::BackendControlTask task)
{
    {
        std::lock_guard lock(control_->mutex);
        if (control_->control_task_state !=
            runtime::BackendExecutionSnapshot::ControlTaskState::Idle)
        {
            return runtime::BackendResult::Failure(
                runtime::BackendErrorCode::InvalidState,
                "scripted execution actuator is busy");
        }
        control_->control_task_state =
            runtime::BackendExecutionSnapshot::ControlTaskState::Running;
    }
    runtime::BackendResult result;
    switch (task.kind)
    {
    case runtime::BackendControlTaskKind::Pause:
        result = Pause(std::chrono::milliseconds(0));
        break;
    case runtime::BackendControlTaskKind::Resume:
        result = Resume();
        break;
    case runtime::BackendControlTaskKind::FrameStep:
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("begin_frame_step");
        result = control_->step_frame_result;
        if (result.ok)
        {
            ++control_->vi_count;
            control_->core_state = runtime::BackendCoreState::Paused;
        }
        control_->changed.notify_all();
        break;
    }
    case runtime::BackendControlTaskKind::SynchronizePaused:
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("synchronize_paused");
        result = control_->core_state == runtime::BackendCoreState::Paused
            ? runtime::BackendResult::Success()
            : runtime::BackendResult::Failure(
                  runtime::BackendErrorCode::InvalidState,
                  "scripted core is not paused");
        break;
    }
    }
    {
        std::lock_guard lock(control_->mutex);
        control_->control_completion = runtime::BackendControlCompletion{
            task.kind,
            result};
        control_->control_task_state =
            runtime::BackendExecutionSnapshot::ControlTaskState::Completed;
    }
    return runtime::BackendResult::Success();
}

std::optional<runtime::BackendControlCompletion>
ScriptedDolphinBackend::TakeControlCompletion()
{
    std::lock_guard lock(control_->mutex);
    if (control_->control_task_state !=
            runtime::BackendExecutionSnapshot::ControlTaskState::Completed ||
        !control_->control_completion)
    {
        return std::nullopt;
    }
    auto completion = std::move(control_->control_completion);
    control_->control_completion.reset();
    control_->control_task_state =
        runtime::BackendExecutionSnapshot::ControlTaskState::Idle;
    return completion;
}

runtime::BackendResult ScriptedDolphinBackend::SetThrottleDisabled(
    bool disabled)
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked(
        disabled ? "disable_throttle" : "enable_throttle");
    control_->throttle_disabled = disabled;
    control_->changed.notify_all();
    return runtime::BackendResult::Success();
}

std::unique_ptr<runtime::IDolphinBackend> MakeScriptedDolphinBackend(
    std::shared_ptr<ScriptedDolphinBackendControl> control,
    std::unique_ptr<runtime::IPhysicalStopPointBackendPort>
        physical_stop_points)
{
    return std::make_unique<ScriptedDolphinBackend>(
        std::move(control),
        std::move(physical_stop_points));
}

} // namespace savor::test_support
