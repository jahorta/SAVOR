#pragma once

#include "../../../Core/Input/GCInputFrame.h"
#include "../IDolphinBackend.h"
#include "../RuntimeTypes.h"
#include "../Services/Movie/MovieTypes.h"
#include "../StopPoints/StopPointTypes.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace savor::runtime {

struct ExecutionOperationIdTag;
struct StopTransitionIdTag;
struct InterruptionFrameIdTag;
struct InputExecutionRelationshipIdTag;
struct InputExecutionBindingIdTag;
struct InputLeaseIdTag;
struct InputPublicationTokenTag;
struct InputDeliveryIdTag;

using ExecutionOperationId = StrongId<ExecutionOperationIdTag>;
using StopTransitionId = StrongId<StopTransitionIdTag>;
using InterruptionFrameId = StrongId<InterruptionFrameIdTag>;
using InputExecutionRelationshipId = StrongId<InputExecutionRelationshipIdTag>;
using InputExecutionBindingId = StrongId<InputExecutionBindingIdTag>;
using InputLeaseId = StrongId<InputLeaseIdTag>;
using InputPublicationToken = StrongId<InputPublicationTokenTag>;
using InputDeliveryId = StrongId<InputDeliveryIdTag>;

static_assert(!std::is_convertible_v<ExecutionOperationId, InvocationId>);
static_assert(!std::is_convertible_v<StopTransitionId, ExecutionOperationId>);

enum class ExecutionControlState : std::uint8_t
{
    PausedReady,
    PublishingRoute,
    Resuming,
    Running,
    StopObserved,
    ConfirmingPause,
    Completing,
    FrameStepping,
    Interrupting,
    Stopping,
    Failed,
};
static_assert(!std::is_convertible_v<InterruptionFrameId, ExecutionOperationId>);
static_assert(!std::is_convertible_v<InputExecutionRelationshipId, InputExecutionBindingId>);
static_assert(!std::is_convertible_v<InputExecutionBindingId, InputPublicationToken>);
static_assert(!std::is_convertible_v<InputLeaseId, InputPublicationToken>);

struct InputExecutionBindingEvidence
{
    InputLeaseId lease;
    InputExecutionBindingId binding;
    InputPublicationToken publication;
    WorksetEpoch epoch;
    std::uint64_t state_generation = 0;
    savor::GCInputFrame frame{};
};

enum class ExecutionOperationKind : std::uint8_t
{
    ContinueUntil,
    StepFrames,
    ContinueUntilInputObserved,
    SafePause,
    InteractiveResume,
};

enum class ExecutionActivity : std::uint8_t
{
    IdlePaused,
    Continuing,
    SteppingFrame,
    Pausing,
    InteractiveRunning,
    HandlingInterruption,
    Failed,
    Closed,
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
    ExecutionThrottlePolicy throttle = ExecutionThrottlePolicy::RequireDisabled;
    ExecutionCurrentPointPolicy current_point =
        ExecutionCurrentPointPolicy::Ignore;
    ExecutionInterruptionPolicy interruptions =
        ExecutionInterruptionPolicy::Reject;
    std::optional<InputExecutionRelationshipId> input_relationship;
    CancellationToken cancellation;
    // Worker-local action correlation used only for diagnostics. These are
    // never encoded in a program, workset, or wire contract.
    std::uint64_t diagnostic_invocation = 0;
    std::uint64_t diagnostic_attempt = 0;
    std::uint64_t diagnostic_request = 0;
    std::string diagnostic_selector;
};

struct ContinueUntilRequest
{
    ExecutionRequestPolicy policy;
    StopSubscriptionGroupDefinition wake_group;
    // When present, complete successfully once playback has consumed more
    // than this many complete DTM input records. Equality remains a valid
    // checkpoint boundary and may still be claimed by a routed stop.
    std::optional<std::uint64_t> expected_movie_input_count;
    bool movie_end_only = false;
};

struct StepFramesRequest
{
    ExecutionRequestPolicy policy;
    std::uint32_t count = 1;
};

// Runs without a frame/time bound until the exact bound input publication is
// polled. The terminal pause must still prove that recording advanced beyond
// the caller's checkpoint cursor.
struct ContinueUntilInputObservedRequest
{
    ExecutionRequestPolicy policy;
    std::uint64_t expected_movie_input_count = 0;
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
    ExecutionThrottlePolicy throttle = ExecutionThrottlePolicy::RequireDisabled;
    ExecutionInterruptionPolicy interruptions =
        ExecutionInterruptionPolicy::Reject;
    std::optional<InputExecutionRelationshipId> input_relationship;
    CancellationToken cancellation;
};

using ExecutionRequest = std::variant<
    ContinueUntilRequest,
    StepFramesRequest,
    ContinueUntilInputObservedRequest,
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
    UnexpectedStop,
    InterruptionUnavailable,
    InterruptionAborted,
    InterruptionDepthExceeded,
    InterruptionFailed,
    WorksetEpochMismatch,
    Unsupported,
    BackendFailure,
    CleanupFailure,
    CursorOverrun,
    InputObserved,
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
    bool paused_quiescent = false;
    std::uint32_t pc = 0;
    std::uint64_t vi_count = 0;
    MovieState movie_state = MovieState::Unknown;
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
    ExecutionError error;
    BackendIntegrity integrity = BackendIntegrity::Preserved;
    StopTransitionId stop_transition;
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
    ExecutionControlState control_state = ExecutionControlState::Failed;
    StopTransitionId stop_transition;
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
