#include "ScriptedDolphinBackend.h"

#include "FakePhysicalStopBackend.h"
#include "Runner/Runtime/StopPoints/IPhysicalStopPointBackendPort.h"

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

void ScriptedDolphinBackendControl::SetRebootResult(
    runtime::BackendResult result)
{
    std::lock_guard lock(mutex);
    reboot_result = std::move(result);
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

void ScriptedDolphinBackendControl::SetStepInstructionResult(
    runtime::BackendResult result)
{
    std::lock_guard lock(mutex);
    step_instruction_result = std::move(result);
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
        control_->core_state = control_->open_core_state;
    control_->changed.notify_all();
    return result;
}

runtime::BackendResult ScriptedDolphinBackend::Reboot()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("reboot");
    ++control_->reboot_count;
    runtime::BackendResult result = control_->reboot_result;
    if (result.ok)
        control_->core_state = runtime::BackendCoreState::Running;
    else if (result.integrity == runtime::BackendIntegrity::Unknown)
        control_->core_state = runtime::BackendCoreState::Unknown;
    control_->changed.notify_all();
    return result;
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

runtime::BackendResult ScriptedDolphinBackend::StepInstruction(
    std::chrono::milliseconds)
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("step_instruction");
    control_->changed.notify_all();
    return control_->step_instruction_result;
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
    if (!result.ok && result.integrity == runtime::BackendIntegrity::Unknown)
        control_->core_state = runtime::BackendCoreState::Unknown;
    control_->changed.notify_all();
    return result;
}

runtime::BackendResult ScriptedDolphinBackend::SaveStateFile(
    const std::filesystem::path&)
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("save_file");
    control_->changed.notify_all();
    return control_->save_file_result;
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
    if (!result.ok && result.integrity == runtime::BackendIntegrity::Unknown)
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

runtime::BackendExecutionCapabilityMask
ScriptedDolphinBackend::Capabilities() const noexcept
{
    return runtime::BackendExecutionCapability::Pause |
        runtime::BackendExecutionCapability::Resume |
        runtime::BackendExecutionCapability::FrameStep |
        runtime::BackendExecutionCapability::ExactInstructionStep |
        runtime::BackendExecutionCapability::ViObservation |
        runtime::BackendExecutionCapability::MovieObservation |
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
        control_->movie_state,
        control_->movie_input_count,
        control_->throttle_disabled};
}

runtime::BackendResult ScriptedDolphinBackend::RequestPause()
{
    return Pause(std::chrono::milliseconds(0));
}

runtime::BackendResult ScriptedDolphinBackend::BeginFrameStep()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("begin_frame_step");
    runtime::BackendResult result = control_->step_frame_result;
    if (result.ok)
    {
        ++control_->vi_count;
        control_->core_state = runtime::BackendCoreState::Paused;
    }
    control_->changed.notify_all();
    return result;
}

runtime::BackendResult ScriptedDolphinBackend::BeginExactInstructionStep()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("begin_instruction_step");
    runtime::BackendResult result = control_->step_instruction_result;
    if (result.ok)
    {
        control_->pc += 4;
        control_->core_state = runtime::BackendCoreState::Paused;
    }
    control_->changed.notify_all();
    return result;
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
