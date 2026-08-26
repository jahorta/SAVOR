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
    snapshot.paused_quiescent =
        value == runtime::BackendCoreState::Paused;
}

void FakeExecutionBackendControl::SetViCount(std::uint64_t value)
{
    std::lock_guard lock(mutex);
    snapshot.vi_count = value;
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

void FakeExecutionBackendControl::SetThrottleResult(runtime::BackendResult value)
{
    std::lock_guard lock(mutex);
    throttle_result = std::move(value);
}

void FakeExecutionBackendControl::SetPauseChangesState(bool value)
{
    std::lock_guard lock(mutex);
    pause_changes_state = value;
}

void FakeExecutionBackendControl::SetHealth(
    runtime::BackendHealthReport value)
{
    std::lock_guard lock(mutex);
    health = std::move(value);
}

void FakeExecutionBackendControl::QueueQuerySnapshot(
    runtime::BackendExecutionSnapshot value)
{
    std::lock_guard lock(mutex);
    queued_query_snapshots.push_back(std::move(value));
}

void FakeExecutionBackendControl::QueueQueryCallback(
    std::function<void()> callback)
{
    std::lock_guard lock(mutex);
    queued_query_callbacks.push_back(std::move(callback));
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
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("query");
        if (!control_->queued_query_snapshots.empty())
        {
            control_->snapshot =
                std::move(control_->queued_query_snapshots.front());
            control_->queued_query_snapshots.pop_front();
        }
        if (control_->queued_query_callbacks.empty())
            return control_->snapshot;
        std::function<void()> callback =
            std::move(control_->queued_query_callbacks.front());
        control_->queued_query_callbacks.pop_front();
        const runtime::BackendExecutionSnapshot snapshot =
            control_->snapshot;
        callback();
        return snapshot;
    }
}

runtime::BackendHealthReport FakeExecutionBackend::CheckHealth() const
{
    std::lock_guard lock(control_->mutex);
    control_->RecordLocked("check_health");
    return control_->health;
}

runtime::BackendResult FakeExecutionBackend::SubmitControlTask(
    runtime::BackendControlTask task)
{
    std::lock_guard lock(control_->mutex);
    if (control_->control_task_state !=
        runtime::BackendExecutionSnapshot::ControlTaskState::Idle)
    {
        return runtime::BackendResult::Failure(
            runtime::BackendErrorCode::InvalidState,
            "fake execution actuator is busy");
    }
    control_->control_task_state =
        runtime::BackendExecutionSnapshot::ControlTaskState::Running;
    runtime::BackendResult result;
    switch (task.kind)
    {
    case runtime::BackendControlTaskKind::Pause:
        control_->RecordLocked("pause");
        result = control_->pause_result;
        if (result.ok && control_->pause_changes_state)
        {
            control_->snapshot.core_state = runtime::BackendCoreState::Paused;
            control_->snapshot.paused_quiescent = true;
        }
        break;
    case runtime::BackendControlTaskKind::Resume:
        control_->RecordLocked("resume");
        result = control_->resume_result;
        if (result.ok)
        {
            control_->snapshot.core_state = runtime::BackendCoreState::Running;
            control_->snapshot.paused_quiescent = false;
        }
        break;
    case runtime::BackendControlTaskKind::FrameStep:
        control_->RecordLocked("frame_step");
        result = control_->frame_step_result;
        if (result.ok)
        {
            control_->snapshot.core_state = runtime::BackendCoreState::Paused;
            control_->snapshot.paused_quiescent = true;
            ++control_->snapshot.vi_count;
        }
        break;
    case runtime::BackendControlTaskKind::SynchronizePaused:
        control_->RecordLocked("synchronize_paused");
        result = control_->synchronize_result;
        if (result.ok &&
            control_->snapshot.core_state == runtime::BackendCoreState::Paused)
        {
            control_->snapshot.paused_quiescent = true;
        }
        break;
    }
    control_->control_completion = runtime::BackendControlCompletion{
        task.kind,
        result};
    control_->control_task_state =
        runtime::BackendExecutionSnapshot::ControlTaskState::Completed;
    control_->snapshot.control_task_state = control_->control_task_state;
    return runtime::BackendResult::Success();
}

std::optional<runtime::BackendControlCompletion>
FakeExecutionBackend::TakeControlCompletion()
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
    control_->snapshot.control_task_state = control_->control_task_state;
    return completion;
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
