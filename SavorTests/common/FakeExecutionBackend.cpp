#include "FakeExecutionBackend.h"

#include <utility>

namespace savor::test_support {

void FakeExecutionBackendControl::RecordLocked(const char* call)
{
    const std::thread::id current = std::this_thread::get_id();
    if (!owner_thread)
        owner_thread = current;
    else if (*owner_thread != current)
        owner_thread_violation = true;
    calls.emplace_back(call);
}

void FakeExecutionBackendControl::SetCapabilities(
    runtime::BackendExecutionCapabilityMask value)
{
    std::lock_guard lock(mutex);
    capabilities = value;
}

void FakeExecutionBackendControl::SetSnapshot(
    runtime::BackendExecutionSnapshot value)
{
    std::lock_guard lock(mutex);
    snapshot = std::move(value);
}

void FakeExecutionBackendControl::SetCoreState(runtime::BackendCoreState value)
{
    std::lock_guard lock(mutex);
    snapshot.core_state = value;
    snapshot.pause_confirmed =
        value == runtime::BackendCoreState::Paused;
}

void FakeExecutionBackendControl::SetViCount(std::uint64_t value)
{
    std::lock_guard lock(mutex);
    snapshot.vi_count = value;
}

void FakeExecutionBackendControl::SetMovieState(runtime::BackendMovieState value)
{
    std::lock_guard lock(mutex);
    snapshot.movie_state = value;
}

void FakeExecutionBackendControl::SetPauseResult(runtime::BackendResult value)
{
    std::lock_guard lock(mutex);
    pause_result = std::move(value);
}

void FakeExecutionBackendControl::SetResumeResult(runtime::BackendResult value)
{
    std::lock_guard lock(mutex);
    resume_result = std::move(value);
}

void FakeExecutionBackendControl::SetFrameStepResult(runtime::BackendResult value)
{
    std::lock_guard lock(mutex);
    frame_step_result = std::move(value);
}

void FakeExecutionBackendControl::SetInstructionStepResult(
    runtime::BackendResult value)
{
    std::lock_guard lock(mutex);
    instruction_step_result = std::move(value);
}

void FakeExecutionBackendControl::SetThrottleResult(runtime::BackendResult value)
{
    std::lock_guard lock(mutex);
    throttle_result = std::move(value);
}

std::vector<std::string> FakeExecutionBackendControl::Calls() const
{
    std::lock_guard lock(mutex);
    return calls;
}

runtime::BackendExecutionSnapshot
FakeExecutionBackendControl::Snapshot() const
{
    std::lock_guard lock(mutex);
    return snapshot;
}

bool FakeExecutionBackendControl::HasOwnerViolation() const
{
    std::lock_guard lock(mutex);
    return owner_thread_violation;
}

FakeExecutionBackend::FakeExecutionBackend(
    std::shared_ptr<FakeExecutionBackendControl> control)
    : control_(std::move(control))
{
}

runtime::BackendExecutionCapabilityMask
FakeExecutionBackend::Capabilities() const noexcept
{
    std::lock_guard lock(control_->mutex);
    return control_->capabilities;
}

runtime::BackendExecutionSnapshot
FakeExecutionBackend::QueryExecutionSnapshot() const
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("query");
    return control_->snapshot;
}

runtime::BackendResult FakeExecutionBackend::RequestPause()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("pause");
    const runtime::BackendResult result = control_->pause_result;
    if (result.ok)
    {
        control_->snapshot.core_state = runtime::BackendCoreState::Paused;
        control_->snapshot.pause_confirmed = true;
    }
    return result;
}

runtime::BackendResult FakeExecutionBackend::Resume()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("resume");
    const runtime::BackendResult result = control_->resume_result;
    if (result.ok)
    {
        control_->snapshot.core_state = runtime::BackendCoreState::Running;
        control_->snapshot.pause_confirmed = false;
    }
    return result;
}

runtime::BackendResult FakeExecutionBackend::BeginFrameStep()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("frame_step");
    const runtime::BackendResult result = control_->frame_step_result;
    if (result.ok)
    {
        control_->snapshot.core_state = runtime::BackendCoreState::Paused;
        control_->snapshot.pause_confirmed = true;
        ++control_->snapshot.vi_count;
    }
    return result;
}

runtime::BackendResult FakeExecutionBackend::BeginExactInstructionStep()
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("instruction_step");
    const runtime::BackendResult result = control_->instruction_step_result;
    if (result.ok)
    {
        control_->snapshot.core_state = runtime::BackendCoreState::Paused;
        control_->snapshot.pause_confirmed = true;
        control_->snapshot.pc += 4;
    }
    return result;
}

runtime::BackendResult FakeExecutionBackend::SetThrottleDisabled(bool disabled)
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked(disabled ? "throttle_disable" : "throttle_enable");
    const runtime::BackendResult result = control_->throttle_result;
    if (result.ok)
        control_->snapshot.throttle_disabled = disabled;
    return result;
}

} // namespace savor::test_support
