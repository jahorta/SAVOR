#pragma once

#include <atomic>
#include <compare>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace savor::runtime {

template <typename Tag>
class StrongId final
{
public:
    using value_type = std::uint64_t;

    constexpr StrongId() noexcept = default;
    explicit constexpr StrongId(value_type value) noexcept
        : value_(value)
    {
    }

    [[nodiscard]] constexpr value_type value() const noexcept { return value_; }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return value_ != 0; }

    auto operator<=>(const StrongId&) const noexcept = default;

private:
    value_type value_ = 0;
};

struct WireRequestIdTag;
struct WorkerCommandSequenceTag;
struct HostEventSequenceTag;
struct SessionIdTag;
struct InvocationIdTag;
struct AttemptIdTag;
struct StateEpochTag;

using WireRequestId = StrongId<WireRequestIdTag>;
using WorkerCommandSequence = StrongId<WorkerCommandSequenceTag>;
using HostEventSequence = StrongId<HostEventSequenceTag>;
using SessionId = StrongId<SessionIdTag>;
using InvocationId = StrongId<InvocationIdTag>;
using AttemptId = StrongId<AttemptIdTag>;
using StateEpoch = StrongId<StateEpochTag>;

static_assert(!std::is_convertible_v<StateEpoch, WorkerCommandSequence>);
static_assert(!std::is_convertible_v<WorkerCommandSequence, StateEpoch>);
static_assert(!std::is_convertible_v<InvocationId, AttemptId>);

enum class WorkerCapability : std::uint64_t
{
    None = 0,
    SessionLifecycle = 1ull << 0,
    Screenshot = 1ull << 1,
    HostEvents = 1ull << 2,
    CancellationProtocol = 1ull << 3,
    Shutdown = 1ull << 4,
    ProgramInvocation = 1ull << 5,
    InteractiveVisualDebug = 1ull << 6,
};

using WorkerCapabilityMask = std::uint64_t;

[[nodiscard]] constexpr WorkerCapabilityMask CapabilityMask(WorkerCapability capability) noexcept
{
    return static_cast<WorkerCapabilityMask>(capability);
}

[[nodiscard]] constexpr WorkerCapabilityMask operator|(
    WorkerCapability lhs,
    WorkerCapability rhs) noexcept
{
    return CapabilityMask(lhs) | CapabilityMask(rhs);
}

[[nodiscard]] constexpr WorkerCapabilityMask operator|(
    WorkerCapabilityMask lhs,
    WorkerCapability rhs) noexcept
{
    return lhs | CapabilityMask(rhs);
}

[[nodiscard]] constexpr WorkerCapabilityMask operator|(
    WorkerCapability lhs,
    WorkerCapabilityMask rhs) noexcept
{
    return CapabilityMask(lhs) | rhs;
}

[[nodiscard]] constexpr WorkerCapabilityMask AddCapability(
    WorkerCapabilityMask capabilities,
    WorkerCapability capability) noexcept
{
    return capabilities | CapabilityMask(capability);
}

[[nodiscard]] constexpr WorkerCapabilityMask RemoveCapability(
    WorkerCapabilityMask capabilities,
    WorkerCapability capability) noexcept
{
    return capabilities & ~CapabilityMask(capability);
}

[[nodiscard]] constexpr bool HasCapability(
    WorkerCapabilityMask capabilities,
    WorkerCapability capability) noexcept
{
    return (capabilities & CapabilityMask(capability)) == CapabilityMask(capability);
}

inline constexpr WorkerCapabilityMask kSlice1ProductionCapabilities =
    WorkerCapability::SessionLifecycle |
    WorkerCapability::Screenshot |
    WorkerCapability::HostEvents |
    WorkerCapability::CancellationProtocol |
    WorkerCapability::Shutdown;

enum class WorkerState : std::uint8_t
{
    Starting,
    AwaitingSession,
    Ready,
    Running,
    Cancelling,
    Tainted,
    Stopping,
    Stopped,
};

enum class SessionDisposition : std::uint8_t
{
    Closed,
    Clean,
    CleanWithDiagnostics,
    Tainted,
};

enum class WorkerCommandKind : std::uint8_t
{
    OpenSession,
    PrepareModule,
    InvokeProgram,
    CancelInvocation,
    CaptureScreenshot,
    Shutdown,
};

enum class WorkerCommandOutcome : std::uint8_t
{
    Accepted,
    Completed,
    Rejected,
};

enum class WorkerRejectionCode : std::uint16_t
{
    None,
    Unsupported,
    InvalidState,
    InvalidArgument,
    SessionUnavailable,
    SessionMismatch,
    SessionTainted,
    ProgramRuntimeUnavailable,
    InvocationAlreadyActive,
    InvocationNotActive,
    InvocationMismatch,
    DuplicateCancellation,
    StateEpochMismatch,
    BackendFailure,
    RuntimeStopping,
    InternalFailure,
};

enum class InvocationTerminalStatus : std::uint8_t
{
    Completed,
    Cancelled,
    TimedOut,
    Failed,
    InfrastructureFailure,
    CleanupFailure,
};

enum class CleanupStatus : std::uint8_t
{
    Clean,
    CleanWithDiagnostics,
    Failed,
};

enum class CancellationReason : std::uint8_t
{
    None,
    ExternalRequest,
    Shutdown,
    Deadline,
    RuntimeFailure,
};

struct RuntimeError
{
    WorkerRejectionCode code = WorkerRejectionCode::None;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return code != WorkerRejectionCode::None;
    }
};

namespace detail {

struct CancellationState
{
    explicit CancellationState(InvocationId invocation_id)
        : invocation(invocation_id)
    {
    }

    InvocationId invocation;
    std::atomic<CancellationReason> reason{CancellationReason::None};
};

} // namespace detail

class CancellationToken final
{
public:
    CancellationToken() = default;

    [[nodiscard]] InvocationId invocation_id() const noexcept
    {
        return state_ ? state_->invocation : InvocationId{};
    }

    [[nodiscard]] bool is_cancellation_requested() const noexcept
    {
        return reason() != CancellationReason::None;
    }

    [[nodiscard]] CancellationReason reason() const noexcept
    {
        return state_
            ? state_->reason.load(std::memory_order_acquire)
            : CancellationReason::None;
    }

private:
    explicit CancellationToken(std::shared_ptr<const detail::CancellationState> state)
        : state_(std::move(state))
    {
    }

    std::shared_ptr<const detail::CancellationState> state_;

    friend class CancellationSource;
};

class CancellationSource final
{
public:
    explicit CancellationSource(InvocationId invocation_id)
        : state_(std::make_shared<detail::CancellationState>(invocation_id))
    {
    }

    CancellationSource(const CancellationSource&) = delete;
    CancellationSource& operator=(const CancellationSource&) = delete;
    CancellationSource(CancellationSource&&) noexcept = default;
    CancellationSource& operator=(CancellationSource&&) noexcept = default;

    [[nodiscard]] CancellationToken token() const noexcept
    {
        return CancellationToken(state_);
    }

    [[nodiscard]] InvocationId invocation_id() const noexcept
    {
        return state_ ? state_->invocation : InvocationId{};
    }

    [[nodiscard]] bool request_cancellation(CancellationReason reason) noexcept
    {
        if (!state_ || reason == CancellationReason::None)
            return false;

        CancellationReason expected = CancellationReason::None;
        return state_->reason.compare_exchange_strong(
                expected,
                reason,
                std::memory_order_acq_rel,
                std::memory_order_acquire);
    }

    [[nodiscard]] bool is_cancellation_requested() const noexcept
    {
        return state_ &&
            state_->reason.load(std::memory_order_acquire) !=
                CancellationReason::None;
    }

private:
    std::shared_ptr<detail::CancellationState> state_;
};

} // namespace savor::runtime
