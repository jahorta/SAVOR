#pragma once

#include "Runner/Runtime/Execution/IExecutionBackendPort.h"

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace savor::test_support {

struct FakeExecutionBackendControl
{
    mutable std::mutex mutex;
    std::optional<std::thread::id> owner_thread;
    bool owner_thread_violation = false;
    std::vector<std::string> calls;

    runtime::BackendExecutionCapabilityMask capabilities =
        runtime::BackendExecutionCapability::Pause |
        runtime::BackendExecutionCapability::Resume |
        runtime::BackendExecutionCapability::FrameStep |
        runtime::BackendExecutionCapability::ViObservation |
        runtime::BackendExecutionCapability::ThrottleControl;
    runtime::BackendExecutionSnapshot snapshot{
        .result = runtime::BackendResult::Success(),
        .core_state = runtime::BackendCoreState::Paused,
        .pause_confirmed = true,
    };
    runtime::BackendResult pause_result = runtime::BackendResult::Success();
    runtime::BackendResult resume_result = runtime::BackendResult::Success();
    runtime::BackendResult frame_step_result = runtime::BackendResult::Success();
    runtime::BackendResult throttle_result = runtime::BackendResult::Success();
    bool pause_changes_state = true;
    runtime::BackendHealthReport health{
        true,
        runtime::BackendCoreState::Paused,
        {}};
    std::deque<runtime::BackendExecutionSnapshot> queued_query_snapshots;
    std::deque<std::function<void()>> queued_query_callbacks;

    void SetCapabilities(runtime::BackendExecutionCapabilityMask value);
    void SetSnapshot(runtime::BackendExecutionSnapshot value);
    void SetCoreState(runtime::BackendCoreState value);
    void SetViCount(std::uint64_t value);
    void SetPauseResult(runtime::BackendResult value);
    void SetResumeResult(runtime::BackendResult value);
    void SetFrameStepResult(runtime::BackendResult value);
    void SetThrottleResult(runtime::BackendResult value);
    void SetPauseChangesState(bool value);
    void SetHealth(runtime::BackendHealthReport value);
    void QueueQuerySnapshot(runtime::BackendExecutionSnapshot value);
    void QueueQueryCallback(std::function<void()> callback);

    [[nodiscard]] std::vector<std::string> Calls() const;
    [[nodiscard]] runtime::BackendExecutionSnapshot Snapshot() const;
    [[nodiscard]] bool HasOwnerViolation() const;
    void RecordLocked(const char* call);
};

class FakeExecutionBackend final : public runtime::IExecutionBackendPort
{
public:
    explicit FakeExecutionBackend(
        std::shared_ptr<FakeExecutionBackendControl> control);

    [[nodiscard]] runtime::BackendExecutionCapabilityMask
    Capabilities() const noexcept override;
    [[nodiscard]] runtime::BackendExecutionSnapshot
    QueryExecutionSnapshot() const override;
    [[nodiscard]] runtime::BackendHealthReport CheckHealth() const override;
    runtime::BackendResult SubmitControlCommand(
        runtime::BackendControlCommand command) override;
    runtime::BackendResult SetThrottleDisabled(bool disabled) override;

private:
    std::shared_ptr<FakeExecutionBackendControl> control_;
};

} // namespace savor::test_support
