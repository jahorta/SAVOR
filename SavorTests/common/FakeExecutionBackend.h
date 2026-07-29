#pragma once

#include "Runner/Runtime/Execution/IExecutionBackendPort.h"

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
        runtime::BackendExecutionCapability::MovieObservation |
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

    void SetCapabilities(runtime::BackendExecutionCapabilityMask value);
    void SetSnapshot(runtime::BackendExecutionSnapshot value);
    void SetCoreState(runtime::BackendCoreState value);
    void SetViCount(std::uint64_t value);
    void SetMovieState(runtime::BackendMovieState value);
    void SetPauseResult(runtime::BackendResult value);
    void SetResumeResult(runtime::BackendResult value);
    void SetFrameStepResult(runtime::BackendResult value);
    void SetThrottleResult(runtime::BackendResult value);

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
    runtime::BackendResult RequestPause() override;
    runtime::BackendResult Resume() override;
    runtime::BackendResult BeginFrameStep() override;
    runtime::BackendResult SetThrottleDisabled(bool disabled) override;

private:
    std::shared_ptr<FakeExecutionBackendControl> control_;
};

} // namespace savor::test_support
