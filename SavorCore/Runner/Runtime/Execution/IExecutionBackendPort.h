#pragma once

#include "../IDolphinBackend.h"

#include <cstdint>

namespace savor::runtime {

enum class BackendExecutionCapability : std::uint32_t
{
    None = 0,
    Pause = 1u << 0,
    Resume = 1u << 1,
    FrameStep = 1u << 2,
    ExactInstructionStep = 1u << 3,
    ViObservation = 1u << 4,
    MovieObservation = 1u << 5,
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

enum class BackendMovieState : std::uint8_t
{
    Inactive,
    Playing,
    Ended,
    Unknown,
};

struct BackendExecutionSnapshot
{
    BackendResult result;
    BackendCoreState core_state = BackendCoreState::Unknown;
    // Paused is only authoritative after the backend has observed the CPU
    // thread leave its run loop. Core::GetState(Paused) alone is not enough.
    bool pause_confirmed = false;
    std::uint32_t pc = 0;
    std::uint64_t vi_count = 0;
    BackendMovieState movie_state = BackendMovieState::Unknown;
    std::uint64_t movie_input_count = 0;
    bool throttle_disabled = false;
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

    virtual BackendResult RequestPause() = 0;
    virtual BackendResult Resume() = 0;
    virtual BackendResult BeginFrameStep() = 0;
    virtual BackendResult BeginExactInstructionStep() = 0;
    virtual BackendResult SetThrottleDisabled(bool disabled) = 0;

protected:
    IExecutionBackendPort() = default;
};

} // namespace savor::runtime
