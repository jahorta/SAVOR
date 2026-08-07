#pragma once

#include "../../../Core/Input/InputPlan.h"
#include "../IDolphinBackend.h"
#include "../RuntimeTypes.h"
#include "../StopPoints/StopPointTypes.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace savor::runtime {

struct ExecutionOperationIdTag;
struct InterruptionFrameIdTag;
struct InputAdvanceBindingIdTag;
struct InputLeaseIdTag;
struct InputPublicationTokenTag;

using ExecutionOperationId = StrongId<ExecutionOperationIdTag>;
using InterruptionFrameId = StrongId<InterruptionFrameIdTag>;
using InputAdvanceBindingId = StrongId<InputAdvanceBindingIdTag>;
using InputLeaseId = StrongId<InputLeaseIdTag>;
using InputPublicationToken = StrongId<InputPublicationTokenTag>;

static_assert(!std::is_convertible_v<ExecutionOperationId, InvocationId>);
static_assert(!std::is_convertible_v<InterruptionFrameId, ExecutionOperationId>);
static_assert(!std::is_convertible_v<InputAdvanceBindingId, InputPublicationToken>);
static_assert(!std::is_convertible_v<InputLeaseId, InputPublicationToken>);

struct InputPublicationEvidence
{
    InputLeaseId lease;
    InputPublicationToken publication;
    WorksetEpoch epoch;
    savor::GCInputFrame frame{};
};

enum class ExecutionOperationKind : std::uint8_t
{
    ContinueUntil,
    StepFrames,
    InputSynchronizedAdvance,
    SafePause,
    InteractiveResume,
};

enum class ExecutionActivity : std::uint8_t
{
    IdlePaused,
    Continuing,
    SteppingFrame,
    AdvancingInput,
    Pausing,
    InteractiveRunning,
    HandlingInterruption,
    Failed,
    Closed,
};

enum class ExecutionMovieState : std::uint8_t
{
    Inactive,
    Playing,
    Ended,
    Unknown,
};

enum class MovieEndedPolicy : std::uint8_t
{
    Ignore,
    Complete,
    Fail,
};

enum class ExecutionThrottlePolicy : std::uint8_t
{
    Preserve,
    RequireEnabled,
    RequireDisabled,
};

enum class ExecutionCurrentPointPolicy : std::uint8_t
{
    Ignore,
    AcceptIfAvailable,
    Require,
};

enum class ExecutionInterruptionPolicy : std::uint8_t
{
    Reject,
    AllowKnown,
};

struct ExecutionRequestPolicy
{
    WorksetEpoch expected_epoch;
    MovieEndedPolicy movie_ended = MovieEndedPolicy::Ignore;
    ExecutionThrottlePolicy throttle = ExecutionThrottlePolicy::Preserve;
    ExecutionCurrentPointPolicy current_point =
        ExecutionCurrentPointPolicy::Ignore;
    ExecutionInterruptionPolicy interruptions =
        ExecutionInterruptionPolicy::Reject;
    std::optional<InputAdvanceBindingId> input_relationship;
    CancellationToken cancellation;
};

struct ContinueUntilRequest
{
    ExecutionRequestPolicy policy;
    StopSubscriptionGroupDefinition wake_group;
    // When present, complete successfully once playback has consumed more
    // than this many complete DTM input records. Equality remains a valid
    // checkpoint boundary and may still be claimed by a routed stop.
    std::optional<std::uint64_t> expected_movie_input_count;
};

struct StepFramesRequest
{
    ExecutionRequestPolicy policy;
    std::uint32_t count = 1;
};

struct InputSynchronizedAdvanceRequest
{
    ExecutionRequestPolicy policy;
    InputAdvanceBindingId binding;
    std::uint32_t maximum_advances = 1;
};

struct SafePauseRequest
{
    ExecutionRequestPolicy policy;
    // Host-side pause confirmation remains bounded even though guest
    // execution is cancellation-driven.
    std::chrono::milliseconds confirmation_timeout{
        std::chrono::seconds(5)};
};

struct InteractiveResumeRequest
{
    WorksetEpoch expected_epoch;
    ExecutionThrottlePolicy throttle = ExecutionThrottlePolicy::Preserve;
    ExecutionInterruptionPolicy interruptions =
        ExecutionInterruptionPolicy::Reject;
    std::optional<InputAdvanceBindingId> input_relationship;
    CancellationToken cancellation;
};

using ExecutionRequest = std::variant<
    ContinueUntilRequest,
    StepFramesRequest,
    InputSynchronizedAdvanceRequest,
    SafePauseRequest,
    InteractiveResumeRequest>;

enum class ExecutionTerminalStatus : std::uint8_t
{
    RequestedCompletion,
    StepsCompleted,
    Paused,
    Cancelled,
    TimedOut,
    // Retains terminal numeric value 5 for WRMS v1 compatibility.
    CoreStalled,
    MovieEnded,
    ConsumedStop,
    UnexpectedStop,
    GuardFailed,
    InterruptionUnavailable,
    InterruptionAborted,
    InterruptionDepthExceeded,
    InterruptionFailed,
    WorksetEpochMismatch,
    Unsupported,
    BackendFailure,
    CleanupFailure,
    CursorOverrun,
};

enum class ExecutionErrorCode : std::uint16_t
{
    None,
    InvalidArgument,
    InvalidState,
    WrongThread,
    Busy,
    WorksetEpochMismatch,
    Unsupported,
    StopPointFailure,
    BackendFailure,
    InterruptionUnavailable,
    InterruptionPolicyViolation,
    InterruptionDepthExceeded,
    InputUnavailable,
    RuntimeStopping,
};

struct ExecutionError
{
    ExecutionErrorCode code = ExecutionErrorCode::None;
    std::string message;
    BackendIntegrity integrity = BackendIntegrity::Preserved;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return code != ExecutionErrorCode::None;
    }
};

struct ExecutionEnvironmentEvidence
{
    BackendCoreState core_state = BackendCoreState::Unknown;
    bool pause_confirmed = false;
    std::uint32_t pc = 0;
    std::uint64_t vi_count = 0;
    ExecutionMovieState movie_state = ExecutionMovieState::Unknown;
    std::uint64_t movie_input_count = 0;
    bool throttle_disabled = false;
};

struct ExecutionTerminalResult
{
    ExecutionOperationId operation_id;
    ExecutionOperationKind kind = ExecutionOperationKind::SafePause;
    ExecutionTerminalStatus status = ExecutionTerminalStatus::BackendFailure;
    WorksetEpoch workset_epoch;
    std::uint32_t completed_count = 0;
    ExecutionEnvironmentEvidence evidence;
    std::optional<StopRouteReceipt> stop;
    std::optional<InputPublicationEvidence> input_publication;
    ExecutionError error;
    BackendIntegrity integrity = BackendIntegrity::Preserved;
};

struct ExecutionSnapshot
{
    ExecutionActivity activity = ExecutionActivity::Closed;
    WorksetEpoch workset_epoch;
    std::optional<ExecutionOperationId> active_operation;
    ExecutionOperationKind active_kind = ExecutionOperationKind::SafePause;
    std::uint8_t interruption_depth = 0;
    std::optional<InterruptionFrameId> active_interruption_frame;
    bool input_bound = false;
    ExecutionEnvironmentEvidence evidence;
};

struct ExecutionProgress
{
    ExecutionOperationId operation_id;
    ExecutionOperationKind kind = ExecutionOperationKind::SafePause;
    std::uint32_t completed_count = 0;
    ExecutionEnvironmentEvidence evidence;
};

enum class ExecutionEventKind : std::uint8_t
{
    StateChanged,
    Progress,
    Terminal,
    HealthWarning,
};

enum class ExecutionHealthWarningKind : std::uint8_t
{
    SuspectedCoreStall,
    HostActivityLongRunning,
    HostActivityDiagnosticOverflow,
};

struct ExecutionHealthWarning
{
    ExecutionHealthWarningKind kind =
        ExecutionHealthWarningKind::SuspectedCoreStall;
    WorksetEpoch workset_epoch;
    std::optional<ExecutionOperationId> operation_id;
    std::chrono::milliseconds elapsed{};
    std::uint64_t host_activity_generation = 0;
    std::string code;
    std::string message;
};

struct ExecutionEvent
{
    ExecutionEventKind kind = ExecutionEventKind::StateChanged;
    ExecutionSnapshot snapshot;
    std::optional<ExecutionTerminalResult> terminal;
    std::optional<ExecutionProgress> progress;
    std::optional<ExecutionHealthWarning> health_warning;
};

struct ExecutionSubmissionReceipt
{
    bool accepted = false;
    ExecutionOperationId operation_id;
    ExecutionError error;
};

struct ExecutionControlReceipt
{
    bool accepted = false;
    ExecutionOperationId operation_id;
    ExecutionError error;
};

enum class InterruptionHandlerOutcome : std::uint8_t
{
    ResumeParent,
    AbortParent,
    InfrastructureFailure,
};

struct InterruptionHandlerDescriptor
{
    std::string key;
    std::vector<ExecutionOperationKind> allowed_child_operations;
    std::vector<std::string> permitted_nested_keys;
    bool allow_self_recursion = false;
    std::uint8_t maximum_depth = 1;
};

} // namespace savor::runtime
