#pragma once

#include "ExecutionTypes.h"
#include "../IDolphinBackend.h"

#include <cstdint>
#include <optional>

namespace savor::runtime {

enum class BackendExecutionCapability : std::uint32_t
{
    None = 0,
    Pause = 1u << 0,
    Resume = 1u << 1,
    FrameStep = 1u << 2,
    ViObservation = 1u << 4,
    ThrottleControl = 1u << 6,
};

using BackendExecutionCapabilityMask = std::uint32_t;

[[nodiscard]] constexpr BackendExecutionCapabilityMask ExecutionCapabilityMask(
    BackendExecutionCapability capability) noexcept
{
    return static_cast<BackendExecutionCapabilityMask>(capability);
}

[[nodiscard]] constexpr BackendExecutionCapabilityMask operator|(
    BackendExecutionCapability lhs,
    BackendExecutionCapability rhs) noexcept
{
    return ExecutionCapabilityMask(lhs) | ExecutionCapabilityMask(rhs);
}

[[nodiscard]] constexpr BackendExecutionCapabilityMask operator|(
    BackendExecutionCapabilityMask lhs,
    BackendExecutionCapability rhs) noexcept
{
    return lhs | ExecutionCapabilityMask(rhs);
}

[[nodiscard]] constexpr bool HasExecutionCapability(
    BackendExecutionCapabilityMask capabilities,
    BackendExecutionCapability capability) noexcept
{
    return (capabilities & ExecutionCapabilityMask(capability)) ==
        ExecutionCapabilityMask(capability);
}

struct BackendExecutionSnapshot
{
    BackendResult result;
    BackendCoreState core_state = BackendCoreState::Unknown;
    // Paused is only authoritative after the backend has observed the CPU
    // thread leave its run loop. Core::GetState(Paused) alone is not enough.
    bool paused_quiescent = false;
    std::uint32_t pc = 0;
    std::uint64_t vi_count = 0;
    bool throttle_disabled = false;
    enum class ControlTaskState : std::uint8_t
    {
        Idle,
        Pending,
        Running,
        Completed,
        Stopping,
    } control_task_state = ControlTaskState::Idle;
};

enum class BackendControlTaskKind : std::uint8_t
{
    Pause,
    Resume,
    FrameStep,
    SynchronizePaused,
};

struct BackendControlTask
{
    BackendControlTaskKind kind = BackendControlTaskKind::Pause;
};

struct BackendControlCompletion
{
    BackendControlTaskKind kind = BackendControlTaskKind::Pause;
    BackendResult result;
    BackendCoreState resulting_core_state = BackendCoreState::Unknown;
    std::uint32_t pc = 0;
    std::uint64_t vi_count = 0;
};

class IExecutionBackendPort
{
public:
    virtual ~IExecutionBackendPort() = default;

    IExecutionBackendPort(const IExecutionBackendPort&) = delete;
    IExecutionBackendPort& operator=(const IExecutionBackendPort&) = delete;

    [[nodiscard]] virtual BackendExecutionCapabilityMask
    Capabilities() const noexcept = 0;
    [[nodiscard]] virtual BackendExecutionSnapshot
    QueryExecutionSnapshot() const = 0;
    [[nodiscard]] virtual BackendHealthReport CheckHealth() const = 0;

    virtual BackendResult SubmitControlTask(BackendControlTask task) = 0;
    virtual BackendResult PumpControlTask() = 0;
    [[nodiscard]] virtual std::optional<BackendControlCompletion>
    TakeControlCompletion() = 0;
    virtual BackendResult SetThrottleDisabled(bool disabled) = 0;

protected:
    IExecutionBackendPort() = default;
};

} // namespace savor::runtime
