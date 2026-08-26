#include "ExecutionControlCore.h"

#include "../../../../SavorProbe/BreakpointDiagnostics.h"

#include <array>
#include <atomic>

#include "../Services/Movie/MovieService.h"
#include "../../../Utils/Log.h"

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <thread>
#include <type_traits>
#include <utility>

namespace savor::runtime {

namespace {

std::atomic<std::uint64_t> g_control_generation{1};

[[nodiscard]] const char* ControlCommandKindName(
    BackendControlCommandKind kind) noexcept
{
    switch (kind)
    {
    case BackendControlCommandKind::Pause:
        return "Pause";
    case BackendControlCommandKind::Resume:
        return "Resume";
    case BackendControlCommandKind::FrameStep:
        return "FrameStep";
    }
    return "Unknown";
}

} // namespace
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] const char* KindName(ExecutionOperationKind kind) noexcept
{
    switch (kind)
    {
    case ExecutionOperationKind::ContinueUntil: return "continue_until";
    case ExecutionOperationKind::StepFrames: return "step_frames";
    case ExecutionOperationKind::ContinueUntilInputObserved:
        return "continue_until_input_observed";
    case ExecutionOperationKind::SafePause: return "safe_pause";
    case ExecutionOperationKind::InteractiveResume: return "interactive_resume";
    }
    return "unknown";
}

[[nodiscard]] const char* TerminalName(ExecutionTerminalStatus status) noexcept
{
    switch (status)
    {
    case ExecutionTerminalStatus::RequestedCompletion: return "requested_completion";
    case ExecutionTerminalStatus::StepsCompleted: return "steps_completed";
    case ExecutionTerminalStatus::Paused: return "paused";
    case ExecutionTerminalStatus::Cancelled: return "cancelled";
    case ExecutionTerminalStatus::TimedOut: return "timed_out";
    case ExecutionTerminalStatus::CoreStalled: return "core_stalled";
    case ExecutionTerminalStatus::MovieEnded: return "movie_ended";
    case ExecutionTerminalStatus::UnexpectedStop: return "unexpected_stop";
    case ExecutionTerminalStatus::InterruptionUnavailable: return "interruption_unavailable";
    case ExecutionTerminalStatus::InterruptionAborted: return "interruption_aborted";
    case ExecutionTerminalStatus::InterruptionDepthExceeded: return "interruption_depth_exceeded";
    case ExecutionTerminalStatus::InterruptionFailed: return "interruption_failed";
    case ExecutionTerminalStatus::WorksetEpochMismatch: return "epoch_mismatch";
    case ExecutionTerminalStatus::Unsupported: return "unsupported";
    case ExecutionTerminalStatus::BackendFailure: return "backend_failure";
    case ExecutionTerminalStatus::CleanupFailure: return "cleanup_failure";
    case ExecutionTerminalStatus::CursorOverrun: return "cursor_overrun";
    case ExecutionTerminalStatus::InputObserved: return "input_observed";
    }
    return "unknown";
}

[[nodiscard]] std::string AwaitedPcs(const ExecutionRequest& request)
{
    const auto* until = std::get_if<ContinueUntilRequest>(&request);
    if (!until)
        return "[]";
    std::string output = "[";
    bool first = true;
    for (const StopSubscriptionDefinition& subscription :
         until->wake_group.subscriptions)
    {
        const auto* pc = std::get_if<PcStopPointSpec>(&subscription.point);
        if (!pc)
            continue;
        char encoded[16]{};
        std::snprintf(encoded, sizeof(encoded), "0x%08X", pc->pc);
        if (!first)
            output.push_back(',');
        output.append(encoded);
        first = false;
    }
    output.push_back(']');
    return output;
}

[[nodiscard]] ExecutionOperationKind KindOf(const ExecutionRequest& request)
{
    return std::visit(
        [](const auto& value) {
            using Request = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Request, ContinueUntilRequest>)
                return ExecutionOperationKind::ContinueUntil;
            if constexpr (std::is_same_v<Request, StepFramesRequest>)
                return ExecutionOperationKind::StepFrames;
            if constexpr (std::is_same_v<
                              Request,
                              ContinueUntilInputObservedRequest>)
                return ExecutionOperationKind::ContinueUntilInputObserved;
            if constexpr (std::is_same_v<Request, SafePauseRequest>)
                return ExecutionOperationKind::SafePause;
            return ExecutionOperationKind::InteractiveResume;
        },
        request);
}

[[nodiscard]] const ExecutionRequestPolicy* PolicyOf(
    const ExecutionRequest& request)
{
    return std::visit(
        [](const auto& value) -> const ExecutionRequestPolicy* {
            using Request = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Request, InteractiveResumeRequest>)
                return nullptr;
            else
                return &value.policy;
        },
        request);
}

[[nodiscard]] WorksetEpoch EpochOf(const ExecutionRequest& request)
{
    return std::visit(
        [](const auto& value) {
            using Request = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Request, InteractiveResumeRequest>)
                return value.expected_epoch;
            else
                return value.policy.expected_epoch;
        },
        request);
}

[[nodiscard]] ExecutionThrottlePolicy ThrottleOf(
    const ExecutionRequest& request)
{
    return std::visit(
        [](const auto& value) {
            using Request = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Request, InteractiveResumeRequest>)
                return value.throttle;
            else
                return value.policy.throttle;
        },
        request);
}

[[nodiscard]] ExecutionInterruptionPolicy InterruptionPolicyOf(
    const ExecutionRequest& request)
{
    return std::visit(
        [](const auto& value) {
            using Request = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Request, InteractiveResumeRequest>)
                return value.interruptions;
            else
                return value.policy.interruptions;
        },
        request);
}

[[nodiscard]] CancellationToken CancellationOf(const ExecutionRequest& request)
{
    return std::visit(
        [](const auto& value) {
            using Request = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Request, InteractiveResumeRequest>)
                return value.cancellation;
            else
                return value.policy.cancellation;
        },
        request);
}

[[nodiscard]] std::optional<InputExecutionRelationshipId> InputRelationshipOf(
    const ExecutionRequest& request)
{
    return std::visit(
        [](const auto& value) {
            using Request = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Request, InteractiveResumeRequest>)
                return value.input_relationship;
            else
                return value.policy.input_relationship;
        },
        request);
}

struct ExecutionObservation : BackendExecutionSnapshot
{
    MovieState movie_state = MovieState::Unknown;
    std::uint64_t movie_input_count = 0;
};

[[nodiscard]] ExecutionEnvironmentEvidence ConvertEvidence(
    const ExecutionObservation& snapshot)
{
    return {
        snapshot.core_state,
        snapshot.pause_confirmed,
        snapshot.pc,
        snapshot.vi_count,
        snapshot.movie_state,
        snapshot.movie_input_count,
        snapshot.throttle_disabled,
    };
}

[[nodiscard]] ExecutionError Error(
    ExecutionErrorCode code,
    std::string message,
    BackendIntegrity integrity = BackendIntegrity::Preserved)
{
    return {code, std::move(message), integrity};
}

[[nodiscard]] ExecutionError BackendError(
    std::string prefix,
    const BackendResult& backend)
{
    if (!backend.message.empty())
    {
        prefix += ": ";
        prefix += backend.message;
    }
    return Error(
        backend.code == BackendErrorCode::Unavailable
            ? ExecutionErrorCode::Unsupported
            : ExecutionErrorCode::BackendFailure,
        std::move(prefix),
        backend.integrity);
}

[[nodiscard]] bool ContainsKind(
    const std::vector<ExecutionOperationKind>& kinds,
    ExecutionOperationKind kind)
{
    return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

} // namespace

struct ExecutionControlCore::Impl
{
    struct ControlTransitionDiagnostic
    {
        std::uint64_t sequence = 0;
        ExecutionControlGeneration generation;
        BackendControlCommandKind kind = BackendControlCommandKind::Pause;
        Clock::time_point submitted;
        bool ok = false;
        std::string source;
        std::string error;
    };

    struct ActiveOperation
    {
        ExecutionOperationId id;
        ExecutionRequest request;
        ExecutionOperationKind kind = ExecutionOperationKind::SafePause;
        Clock::time_point started;
        Clock::time_point next_diagnostic_heartbeat;
        std::uint64_t start_movie_input_count = 0;
        Clock::time_point health_baseline;
        std::uint64_t health_last_vi = 0;
        std::uint64_t health_host_generation = 0;
        bool health_eligible = false;
        bool stall_suspected = false;
        std::uint32_t host_activity_sequence = 0;
        std::chrono::milliseconds host_activity_warned_through{};
        std::uint64_t advance_baseline_vi = 0;
        std::uint32_t completed_count = 0;
        bool observed_running = false;
        bool awaiting_advance = false;
        bool no_progress_pause_logged = false;
        bool throttle_changed = false;
        bool original_throttle_disabled = false;
        std::optional<StopSubscriptionGroupHandle> wake_group;
        std::optional<ExecutionTerminalStatus> pending_terminal;
        std::optional<ExecutionError> pending_error;
        std::optional<StopRouteReceipt> pending_stop;
        std::optional<InterruptionFrameId> handler_owner;
        std::optional<ExecutionOperationId> pause_control_id;
        std::optional<Clock::time_point> pause_control_deadline;
        std::optional<Clock::time_point>
            unconfirmed_pause_deadline;
        bool wake_group_parked = false;
    };

    struct SuspendedFrame
    {
        InterruptionFrameId id;
        InterruptionHandlerDescriptor descriptor;
        ActiveOperation parent;
        bool pause_confirmed = false;
        std::optional<Clock::time_point> pause_confirmation_deadline;
    };

    struct PendingParentTerminal
    {
        ActiveOperation operation;
        ExecutionTerminalStatus status =
            ExecutionTerminalStatus::Cancelled;
        ExecutionError error;
    };

    IExecutionBackendPort& backend;
    MovieService& movies;
    StopPointRouter& stop_points;
    ExecutionControlCoreConfig config;
    HostActivityTracker owned_host_activity;
    HostActivityTracker* host_activity = nullptr;
    std::function<Clock::time_point()> now;
    std::thread::id owner_thread;
    WorksetEpoch epoch;
    ExecutionSnapshot snapshot;
    std::optional<ActiveOperation> active;
    std::vector<SuspendedFrame> handlers;
    std::vector<PendingParentTerminal> pending_parent_terminals;
    std::map<std::string, InterruptionHandlerDescriptor, std::less<>>
        handler_registry;
    std::vector<ExecutionEvent> events;
    std::array<ControlTransitionDiagnostic, 8> control_history{};
    std::size_t control_history_size = 0;
    std::size_t next_control_history = 0;
    std::uint64_t next_control_sequence = 1;
    std::uint64_t next_unrouted_pause_incident = 1;
    std::uint64_t next_operation = 1;
    std::uint64_t next_frame = 1;
    bool initialized = false;
    bool stopping = false;

    Impl(
        IExecutionBackendPort& backend_value,
        MovieService& movies_value,
        StopPointRouter& stop_points_value,
        ExecutionControlCoreConfig config_value)
        : backend(backend_value),
          movies(movies_value),
          stop_points(stop_points_value),
          config(std::move(config_value)),
          now(config.now
                  ? config.now
                  : [] { return Clock::now(); }),
          owner_thread(std::this_thread::get_id())
    {
        host_activity = config.host_activity
            ? config.host_activity
            : &owned_host_activity;
        if (config.maintenance_interval <= std::chrono::milliseconds::zero())
            config.maintenance_interval = std::chrono::milliseconds(10);
        if (config.pause_confirmation_timeout <=
            std::chrono::milliseconds::zero())
        {
            config.pause_confirmation_timeout = std::chrono::seconds(5);
        }
        if (config.suspect_core_stall_after <=
            std::chrono::milliseconds::zero())
        {
            config.suspect_core_stall_after = std::chrono::seconds(10);
        }
        if (config.confirm_core_stall_after <=
            std::chrono::milliseconds::zero())
        {
            config.confirm_core_stall_after = std::chrono::seconds(10);
        }
        if (config.host_activity_warning_after <=
            std::chrono::milliseconds::zero())
        {
            config.host_activity_warning_after = std::chrono::seconds(10);
        }
        if (config.host_activity_warning_repeat <=
            std::chrono::milliseconds::zero())
        {
            config.host_activity_warning_repeat = std::chrono::seconds(30);
        }
        for (InterruptionHandlerDescriptor& descriptor :
            config.interruption_handlers)
        {
            if (!descriptor.key.empty() &&
                descriptor.maximum_depth > 0 &&
                descriptor.maximum_depth <= 8)
            {
                handler_registry.emplace(descriptor.key, std::move(descriptor));
            }
        }
        snapshot.activity = ExecutionActivity::Closed;
    }

    [[nodiscard]] bool OnOwnerThread() const noexcept
    {
        return owner_thread == std::this_thread::get_id();
    }

    [[nodiscard]] static const ExecutionRequestPolicy* DiagnosticPolicy(
        const ActiveOperation& operation) noexcept
    {
        return PolicyOf(operation.request);
    }

    void LogOperationStart(
        const ActiveOperation& operation,
        const ExecutionObservation& observed) const
    {
        if (operation.kind != ExecutionOperationKind::ContinueUntil &&
            operation.kind != ExecutionOperationKind::StepFrames &&
            operation.kind !=
                ExecutionOperationKind::ContinueUntilInputObserved)
            return;
        const ExecutionRequestPolicy* policy = DiagnosticPolicy(operation);
        const auto relationship = InputRelationshipOf(operation.request);
        SCLOGDX(
            SC_TAGS("execution.operation", "execution.begin"),
            "operation=%llu kind=%s epoch=%llu invocation=%llu attempt=%llu request=%llu selector=%s awaited_pcs=%s input_relationship=%llu core_state=%u pc=0x%08X vi=%llu movie_state=%u recording_input_count=%llu",
            operation.id.value(), KindName(operation.kind), epoch.value(),
            policy ? policy->diagnostic_invocation : 0,
            policy ? policy->diagnostic_attempt : 0,
            policy ? policy->diagnostic_request : 0,
            policy && !policy->diagnostic_selector.empty()
                ? policy->diagnostic_selector.c_str()
                : "<unnamed>",
            AwaitedPcs(operation.request).c_str(),
            relationship ? relationship->value() : 0,
            static_cast<unsigned>(observed.core_state), observed.pc,
            observed.vi_count, static_cast<unsigned>(observed.movie_state),
            observed.movie_input_count);
    }

    void LogHeartbeat(
        ActiveOperation& operation,
        const ExecutionObservation& observed,
        Clock::time_point current) const
    {
        if (current < operation.next_diagnostic_heartbeat)
            return;
        operation.next_diagnostic_heartbeat = current + std::chrono::seconds(1);
        if (operation.kind != ExecutionOperationKind::ContinueUntil &&
            operation.kind != ExecutionOperationKind::StepFrames &&
            operation.kind !=
                ExecutionOperationKind::ContinueUntilInputObserved)
            return;

        InputExecutionRelationshipInspection input;
        const auto relationship = InputRelationshipOf(operation.request);
        if (relationship && config.input_relationships)
        {
            input = config.input_relationships->Inspect(*relationship, epoch);
        }
        const ExecutionRequestPolicy* policy = DiagnosticPolicy(operation);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            current - operation.started).count();
        const std::uint64_t recording_delta =
            observed.movie_input_count >= operation.start_movie_input_count
            ? observed.movie_input_count - operation.start_movie_input_count
            : 0;
        const std::uint32_t target = operation.kind ==
                ExecutionOperationKind::StepFrames
            ? std::get<StepFramesRequest>(operation.request).count
            : 0;
        SCLOGDX(
            SC_TAGS("execution.operation", "execution.heartbeat"),
            "operation=%llu kind=%s epoch=%llu invocation=%llu attempt=%llu request=%llu selector=%s elapsed_ms=%lld awaited_pcs=%s input_relationship=%llu publication_epoch=%llu buttons=0x%04X callbacks=%u a_callbacks=%u poll_inspection=%s core_state=%u pc=0x%08X vi=%llu movie_state=%u recording_input_count=%llu recording_input_delta=%llu completed=%u target=%u baseline_vi=%llu observed_running=%d",
            operation.id.value(), KindName(operation.kind), epoch.value(),
            policy ? policy->diagnostic_invocation : 0,
            policy ? policy->diagnostic_attempt : 0,
            policy ? policy->diagnostic_request : 0,
            policy && !policy->diagnostic_selector.empty()
                ? policy->diagnostic_selector.c_str()
                : "<unnamed>",
            static_cast<long long>(elapsed), AwaitedPcs(operation.request).c_str(),
            relationship ? relationship->value() : 0,
            input.publication_epoch, input.frame.buttons,
            input.callback_count, input.a_control_callback_count,
            input.ok ? (input.requires_observation ? "observed" : "not_required")
                     : (relationship ? input.message.c_str() : "none"),
            static_cast<unsigned>(observed.core_state), observed.pc,
            observed.vi_count, static_cast<unsigned>(observed.movie_state),
            observed.movie_input_count, recording_delta,
            operation.completed_count, target,
            operation.advance_baseline_vi,
            operation.observed_running ? 1 : 0);
    }

    [[nodiscard]] ExecutionOperationId NextOperationId()
    {
        if (next_operation == 0)
            return {};
        const ExecutionOperationId id(next_operation);
        if (next_operation == std::numeric_limits<std::uint64_t>::max())
            next_operation = 0;
        else
            ++next_operation;
        return id;
    }

    [[nodiscard]] InterruptionFrameId NextFrameId()
    {
        if (next_frame == 0)
            return {};
        const InterruptionFrameId id(next_frame);
        if (next_frame == std::numeric_limits<std::uint64_t>::max())
            next_frame = 0;
        else
            ++next_frame;
        return id;
    }

    void RefreshSnapshot(const ExecutionObservation* backend_snapshot = nullptr)
    {
        snapshot.workset_epoch = epoch;
        snapshot.active_operation =
            active ? std::optional(active->id) : std::nullopt;
        if (active)
            snapshot.active_kind = active->kind;
        snapshot.interruption_depth =
            static_cast<std::uint8_t>(std::min<std::size_t>(
                handlers.size(),
                std::numeric_limits<std::uint8_t>::max()));
        snapshot.active_interruption_frame = handlers.empty()
            ? std::nullopt
            : std::optional(handlers.back().id);
        snapshot.input_bound =
            active && InputRelationshipOf(active->request).has_value();
        if (backend_snapshot)
        {
            snapshot.evidence = ConvertEvidence(*backend_snapshot);
            snapshot.control_generation =
                backend_snapshot->applied_control_generation;
        }

        if (stopping)
        {
            snapshot.activity = ExecutionActivity::Closed;
            snapshot.control_state = ExecutionControlState::Stopping;
        }
        else if (!active)
        {
            snapshot.activity = handlers.empty()
                ? ExecutionActivity::IdlePaused
                : ExecutionActivity::HandlingInterruption;
            snapshot.control_state = handlers.empty()
                ? ExecutionControlState::PausedReady
                : ExecutionControlState::Interrupting;
        }
        else
        {
            if (active->pending_terminal)
                snapshot.control_state = ExecutionControlState::Completing;
            switch (active->kind)
            {
            case ExecutionOperationKind::ContinueUntil:
                snapshot.activity = ExecutionActivity::Continuing;
                if (!active->pending_terminal)
                    snapshot.control_state = ExecutionControlState::Running;
                break;
            case ExecutionOperationKind::StepFrames:
                snapshot.activity = ExecutionActivity::SteppingFrame;
                if (!active->pending_terminal)
                    snapshot.control_state = ExecutionControlState::FrameStepping;
                break;
            case ExecutionOperationKind::ContinueUntilInputObserved:
                snapshot.activity = ExecutionActivity::Continuing;
                if (!active->pending_terminal)
                    snapshot.control_state = ExecutionControlState::Running;
                break;
            case ExecutionOperationKind::SafePause:
                snapshot.activity = ExecutionActivity::Pausing;
                if (!active->pending_terminal)
                    snapshot.control_state = ExecutionControlState::ConfirmingPause;
                break;
            case ExecutionOperationKind::InteractiveResume:
                snapshot.activity = ExecutionActivity::InteractiveRunning;
                if (!active->pending_terminal)
                    snapshot.control_state = ExecutionControlState::Running;
                break;
            }
        }
    }

    void PublishState(const ExecutionObservation* backend_snapshot = nullptr)
    {
        RefreshSnapshot(backend_snapshot);
        events.push_back({
            ExecutionEventKind::StateChanged,
            snapshot,
            std::nullopt,
            std::nullopt});
    }

    void PublishProgress(
        const ActiveOperation& operation,
        const ExecutionObservation& observed)
    {
        RefreshSnapshot(&observed);
        ExecutionProgress progress;
        progress.operation_id = operation.id;
        progress.kind = operation.kind;
        progress.completed_count = operation.completed_count;
        progress.evidence = ConvertEvidence(observed);
        events.push_back({
            ExecutionEventKind::Progress,
            snapshot,
            std::nullopt,
            std::move(progress)});
    }

    void PublishHealthWarning(
        const ActiveOperation& operation,
        ExecutionHealthWarningKind kind,
        Clock::duration elapsed,
        std::string code,
        std::string message)
    {
        RefreshSnapshot();
        ExecutionHealthWarning warning;
        warning.kind = kind;
        warning.workset_epoch = epoch;
        warning.operation_id = operation.id;
        warning.elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                elapsed);
        warning.host_activity_generation =
            host_activity->snapshot().generation;
        warning.code = std::move(code);
        warning.message = std::move(message);
        events.push_back({
            ExecutionEventKind::HealthWarning,
            snapshot,
            std::nullopt,
            std::nullopt,
            std::move(warning)});
    }

    void PublishHostActivityWarnings(
        ActiveOperation& operation,
        std::uint32_t activity_sequence,
        std::chrono::milliseconds elapsed)
    {
        if (activity_sequence == 0)
            return;
        if (operation.host_activity_sequence != activity_sequence)
        {
            operation.host_activity_sequence = activity_sequence;
            operation.host_activity_warned_through =
                std::chrono::milliseconds::zero();
        }

        std::chrono::milliseconds next_warning =
            operation.host_activity_warned_through ==
                    std::chrono::milliseconds::zero()
            ? config.host_activity_warning_after
            : operation.host_activity_warned_through +
                  config.host_activity_warning_repeat;
        while (elapsed >= next_warning)
        {
            PublishHealthWarning(
                operation,
                ExecutionHealthWarningKind::HostActivityLongRunning,
                next_warning,
                "host_activity_long_running",
                "Synchronous host activity is delaying guest advancement");
            operation.host_activity_warned_through = next_warning;
            next_warning += config.host_activity_warning_repeat;
        }
    }

    void DrainCompletedHostActivityWarnings(
        ActiveOperation& operation)
    {
        HostActivityTracker::CompletedActivityBatch completed =
            host_activity->DrainCompletedActivities();
        for (std::size_t index = 0;
             index < completed.count;
             ++index)
        {
            PublishHostActivityWarnings(
                operation,
                completed.activities[index].sequence,
                completed.activities[index].elapsed);
        }
        if (completed.overflow_count != 0)
        {
            PublishHealthWarning(
                operation,
                ExecutionHealthWarningKind::
                    HostActivityDiagnosticOverflow,
                Clock::duration::zero(),
                "host_activity_diagnostic_overflow",
                "Synchronous host-activity diagnostics overflowed by " +
                    std::to_string(completed.overflow_count) +
                    " completed scope(s)");
        }
    }

    void PumpSuspendedHostActivityWarnings(
        ActiveOperation& operation)
    {
        DrainCompletedHostActivityWarnings(operation);
    }

    [[nodiscard]] ExecutionObservation Query()
    {
        try
        {
            ExecutionObservation observed;
            static_cast<BackendExecutionSnapshot&>(observed) =
                backend.QueryExecutionSnapshot();
            if (!observed.result.ok)
                return observed;

            const MovieStateSnapshot movie =
                observed.core_state == BackendCoreState::Paused &&
                    observed.pause_confirmed
                ? movies.ReconcilePausedState(epoch)
                : movies.SnapshotState(epoch);
            if (!movie.result.ok)
            {
                observed.result = BackendResult::Failure(
                    movie.result.code == MovieServiceErrorCode::Unsupported
                        ? BackendErrorCode::Unavailable
                        : movie.result.code == MovieServiceErrorCode::InvalidState
                            ? BackendErrorCode::InvalidState
                            : BackendErrorCode::OperationFailed,
                    movie.result.message.empty()
                        ? "MovieService state observation failed"
                        : "MovieService state observation failed: " +
                            movie.result.message,
                    movie.result.integrity == GuestIntegrity::Unknown
                        ? BackendIntegrity::Unknown
                        : BackendIntegrity::Preserved);
                return observed;
            }
            if (!movie.workset_epoch || movie.workset_epoch != epoch)
            {
                observed.result = BackendResult::Failure(
                    BackendErrorCode::InvalidState,
                    "MovieService returned state for the wrong workset epoch",
                    BackendIntegrity::Unknown);
                return observed;
            }
            observed.movie_state = movie.state;
            observed.movie_input_count = movie.current_input_count;
            return observed;
        }
        catch (const std::exception& ex)
        {
            ExecutionObservation snapshot_result;
            snapshot_result.result = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                std::string("execution snapshot query threw: ") + ex.what(),
                BackendIntegrity::Unknown);
            return snapshot_result;
        }
        catch (...)
        {
            ExecutionObservation snapshot_result;
            snapshot_result.result = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "execution snapshot query threw",
                BackendIntegrity::Unknown);
            return snapshot_result;
        }
    }

    [[nodiscard]] BackendResult CallBackend(
        const char* name,
        const std::function<BackendResult()>& call)
    {
        try
        {
            return call();
        }
        catch (const std::exception& ex)
        {
            return BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                std::string(name) + " threw: " + ex.what(),
                BackendIntegrity::Unknown);
        }
        catch (...)
        {
            return BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                std::string(name) + " threw",
                BackendIntegrity::Unknown);
        }
    }

    [[nodiscard]] BackendResult SubmitBackendControl(
        const char* source,
        BackendControlCommandKind kind)
    {
        ControlTransitionDiagnostic diagnostic;
        diagnostic.sequence = next_control_sequence++;
        diagnostic.generation = ExecutionControlGeneration(
            g_control_generation.fetch_add(1, std::memory_order_relaxed));
        diagnostic.kind = kind;
        diagnostic.submitted = now();
        diagnostic.source = source;

        BackendResult result = CallBackend(
            source,
            [&] {
                return backend.SubmitControlCommand(
                    {diagnostic.generation, kind});
            });
        diagnostic.ok = result.ok;
        diagnostic.error = result.message;
        control_history[next_control_history] = std::move(diagnostic);
        next_control_history =
            (next_control_history + 1) % control_history.size();
        control_history_size =
            std::min(control_history_size + 1, control_history.size());
        return result;
    }

    [[nodiscard]] std::uint64_t LogUnroutedPause(
        const ActiveOperation& operation,
        const ExecutionObservation& observed,
        const char* reason)
    {
        const std::uint64_t incident = next_unrouted_pause_incident++;
        const ExecutionRequestPolicy* policy = DiagnosticPolicy(operation);
        std::ostringstream text;
        text << "incident=" << incident
             << " reason=" << reason
             << " operation=" << operation.id.value()
             << " kind=" << KindName(operation.kind)
             << " epoch=" << epoch.value()
             << " invocation="
             << (policy ? policy->diagnostic_invocation : 0)
             << " attempt=" << (policy ? policy->diagnostic_attempt : 0)
             << " request=" << (policy ? policy->diagnostic_request : 0)
             << " selector="
             << (policy && !policy->diagnostic_selector.empty()
                     ? policy->diagnostic_selector
                     : "<unnamed>")
             << '\n'
             << "execution_snapshot core_state="
             << static_cast<unsigned>(observed.core_state)
             << " pause_confirmed=" << (observed.pause_confirmed ? 1 : 0)
             << " pc=0x" << std::hex << observed.pc << std::dec
             << " vi=" << observed.vi_count
             << " movie_state=" << static_cast<unsigned>(observed.movie_state)
             << " movie_input_count=" << observed.movie_input_count
             << " applied_control_generation="
             << observed.applied_control_generation.value()
             << " control_transition_in_flight="
             << (observed.control_transition_in_flight ? 1 : 0)
             << " observed_running="
             << (operation.observed_running ? 1 : 0)
             << '\n'
             << "recent_control_commands count=" << control_history_size;

        const auto current = now();
        const std::size_t oldest =
            (next_control_history + control_history.size() -
             control_history_size) %
            control_history.size();
        for (std::size_t i = 0; i < control_history_size; ++i)
        {
            const ControlTransitionDiagnostic& command =
                control_history[(oldest + i) % control_history.size()];
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                current - command.submitted);
            text << '\n'
                 << "  sequence=" << command.sequence
                 << " generation=" << command.generation.value()
                 << " kind=" << ControlCommandKindName(command.kind)
                 << " source=" << command.source
                 << " age_ms=" << age.count()
                 << " ok=" << (command.ok ? 1 : 0);
            if (!command.error.empty())
                text << " error=" << command.error;
        }

        try
        {
            text << '\n' << stop_points.DescribeUnroutedPause(observed.pc);
        }
        catch (const std::exception& ex)
        {
            text << '\n'
                 << "stop_point_diagnostics_error=" << ex.what();
        }
        catch (...)
        {
            text << '\n' << "stop_point_diagnostics_error=unknown";
        }

        try
        {
            text << '\n'
                 << savor::probe::DescribeRecentBreakpointDiagnostics(32);
        }
        catch (const std::exception& ex)
        {
            text << '\n'
                 << "breakpoint_diagnostics_error=" << ex.what();
        }
        catch (...)
        {
            text << '\n'
                 << "breakpoint_diagnostics_error=unknown";
        }

        const std::string diagnostic = text.str();
        SCLOGWX(
            SC_TAGS("execution.unrouted_pause", "execution.invariant"),
            "%s",
            diagnostic.c_str());
        return incident;
    }

    [[nodiscard]] BackendHealthReport CheckBackendHealth()
    {
        try
        {
            return backend.CheckHealth();
        }
        catch (const std::exception& ex)
        {
            return {
                false,
                BackendCoreState::Unknown,
                std::string("execution health check threw: ") + ex.what()};
        }
        catch (...)
        {
            return {
                false,
                BackendCoreState::Unknown,
                "execution health check threw"};
        }
    }

    void ApplyCoreStallHealthProof(
        ExecutionTerminalStatus status,
        ExecutionError& error)
    {
        if (status != ExecutionTerminalStatus::CoreStalled)
            return;
        const BackendHealthReport health = CheckBackendHealth();
        if (health.healthy &&
            health.core_state == BackendCoreState::Paused)
        {
            return;
        }
        error = Error(
            ExecutionErrorCode::BackendFailure,
            health.diagnostic.empty()
                ? "core_stalled: paused backend health is unproven"
                : "core_stalled: " + health.diagnostic,
            BackendIntegrity::Unknown);
    }

    void RebaselineHealth(
        ActiveOperation& operation,
        const ExecutionObservation& observed,
        Clock::time_point current)
    {
        const HostActivityTracker::Snapshot host =
            host_activity->snapshot();
        operation.health_baseline = current;
        operation.health_last_vi = observed.vi_count;
        operation.health_host_generation = host.generation;
        operation.stall_suspected = false;
    }

    [[nodiscard]] bool PumpCoreHealth(
        ActiveOperation& operation,
        const ExecutionObservation& observed,
        Clock::time_point current)
    {
        DrainCompletedHostActivityWarnings(operation);

        if (operation.kind == ExecutionOperationKind::SafePause ||
            observed.core_state != BackendCoreState::Running)
        {
            operation.health_eligible = false;
            RebaselineHealth(operation, observed, current);
            return false;
        }

        const HostActivityTracker::Snapshot host =
            host_activity->snapshot();
        if (!operation.health_eligible)
        {
            operation.health_eligible = true;
            RebaselineHealth(operation, observed, current);
            return false;
        }

        if (host.in_flight != 0)
        {
            RebaselineHealth(operation, observed, current);
            return false;
        }

        if (observed.vi_count != operation.health_last_vi ||
            host.generation != operation.health_host_generation)
        {
            RebaselineHealth(operation, observed, current);
            return false;
        }

        const Clock::duration stalled_for =
            current - operation.health_baseline;
        if (stalled_for >=
            config.suspect_core_stall_after +
                config.confirm_core_stall_after)
        {
            const ExecutionObservation confirmation = Query();
            const HostActivityTracker::Snapshot confirmation_host =
                host_activity->snapshot();
            if (!confirmation.result.ok)
            {
                BeginFinish(
                    ExecutionTerminalStatus::CleanupFailure,
                    BackendError(
                        "core-stall confirmation snapshot failed",
                        confirmation.result));
                return true;
            }
            if (confirmation.core_state != BackendCoreState::Running ||
                confirmation.vi_count != observed.vi_count ||
                confirmation_host.in_flight != 0 ||
                confirmation_host.generation != host.generation)
            {
                RebaselineHealth(operation, confirmation, current);
                return false;
            }
            if (!operation.stall_suspected)
            {
                operation.stall_suspected = true;
                PublishHealthWarning(
                    operation,
                    ExecutionHealthWarningKind::SuspectedCoreStall,
                    stalled_for,
                    "suspected_core_stall",
                    "Dolphin has not demonstrated VI or synchronous host progress");
            }
            BeginFinish(
                ExecutionTerminalStatus::CoreStalled,
                Error(
                    ExecutionErrorCode::BackendFailure,
                    "core_stalled"));
            return true;
        }
        if (!operation.stall_suspected &&
            stalled_for >= config.suspect_core_stall_after)
        {
            operation.stall_suspected = true;
            PublishHealthWarning(
                operation,
                ExecutionHealthWarningKind::SuspectedCoreStall,
                stalled_for,
                "suspected_core_stall",
                "Dolphin has not demonstrated VI or synchronous host progress");
        }
        return false;
    }

    [[nodiscard]] BackendResult RestoreThrottle(ActiveOperation& operation)
    {
        if (!operation.throttle_changed)
            return BackendResult::Success();
        BackendResult result = CallBackend(
            "throttle restoration",
            [&] {
                return backend.SetThrottleDisabled(
                    operation.original_throttle_disabled);
            });
        if (result.ok)
            operation.throttle_changed = false;
        return result;
    }

    [[nodiscard]] ExecutionError ParkWakeGroup(
        ActiveOperation& operation)
    {
        if (operation.kind != ExecutionOperationKind::ContinueUntil ||
            !operation.wake_group || operation.wake_group_parked)
        {
            return {};
        }
        const StopReleaseReceipt released =
            operation.wake_group->Release();
        operation.wake_group.reset();
        if (!released.ok)
        {
            return Error(
                ExecutionErrorCode::StopPointFailure,
                released.error.message.empty()
                    ? "failed releasing suspended foreground wait"
                    : released.error.message,
                released.error.code ==
                        StopPointErrorCode::PhysicalIntegrityUnknown
                    ? BackendIntegrity::Unknown
                    : BackendIntegrity::Preserved);
        }
        operation.wake_group_parked = true;
        return {};
    }

    [[nodiscard]] ExecutionError UnparkWakeGroup(
        ActiveOperation& operation)
    {
        if (!operation.wake_group_parked)
            return {};
        if (ExecutionError restored = PrepareWakeGroup(operation, true))
            return restored;
        operation.wake_group_parked = false;
        return {};
    }

    void EmitTerminal(
        ActiveOperation operation,
        ExecutionTerminalStatus status,
        ExecutionError error = {},
        std::optional<StopRouteReceipt> stop = std::nullopt,
        const ExecutionObservation* known_snapshot = nullptr)
    {
        DrainCompletedHostActivityWarnings(operation);
        InputExecutionRelationshipInspection diagnostic_input;
        const auto diagnostic_relationship =
            InputRelationshipOf(operation.request);
        if (diagnostic_relationship && config.input_relationships)
        {
            diagnostic_input = config.input_relationships->Inspect(
                *diagnostic_relationship, epoch);
        }
        ExecutionObservation observed =
            known_snapshot ? *known_snapshot : Query();
        if (!observed.result.ok && !error)
            error = BackendError("execution terminal snapshot failed", observed.result);

        BackendResult throttle = RestoreThrottle(operation);
        if (!throttle.ok)
        {
            status = ExecutionTerminalStatus::CleanupFailure;
            error = BackendError("execution throttle cleanup failed", throttle);
        }
        if (operation.wake_group)
        {
            StopReleaseReceipt released = operation.wake_group->Release();
            operation.wake_group.reset();
            if (!released.ok)
            {
                status = ExecutionTerminalStatus::CleanupFailure;
                error = Error(
                    ExecutionErrorCode::StopPointFailure,
                    released.error.message.empty()
                        ? "execution Wake group cleanup failed"
                        : released.error.message,
                    released.error.code == StopPointErrorCode::PhysicalIntegrityUnknown
                        ? BackendIntegrity::Unknown
                        : BackendIntegrity::Preserved);
            }
        }
        if (const auto input_relationship =
                InputRelationshipOf(operation.request);
            input_relationship && config.input_relationships)
        {
            const bool borrowed_from_parent =
                operation.handler_owner && !handlers.empty() &&
                handlers.back().id == *operation.handler_owner &&
                InputRelationshipOf(handlers.back().parent.request) ==
                    input_relationship;
            const bool completed =
                status ==
                    ExecutionTerminalStatus::RequestedCompletion ||
                status == ExecutionTerminalStatus::StepsCompleted ||
                status == ExecutionTerminalStatus::InputObserved ||
                status == ExecutionTerminalStatus::Paused;
            const InputExecutionRelationshipOperationReceipt retired = borrowed_from_parent
                ? InputExecutionRelationshipOperationReceipt{true, {}}
                : completed
                ? config.input_relationships->Complete(
                      *input_relationship,
                      epoch)
                : config.input_relationships->Cancel(
                      *input_relationship,
                      epoch);
            if (!retired.ok)
            {
                status = ExecutionTerminalStatus::CleanupFailure;
                error = Error(
                    ExecutionErrorCode::InputUnavailable,
                    retired.message.empty()
                        ? "input relationship cleanup failed"
                        : retired.message);
            }
        }
        ExecutionTerminalResult terminal;
        terminal.operation_id = operation.id;
        terminal.kind = operation.kind;
        terminal.status = status;
        terminal.workset_epoch = epoch;
        terminal.completed_count = operation.completed_count;
        terminal.evidence = ConvertEvidence(observed);
        terminal.stop = std::move(stop);
        terminal.error = std::move(error);
        terminal.integrity = terminal.error
            ? terminal.error.integrity
            : observed.result.integrity;
        terminal.control_generation = observed.applied_control_generation;
        if (terminal.stop)
        {
            terminal.stop_transition = StopTransitionId(
                terminal.stop->stop_transition_id);
        }

        if (operation.kind == ExecutionOperationKind::ContinueUntil ||
            operation.kind == ExecutionOperationKind::StepFrames ||
            operation.kind ==
                ExecutionOperationKind::ContinueUntilInputObserved)
        {
            const ExecutionRequestPolicy* policy = DiagnosticPolicy(operation);
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now() - operation.started).count();
            const std::uint64_t recording_delta =
                observed.movie_input_count >= operation.start_movie_input_count
                ? observed.movie_input_count - operation.start_movie_input_count
                : 0;
            SCLOGDX(
                SC_TAGS("execution.operation", "execution.terminal"),
                "operation=%llu kind=%s epoch=%llu invocation=%llu attempt=%llu request=%llu selector=%s status=%s elapsed_ms=%lld awaited_pcs=%s input_relationship=%llu publication_epoch=%llu buttons=0x%04X callbacks=%u a_callbacks=%u core_state=%u pc=0x%08X vi=%llu movie_state=%u recording_input_count=%llu recording_input_delta=%llu routed_sequence=%llu hit_pc=0x%08X error=%s",
                operation.id.value(), KindName(operation.kind), epoch.value(),
                policy ? policy->diagnostic_invocation : 0,
                policy ? policy->diagnostic_attempt : 0,
                policy ? policy->diagnostic_request : 0,
                policy && !policy->diagnostic_selector.empty()
                    ? policy->diagnostic_selector.c_str()
                    : "<unnamed>",
                TerminalName(terminal.status), static_cast<long long>(elapsed),
                AwaitedPcs(operation.request).c_str(),
                diagnostic_relationship ? diagnostic_relationship->value() : 0,
                diagnostic_input.publication_epoch,
                static_cast<unsigned>(diagnostic_input.frame.buttons),
                diagnostic_input.callback_count,
                diagnostic_input.a_control_callback_count,
                static_cast<unsigned>(observed.core_state), observed.pc,
                observed.vi_count, static_cast<unsigned>(observed.movie_state),
                observed.movie_input_count, recording_delta,
                terminal.stop ? terminal.stop->identity.sequence.value() : 0,
                terminal.stop && terminal.stop->event
                    ? terminal.stop->event->evidence.hit_pc
                    : 0,
                terminal.error.message.empty()
                    ? "<none>"
                    : terminal.error.message.c_str());
        }

        const std::optional<ExecutionOperationId> pause_control_id =
            operation.pause_control_id;
        active.reset();
        RefreshSnapshot(&observed);
        const BackendIntegrity terminal_integrity = terminal.integrity;
        events.push_back({
            ExecutionEventKind::Terminal,
            snapshot,
            std::move(terminal)});
        if (pause_control_id)
        {
            ExecutionTerminalResult pause_terminal;
            pause_terminal.operation_id = *pause_control_id;
            pause_terminal.kind = ExecutionOperationKind::SafePause;
            pause_terminal.status =
                status == ExecutionTerminalStatus::Paused
                ? ExecutionTerminalStatus::Paused
                : ExecutionTerminalStatus::CleanupFailure;
            pause_terminal.workset_epoch = epoch;
            pause_terminal.evidence = ConvertEvidence(observed);
            pause_terminal.error =
                pause_terminal.status == ExecutionTerminalStatus::Paused
                ? ExecutionError{}
                : Error(
                      ExecutionErrorCode::BackendFailure,
                      "interactive pause could not be confirmed",
                      terminal_integrity);
            pause_terminal.integrity = terminal_integrity;
            events.push_back({
                ExecutionEventKind::Terminal,
                snapshot,
                std::move(pause_terminal)});
        }
        PublishState(&observed);

        if (!pending_parent_terminals.empty())
        {
            std::vector<PendingParentTerminal> parents;
            parents.swap(pending_parent_terminals);
            const bool pause_confirmed =
                observed.result.ok &&
                observed.core_state == BackendCoreState::Paused &&
                observed.pause_confirmed;
            for (PendingParentTerminal& parent : parents)
            {
                if (!pause_confirmed)
                {
                    parent.status =
                        ExecutionTerminalStatus::CleanupFailure;
                    parent.error = Error(
                        ExecutionErrorCode::BackendFailure,
                        "interruption-parent cancellation could not confirm Dolphin paused",
                        BackendIntegrity::Unknown);
                }
                EmitTerminal(
                    std::move(parent.operation),
                    parent.status,
                    std::move(parent.error),
                    std::nullopt,
                    &observed);
            }
        }
    }

    void BeginFinish(
        ExecutionTerminalStatus status,
        ExecutionError error = {},
        std::optional<StopRouteReceipt> stop = std::nullopt)
    {
        if (!active || active->pending_terminal)
            return;
        ExecutionObservation observed = Query();
        if (!observed.result.ok)
        {
            ActiveOperation finished = std::move(*active);
            active.reset();
            (void)SubmitBackendControl(
                "execution emergency pause after snapshot failure",
                BackendControlCommandKind::Pause);
            EmitTerminal(
                std::move(finished),
                ExecutionTerminalStatus::CleanupFailure,
                BackendError(
                    "execution state query failed",
                    observed.result),
                std::move(stop),
                &observed);
            return;
        }
        active->pending_terminal = status;
        active->pending_error = std::move(error);
        active->pending_stop = std::move(stop);
        if (observed.core_state == BackendCoreState::Paused &&
            observed.pause_confirmed)
        {
            ActiveOperation finished = std::move(*active);
            const auto final_status = *finished.pending_terminal;
            ExecutionError final_error =
                finished.pending_error.value_or(ExecutionError{});
            ApplyCoreStallHealthProof(final_status, final_error);
            std::optional<StopRouteReceipt> final_stop =
                std::move(finished.pending_stop);
            EmitTerminal(
                std::move(finished),
                final_status,
                std::move(final_error),
                std::move(final_stop),
                &observed);
            return;
        }

        if (!active->pause_control_deadline)
        {
            active->pause_control_deadline =
                now() + config.pause_confirmation_timeout;
        }
        BackendResult pause = SubmitBackendControl(
            "execution terminal pause",
            BackendControlCommandKind::Pause);
        if (!pause.ok)
        {
            ActiveOperation finished = std::move(*active);
            std::optional<StopRouteReceipt> final_stop =
                std::move(finished.pending_stop);
            EmitTerminal(
                std::move(finished),
                ExecutionTerminalStatus::CleanupFailure,
                BackendError("failed pausing at execution terminal", pause),
                std::move(final_stop),
                &observed);
        }
    }

    [[nodiscard]] ExecutionError ValidateCommon(
        const ExecutionRequest& request) const
    {
        if (!initialized || stopping)
        {
            return Error(
                ExecutionErrorCode::RuntimeStopping,
                "ExecutionControlCore is not available");
        }
        if (!EpochOf(request) || EpochOf(request) != epoch)
        {
            return Error(
                ExecutionErrorCode::WorksetEpochMismatch,
                "Execution request WorksetEpoch does not match the session");
        }
        if (ThrottleOf(request) != ExecutionThrottlePolicy::Preserve &&
            !HasExecutionCapability(
                backend.Capabilities(),
                BackendExecutionCapability::ThrottleControl))
        {
            return Error(
                ExecutionErrorCode::Unsupported,
                "Execution backend does not support throttle control");
        }
        if (const auto relationship = InputRelationshipOf(request))
        {
            if (!config.input_relationships)
            {
                return Error(
                    ExecutionErrorCode::Unsupported,
                    "execution input relationship requires InputArbiter");
            }
            try
            {
                const InputExecutionRelationshipOperationReceipt validation =
                    config.input_relationships->Validate(*relationship, epoch);
                if (!validation.ok)
                {
                    return Error(
                        ExecutionErrorCode::InputUnavailable,
                        validation.message.empty()
                            ? "execution input relationship is invalid"
                            : validation.message);
                }
            }
            catch (const std::exception& ex)
            {
                return Error(
                    ExecutionErrorCode::InputUnavailable,
                    std::string("input relationship validation threw: ") +
                        ex.what(),
                    BackendIntegrity::Unknown);
            }
            catch (...)
            {
                return Error(
                    ExecutionErrorCode::InputUnavailable,
                    "input relationship validation threw",
                    BackendIntegrity::Unknown);
            }
        }
        switch (KindOf(request))
        {
        case ExecutionOperationKind::ContinueUntil:
        {
            const auto& value = std::get<ContinueUntilRequest>(request);
            if (value.wake_group.subscriptions.empty())
            {
                return Error(
                    ExecutionErrorCode::InvalidArgument,
                    "ContinueUntil requires at least one Wake alternative");
            }
            if (std::any_of(
                    value.wake_group.subscriptions.begin(),
                    value.wake_group.subscriptions.end(),
                    [](const StopSubscriptionDefinition& subscription) {
                        return !std::holds_alternative<ForegroundStopWait>(
                            subscription.route);
                    }))
            {
                return Error(
                    ExecutionErrorCode::InvalidArgument,
                    "ContinueUntil group may contain only foreground-wait alternatives");
            }
            break;
        }
        case ExecutionOperationKind::StepFrames:
            if (std::get<StepFramesRequest>(request).count == 0)
                return Error(ExecutionErrorCode::InvalidArgument, "Frame-step count must be nonzero");
            if (!HasExecutionCapability(
                    backend.Capabilities(),
                    BackendExecutionCapability::FrameStep))
            {
                return Error(
                    ExecutionErrorCode::Unsupported,
                    "Guest-frame stepping is unavailable");
            }
            break;
        case ExecutionOperationKind::SafePause:
            if (std::get<SafePauseRequest>(request)
                    .confirmation_timeout <=
                std::chrono::milliseconds::zero())
            {
                return Error(
                    ExecutionErrorCode::InvalidArgument,
                    "SafePause requires a positive host confirmation timeout");
            }
            break;
        case ExecutionOperationKind::ContinueUntilInputObserved:
            if (!std::get<ContinueUntilInputObservedRequest>(request)
                     .policy.input_relationship)
            {
                return Error(
                    ExecutionErrorCode::InvalidArgument,
                    "Input-observation execution requires an exact input relationship");
            }
            break;
        case ExecutionOperationKind::InteractiveResume:
            break;
        }
        return {};
    }

    [[nodiscard]] ExecutionError ApplyThrottle(
        ActiveOperation& operation,
        const ExecutionObservation& observed)
    {
        const ExecutionThrottlePolicy policy = ThrottleOf(operation.request);
        if (policy == ExecutionThrottlePolicy::Preserve)
            return {};
        if (!HasExecutionCapability(
                backend.Capabilities(),
                BackendExecutionCapability::ThrottleControl))
        {
            return Error(
                ExecutionErrorCode::Unsupported,
                "Execution backend does not support throttle control");
        }
        const bool target =
            policy == ExecutionThrottlePolicy::RequireDisabled;
        operation.original_throttle_disabled = observed.throttle_disabled;
        if (target == observed.throttle_disabled)
            return {};
        BackendResult changed = CallBackend(
            "execution throttle override",
            [&] { return backend.SetThrottleDisabled(target); });
        if (!changed.ok)
            return BackendError("execution throttle override failed", changed);
        operation.throttle_changed = true;
        return {};
    }

    [[nodiscard]] ExecutionError PrepareWakeGroup(
        ActiveOperation& operation,
        bool restoring = false)
    {
        auto& request = std::get<ContinueUntilRequest>(operation.request);
        for (StopSubscriptionDefinition& subscription :
            request.wake_group.subscriptions)
        {
            subscription.consumer = owner;
            auto* foreground =
                std::get_if<ForegroundStopWait>(&subscription.route);
            if (foreground == nullptr)
            {
                return Error(
                    ExecutionErrorCode::InvalidArgument,
                    "ContinueUntil received a non-foreground route");
            }
            foreground->execution_control_generation =
                g_control_generation.load(std::memory_order_acquire) - 1;
            foreground->execution_operation_id = operation.id.value();
        }
        if (operation.wake_group)
        {
            request.wake_group.id =
                operation.wake_group->lease().group_id;
            request.wake_group.source.id =
                operation.wake_group->lease().source_id;
            StopGroupReceipt replaced =
                operation.wake_group->Replace(request.wake_group);
            if (!replaced.ok)
            {
                return Error(
                    ExecutionErrorCode::StopPointFailure,
                    replaced.error.message.empty()
                        ? "ContinueUntil Wake replacement failed"
                        : replaced.error.message,
                    replaced.error.code ==
                            StopPointErrorCode::PhysicalIntegrityUnknown
                        ? BackendIntegrity::Unknown
                        : BackendIntegrity::Preserved);
            }
            return {};
        }
        StopGroupRegistrationOptions options;
        switch (request.policy.current_point)
        {
        case ExecutionCurrentPointPolicy::Ignore:
            options.current_point = StopCurrentPointPolicy::Ignore;
            break;
        case ExecutionCurrentPointPolicy::AcceptIfAvailable:
            options.current_point = StopCurrentPointPolicy::AcceptIfAvailable;
            break;
        case ExecutionCurrentPointPolicy::Require:
            options.current_point = StopCurrentPointPolicy::Require;
            break;
        }
        if (restoring)
            options.current_point = StopCurrentPointPolicy::Ignore;

        StopGroupRegistrationResult registration =
            stop_points.RegisterGroup(request.wake_group, options);
        if (!registration.receipt.ok)
        {
            return Error(
                ExecutionErrorCode::StopPointFailure,
                registration.receipt.error.message.empty()
                    ? "ContinueUntil Wake registration failed"
                    : registration.receipt.error.message,
                registration.receipt.error.code ==
                        StopPointErrorCode::PhysicalIntegrityUnknown
                    ? BackendIntegrity::Unknown
                    : BackendIntegrity::Preserved);
        }
        operation.wake_group.emplace(std::move(registration.handle));
        if (registration.current_point)
        {
            if (registration.current_point->terminal ==
                StopRouteTerminal::ForegroundMatched)
            {
                operation.pending_terminal =
                    ExecutionTerminalStatus::RequestedCompletion;
                operation.pending_stop =
                    std::move(*registration.current_point);
            }
            else if (request.policy.current_point ==
                ExecutionCurrentPointPolicy::Require)
            {
                return Error(
                    ExecutionErrorCode::StopPointFailure,
                    "Required current point did not satisfy ContinueUntil");
            }
        }
        return {};
    }

    [[nodiscard]] ExecutionError DepartRetainedPoint()
    {
        if (StopPointError error = stop_points.DepartCurrentPoint())
        {
            return Error(
                ExecutionErrorCode::StopPointFailure,
                error.message.empty()
                    ? "failed departing the retained stop point"
                    : error.message,
                BackendIntegrity::Unknown);
        }
        return {};
    }

    [[nodiscard]] ExecutionError ResumeBackend()
    {
        if (ExecutionError departed = DepartRetainedPoint())
            return departed;
        BackendResult resumed = SubmitBackendControl(
            "execution resume",
            BackendControlCommandKind::Resume);
        if (!resumed.ok)
            return BackendError("execution resume failed", resumed);
        return {};
    }

    [[nodiscard]] ExecutionError BeginAdvance(ActiveOperation& operation)
    {
        ExecutionObservation observed = Query();
        if (!observed.result.ok)
            return BackendError("execution advance snapshot failed", observed.result);
        if (observed.core_state != BackendCoreState::Paused ||
            !observed.pause_confirmed)
        {
            return Error(
                ExecutionErrorCode::InvalidState,
                "bounded advancement requires an authoritatively paused core");
        }
        if (ExecutionError departed = DepartRetainedPoint())
            return departed;

        SCLOGDX(
            SC_TAGS("execution.frame_step", "execution.transition"),
            "operation=%llu epoch=%llu substep=%u target=%u baseline_vi=%llu pre_state=%u pause_confirmed=%d",
            static_cast<unsigned long long>(operation.id.value()),
            static_cast<unsigned long long>(epoch.value()),
            operation.completed_count + 1,
            std::get<StepFramesRequest>(operation.request).count,
            static_cast<unsigned long long>(observed.vi_count),
            static_cast<unsigned>(observed.core_state),
            observed.pause_confirmed ? 1 : 0);

        BackendResult started;
        switch (operation.kind)
        {
        case ExecutionOperationKind::StepFrames:
            if (!HasExecutionCapability(
                    backend.Capabilities(),
                    BackendExecutionCapability::FrameStep))
            {
                return Error(
                    ExecutionErrorCode::Unsupported,
                    "Guest-frame stepping is unavailable");
            }
            started = SubmitBackendControl(
                "frame step",
                BackendControlCommandKind::FrameStep);
            break;
        default:
            return Error(
                ExecutionErrorCode::InvalidState,
                "operation is not a bounded advancement");
        }
        if (!started.ok)
            return BackendError("failed starting bounded advancement", started);
        operation.advance_baseline_vi = observed.vi_count;
        operation.awaiting_advance = true;
        operation.observed_running = false;
        operation.no_progress_pause_logged = false;
        SCLOGDX(
            SC_TAGS("execution.frame_step", "execution.transition"),
            "operation=%llu epoch=%llu substep=%u target=%u backend_result=accepted baseline_vi=%llu",
            static_cast<unsigned long long>(operation.id.value()),
            static_cast<unsigned long long>(epoch.value()),
            operation.completed_count + 1,
            std::get<StepFramesRequest>(operation.request).count,
            static_cast<unsigned long long>(observed.vi_count));
        return {};
    }

    [[nodiscard]] ExecutionError StartOperation(ActiveOperation& operation)
    {
        if (operation.kind == ExecutionOperationKind::StepFrames &&
            !HasExecutionCapability(
                backend.Capabilities(),
                BackendExecutionCapability::FrameStep))
        {
            return Error(
                ExecutionErrorCode::Unsupported,
                "Guest-frame stepping is unavailable");
        }
        ExecutionObservation observed = Query();
        if (!observed.result.ok)
            return BackendError("execution start snapshot failed", observed.result);
        if (operation.kind != ExecutionOperationKind::SafePause &&
            (observed.core_state != BackendCoreState::Paused ||
                !observed.pause_confirmed))
        {
            return Error(
                ExecutionErrorCode::InvalidState,
                "execution operation requires an authoritatively paused core");
        }
        if (ExecutionError throttle = ApplyThrottle(operation, observed))
            return throttle;

        switch (operation.kind)
        {
        case ExecutionOperationKind::ContinueUntil:
        {
            if (ExecutionError registration = PrepareWakeGroup(operation))
                return registration;
            if (operation.pending_terminal)
                return {};
            const auto& request =
                std::get<ContinueUntilRequest>(operation.request);
            if (request.expected_movie_input_count &&
                observed.movie_input_count >
                    *request.expected_movie_input_count)
            {
                operation.pending_terminal =
                    ExecutionTerminalStatus::CursorOverrun;
                return {};
            }
            if (request.policy.movie_ended !=
                    MovieEndedPolicy::Ignore &&
                observed.movie_state == MovieState::PlaybackEnded)
            {
                operation.pending_terminal =
                    ExecutionTerminalStatus::MovieEnded;
                if (request.policy.movie_ended ==
                    MovieEndedPolicy::Fail)
                {
                    operation.pending_error = Error(
                        ExecutionErrorCode::InvalidState,
                        "movie ended before the requested completion");
                }
                return {};
            }
            return ResumeBackend();
        }
        case ExecutionOperationKind::StepFrames:
            return BeginAdvance(operation);
        case ExecutionOperationKind::ContinueUntilInputObserved:
        {
            const auto& request =
                std::get<ContinueUntilInputObservedRequest>(
                    operation.request);
            if (observed.movie_state != MovieState::Recording ||
                observed.movie_input_count !=
                    request.expected_movie_input_count)
            {
                return Error(
                    ExecutionErrorCode::InvalidState,
                    "Input-observation execution requires the exact paused recording checkpoint cursor");
            }
            return ResumeBackend();
        }
        case ExecutionOperationKind::SafePause:
            if (observed.core_state == BackendCoreState::Paused &&
                observed.pause_confirmed)
            {
                operation.pending_terminal = ExecutionTerminalStatus::Paused;
                return {};
            }
            operation.pause_control_deadline =
                now() +
                std::get<SafePauseRequest>(operation.request)
                    .confirmation_timeout;
            if (BackendResult pause = SubmitBackendControl(
                    "safe pause",
                    BackendControlCommandKind::Pause);
                !pause.ok)
            {
                return BackendError("safe pause request failed", pause);
            }
            return {};
        case ExecutionOperationKind::InteractiveResume:
            return ResumeBackend();
        }
        return Error(
            ExecutionErrorCode::InvalidArgument,
            "unknown execution operation");
    }

    [[nodiscard]] bool AdvanceCompleted(
        ActiveOperation& operation,
        const ExecutionObservation& observed) const
    {
        if (!operation.awaiting_advance ||
            observed.core_state != BackendCoreState::Paused ||
            !observed.pause_confirmed)
        {
            return false;
        }
        if (observed.vi_count == operation.advance_baseline_vi &&
            !operation.no_progress_pause_logged)
        {
            operation.no_progress_pause_logged = true;
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now() - operation.started).count();
            SCLOGWX(
                SC_TAGS("execution.frame_step", "execution.no_progress"),
                "operation=%llu epoch=%llu substep=%u target=%u baseline_vi=%llu observed_vi=%llu observed_running=%d core_state=%u pc=0x%08X elapsed_ms=%lld",
                static_cast<unsigned long long>(operation.id.value()),
                static_cast<unsigned long long>(epoch.value()),
                operation.completed_count + 1,
                std::get<StepFramesRequest>(operation.request).count,
                static_cast<unsigned long long>(operation.advance_baseline_vi),
                static_cast<unsigned long long>(observed.vi_count),
                operation.observed_running ? 1 : 0,
                static_cast<unsigned>(observed.core_state),
                observed.pc,
                static_cast<long long>(elapsed));
        }
        return observed.vi_count != operation.advance_baseline_vi;
    }

    [[nodiscard]] std::uint32_t TargetCount(
        const ActiveOperation& operation) const
    {
        switch (operation.kind)
        {
        case ExecutionOperationKind::StepFrames:
            return std::get<StepFramesRequest>(operation.request).count;
        default:
            return 0;
        }
    }

    void CompleteAdvance(const ExecutionObservation& observed)
    {
        if (!active)
            return;
        ActiveOperation& operation = *active;
        operation.awaiting_advance = false;
        ++operation.completed_count;

        PublishProgress(operation, observed);
        if (operation.completed_count >= TargetCount(operation))
        {
            BeginFinish(
                ExecutionTerminalStatus::StepsCompleted,
                {},
                std::nullopt);
            return;
        }
        if (ExecutionError next = BeginAdvance(operation))
        {
            BeginFinish(
                next.code == ExecutionErrorCode::Unsupported
                    ? ExecutionTerminalStatus::Unsupported
                    : ExecutionTerminalStatus::BackendFailure,
                std::move(next));
        }
    }

    ExecutionControlCore* owner = nullptr;
};

ExecutionControlCore::ExecutionControlCore(
    IExecutionBackendPort& backend,
    MovieService& movies,
    StopPointRouter& stop_points,
    ExecutionControlCoreConfig config)
    : impl_(std::make_unique<Impl>(
          backend,
          movies,
          stop_points,
          std::move(config)))
{
    impl_->owner = this;
}

ExecutionControlCore::~ExecutionControlCore()
{
    if (impl_ && impl_->initialized &&
        impl_->owner_thread == std::this_thread::get_id())
    {
        (void)Shutdown();
    }
}

BackendResult ExecutionControlCore::Initialize(WorksetEpoch epoch)
{
    if (!impl_->OnOwnerThread())
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidState,
            "ExecutionControlCore initialized outside its owner thread");
    }
    if (impl_->initialized || impl_->stopping || !epoch)
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidState,
            "ExecutionControlCore initialization state is invalid");
    }
    const BackendExecutionCapabilityMask capabilities =
        impl_->backend.Capabilities();
    if (!HasExecutionCapability(
            capabilities,
            BackendExecutionCapability::Pause) ||
        !HasExecutionCapability(
            capabilities,
            BackendExecutionCapability::Resume))
    {
        return BackendResult::Failure(
            BackendErrorCode::Unavailable,
            "Execution backend lacks required pause/resume capabilities");
    }

    impl_->epoch = epoch;
    ExecutionObservation observed = impl_->Query();
    if (!observed.result.ok)
    {
        impl_->epoch = {};
        return observed.result;
    }
    if (observed.core_state != BackendCoreState::Paused ||
        !observed.pause_confirmed)
    {
        impl_->epoch = {};
        return BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "Execution backend did not open at an authoritatively paused CPU boundary",
            BackendIntegrity::Unknown);
    }
    impl_->initialized = true;
    impl_->snapshot.activity = ExecutionActivity::IdlePaused;
    impl_->PublishState(&observed);
    return BackendResult::Success();
}

ExecutionSubmissionReceipt ExecutionControlCore::Submit(ExecutionRequest request)
{
    ExecutionSubmissionReceipt receipt;
    if (!impl_->OnOwnerThread())
    {
        receipt.error = Error(
            ExecutionErrorCode::WrongThread,
            "ExecutionControlCore submitted outside its owner thread");
        return receipt;
    }
    if (ExecutionError validation = impl_->ValidateCommon(request))
    {
        receipt.error = std::move(validation);
        return receipt;
    }

    const ExecutionOperationKind kind = KindOf(request);
    if (impl_->active)
    {
        if (kind != ExecutionOperationKind::SafePause ||
            impl_->active->kind != ExecutionOperationKind::InteractiveResume ||
            impl_->active->pending_terminal)
        {
            receipt.error = Error(
                ExecutionErrorCode::Busy,
                "ExecutionControlCore already owns a foreground operation");
            return receipt;
        }

        const ExecutionOperationId pause_id = impl_->NextOperationId();
        if (!pause_id)
        {
            receipt.error = Error(
                ExecutionErrorCode::InvalidState,
                "Execution operation IDs are exhausted",
                BackendIntegrity::Unknown);
            return receipt;
        }
        impl_->active->pause_control_id = pause_id;
        const auto& pause_request = std::get<SafePauseRequest>(request);
        impl_->active->pause_control_deadline =
            impl_->now() + pause_request.confirmation_timeout;
        receipt.accepted = true;
        receipt.operation_id = pause_id;
        impl_->BeginFinish(ExecutionTerminalStatus::Paused);
        return receipt;
    }
    if (!impl_->handlers.empty())
    {
        receipt.error = Error(
            ExecutionErrorCode::Busy,
            "An interruption handler must submit or finish its child operation");
        return receipt;
    }

    const ExecutionOperationId operation_id = impl_->NextOperationId();
    if (!operation_id)
    {
        receipt.error = Error(
            ExecutionErrorCode::InvalidState,
            "Execution operation IDs are exhausted",
            BackendIntegrity::Unknown);
        return receipt;
    }

    Impl::ActiveOperation operation;
    operation.id = operation_id;
    operation.kind = kind;
    operation.request = std::move(request);
    operation.started = impl_->now();
    operation.next_diagnostic_heartbeat =
        operation.started + std::chrono::seconds(1);
    operation.health_baseline = operation.started;
    const ExecutionObservation observed = impl_->Query();
    if (!observed.result.ok)
    {
        receipt.error =
            BackendError("execution start snapshot failed", observed.result);
        return receipt;
    }
    operation.start_movie_input_count = observed.movie_input_count;
    operation.health_last_vi = observed.vi_count;
    (void)impl_->host_activity->DrainCompletedActivities();
    operation.health_host_generation =
        impl_->host_activity->snapshot().generation;
    impl_->active.emplace(std::move(operation));
    impl_->LogOperationStart(*impl_->active, observed);
    receipt.accepted = true;
    receipt.operation_id = operation_id;

    if (ExecutionError start = impl_->StartOperation(*impl_->active))
    {
        const ExecutionTerminalStatus status =
            start.code == ExecutionErrorCode::Unsupported
            ? ExecutionTerminalStatus::Unsupported
            : ExecutionTerminalStatus::BackendFailure;
        impl_->BeginFinish(status, std::move(start));
    }
    else if (impl_->active && impl_->active->pending_terminal)
    {
        const ExecutionTerminalStatus status =
            *impl_->active->pending_terminal;
        ExecutionError error = impl_->active->pending_error
            ? std::move(*impl_->active->pending_error)
            : ExecutionError{};
        std::optional<StopRouteReceipt> stop =
            std::move(impl_->active->pending_stop);
        impl_->active->pending_terminal.reset();
        impl_->active->pending_error.reset();
        impl_->BeginFinish(
            status,
            std::move(error),
            std::move(stop));
    }
    else
    {
        const ExecutionObservation started = impl_->Query();
        if (!started.result.ok)
        {
            impl_->BeginFinish(
                ExecutionTerminalStatus::BackendFailure,
                BackendError(
                    "post-start execution snapshot failed",
                    started.result));
        }
        else
        {
            impl_->PublishState(&started);
        }
    }
    return receipt;
}

ExecutionSubmissionReceipt ExecutionControlCore::SubmitInterruptionChild(
    InterruptionFrameId frame_id,
    ExecutionRequest request)
{
    ExecutionSubmissionReceipt receipt;
    if (!impl_->OnOwnerThread())
    {
        receipt.error = Error(
            ExecutionErrorCode::WrongThread,
            "interruption child submitted outside the engine owner thread");
        return receipt;
    }
    if (impl_->handlers.empty() ||
        impl_->handlers.back().id != frame_id ||
        impl_->active)
    {
        receipt.error = Error(
            ExecutionErrorCode::InvalidState,
            "interruption frame is not awaiting a child operation");
        return receipt;
    }
    if (!impl_->handlers.back().pause_confirmed)
    {
        receipt.error = Error(
            ExecutionErrorCode::InvalidState,
            "interruption child requires an authoritatively paused core");
        return receipt;
    }
    Impl::SuspendedFrame& frame = impl_->handlers.back();
    impl_->PumpSuspendedHostActivityWarnings(frame.parent);
    if (CancellationOf(frame.parent.request)
            .is_cancellation_requested())
    {
        receipt.error = Error(
            ExecutionErrorCode::InvalidState,
            "interruption parent is already cancelled");
        return receipt;
    }
    const ExecutionOperationKind kind = KindOf(request);
    if (!ContainsKind(frame.descriptor.allowed_child_operations, kind))
    {
        receipt.error = Error(
            ExecutionErrorCode::InterruptionPolicyViolation,
            "interruption descriptor does not permit this child operation");
        return receipt;
    }
    if (ExecutionError validation = impl_->ValidateCommon(request))
    {
        receipt.error = std::move(validation);
        return receipt;
    }

    const ExecutionOperationId operation_id = impl_->NextOperationId();
    if (!operation_id)
    {
        receipt.error = Error(
            ExecutionErrorCode::InvalidState,
            "Execution operation IDs are exhausted",
            BackendIntegrity::Unknown);
        return receipt;
    }
    Impl::ActiveOperation child;
    child.id = operation_id;
    child.kind = kind;
    child.request = std::move(request);
    child.handler_owner = frame_id;
    child.started = impl_->now();
    child.health_baseline = child.started;
    const ExecutionObservation observed = impl_->Query();
    if (!observed.result.ok)
    {
        receipt.error = BackendError(
            "interruption child snapshot failed",
            observed.result);
        return receipt;
    }
    if (observed.core_state != BackendCoreState::Paused ||
        !observed.pause_confirmed)
    {
        receipt.error = Error(
            ExecutionErrorCode::InvalidState,
            "interruption child requires an authoritatively paused core");
        return receipt;
    }
    child.health_last_vi = observed.vi_count;
    child.health_host_generation =
        impl_->host_activity->snapshot().generation;
    impl_->active.emplace(std::move(child));
    receipt.accepted = true;
    receipt.operation_id = operation_id;
    if (ExecutionError start = impl_->StartOperation(*impl_->active))
    {
        impl_->BeginFinish(
            start.code == ExecutionErrorCode::Unsupported
                ? ExecutionTerminalStatus::Unsupported
                : ExecutionTerminalStatus::BackendFailure,
            std::move(start));
    }
    else
    {
        const ExecutionObservation started = impl_->Query();
        if (!started.result.ok)
        {
            impl_->BeginFinish(
                ExecutionTerminalStatus::BackendFailure,
                BackendError(
                    "post-start interruption child snapshot failed",
                    started.result));
        }
        else
        {
            impl_->PublishState(&started);
        }
    }
    return receipt;
}

ExecutionControlReceipt ExecutionControlCore::Cancel(CancellationReason reason)
{
    ExecutionControlReceipt receipt;
    if (!impl_->OnOwnerThread())
    {
        receipt.error = Error(
            ExecutionErrorCode::WrongThread,
            "ExecutionControlCore cancelled outside its owner thread");
        return receipt;
    }
    if (!impl_->active && impl_->handlers.empty())
    {
        receipt.error = Error(
            ExecutionErrorCode::InvalidState,
            "ExecutionControlCore has no active operation");
        return receipt;
    }
    receipt.accepted = true;
    receipt.operation_id = impl_->active
        ? impl_->active->id
        : impl_->handlers.back().parent.id;
    (void)reason;
    if (!impl_->active && !impl_->handlers.empty())
    {
        Impl::SuspendedFrame anchor =
            std::move(impl_->handlers.back());
        impl_->handlers.pop_back();
        impl_->active.emplace(std::move(anchor.parent));
    }
    while (!impl_->handlers.empty())
    {
        impl_->pending_parent_terminals.push_back({
            std::move(impl_->handlers.back().parent),
            ExecutionTerminalStatus::Cancelled,
            {}});
        impl_->handlers.pop_back();
    }
    if (impl_->active)
    {
        impl_->BeginFinish(ExecutionTerminalStatus::Cancelled);
    }
    return receipt;
}

ExecutionControlReceipt ExecutionControlCore::CompleteInterruptionHandler(
    InterruptionFrameId frame_id,
    InterruptionHandlerOutcome outcome,
    std::string diagnostic)
{
    ExecutionControlReceipt receipt;
    if (!impl_->OnOwnerThread())
    {
        receipt.error = Error(
            ExecutionErrorCode::WrongThread,
            "interruption handler completed outside the engine owner thread");
        return receipt;
    }
    if (impl_->handlers.empty() ||
        impl_->handlers.back().id != frame_id ||
        impl_->active)
    {
        receipt.error = Error(
            ExecutionErrorCode::InvalidState,
            "interruption frame is not awaiting completion");
        return receipt;
    }
    if (!impl_->handlers.back().pause_confirmed)
    {
        receipt.error = Error(
            ExecutionErrorCode::InvalidState,
            "interruption completion requires an authoritatively paused core");
        return receipt;
    }
    impl_->PumpSuspendedHostActivityWarnings(
        impl_->handlers.back().parent);

    Impl::SuspendedFrame frame =
        std::move(impl_->handlers.back());
    impl_->handlers.pop_back();
    receipt.accepted = true;
    receipt.operation_id = frame.parent.id;
    if (outcome != InterruptionHandlerOutcome::ResumeParent)
    {
        impl_->active.emplace(std::move(frame.parent));
        impl_->BeginFinish(
            outcome == InterruptionHandlerOutcome::AbortParent
                ? ExecutionTerminalStatus::InterruptionAborted
                : ExecutionTerminalStatus::InterruptionFailed,
            Error(
                outcome == InterruptionHandlerOutcome::AbortParent
                    ? ExecutionErrorCode::InterruptionPolicyViolation
                    : ExecutionErrorCode::BackendFailure,
                diagnostic.empty()
                    ? (outcome == InterruptionHandlerOutcome::AbortParent
                          ? "interruption handler aborted its parent"
                          : "interruption handler infrastructure failed")
                    : std::move(diagnostic),
                outcome == InterruptionHandlerOutcome::InfrastructureFailure
                    ? BackendIntegrity::Unknown
                    : BackendIntegrity::Preserved));
        return receipt;
    }

    if (CancellationOf(frame.parent.request)
            .is_cancellation_requested())
    {
        impl_->active.emplace(std::move(frame.parent));
        impl_->BeginFinish(ExecutionTerminalStatus::Cancelled);
        return receipt;
    }
    frame.parent.health_eligible = false;
    frame.parent.health_baseline = impl_->now();
    frame.parent.stall_suspected = false;
    impl_->active.emplace(std::move(frame.parent));

    ExecutionError resumed =
        impl_->UnparkWakeGroup(*impl_->active);
    if (!resumed)
    {
        if (const auto relationship =
                InputRelationshipOf(impl_->active->request))
        {
            try
            {
                const InputExecutionRelationshipOperationReceipt validation =
                    impl_->config.input_relationships->Validate(
                        *relationship,
                        impl_->epoch);
                if (!validation.ok)
                {
                    resumed = Error(
                        ExecutionErrorCode::InputUnavailable,
                        validation.message.empty()
                            ? "resumed input relationship is invalid"
                            : validation.message);
                }
            }
            catch (const std::exception& ex)
            {
                resumed = Error(
                    ExecutionErrorCode::InputUnavailable,
                    std::string("resumed input validation threw: ") +
                        ex.what(),
                    BackendIntegrity::Unknown);
            }
            catch (...)
            {
                resumed = Error(
                    ExecutionErrorCode::InputUnavailable,
                    "resumed input validation threw",
                    BackendIntegrity::Unknown);
            }
        }
    }
    if (!resumed)
    {
        switch (impl_->active->kind)
        {
        case ExecutionOperationKind::ContinueUntil:
        case ExecutionOperationKind::ContinueUntilInputObserved:
        case ExecutionOperationKind::InteractiveResume:
            resumed = impl_->ResumeBackend();
            break;
        case ExecutionOperationKind::StepFrames:
            impl_->active->awaiting_advance = false;
            impl_->active->observed_running = false;
            resumed = impl_->BeginAdvance(*impl_->active);
            break;
        case ExecutionOperationKind::SafePause:
        {
            const BackendResult pause = impl_->SubmitBackendControl(
                "resumed safe pause",
                BackendControlCommandKind::Pause);
            if (!pause.ok)
                resumed = BackendError("resumed safe pause failed", pause);
            break;
        }
        }
    }
    if (resumed)
    {
        impl_->BeginFinish(
            resumed.code == ExecutionErrorCode::Unsupported
                ? ExecutionTerminalStatus::Unsupported
                : ExecutionTerminalStatus::BackendFailure,
            std::move(resumed));
    }
    else
    {
        const ExecutionObservation observed = impl_->Query();
        if (!observed.result.ok)
        {
            impl_->BeginFinish(
                ExecutionTerminalStatus::BackendFailure,
                BackendError(
                    "resumed parent snapshot failed",
                    observed.result));
        }
        else
        {
            impl_->PublishState(&observed);
        }
    }
    return receipt;
}

void ExecutionControlCore::HandleStopPointReceipt(StopRouteReceipt receipt)
{
    if (!impl_->OnOwnerThread() || !impl_->active)
    {
        return;
    }
    if (receipt.terminal == StopRouteTerminal::ForegroundMatched ||
        receipt.terminal == StopRouteTerminal::RoutingFailure ||
        receipt.terminal == StopRouteTerminal::Overflow)
    {
        const ExecutionRequestPolicy* policy =
            PolicyOf(impl_->active->request);
        SCLOGDX(
            SC_TAGS("execution.operation", "execution.stop"),
            "operation=%llu kind=%s epoch=%llu invocation=%llu attempt=%llu request=%llu selector=%s terminal=%u routed_sequence=%llu hit_pc=0x%08X awaited_pcs=%s",
            impl_->active->id.value(), KindName(impl_->active->kind),
            impl_->epoch.value(),
            policy ? policy->diagnostic_invocation : 0,
            policy ? policy->diagnostic_attempt : 0,
            policy ? policy->diagnostic_request : 0,
            policy && !policy->diagnostic_selector.empty()
                ? policy->diagnostic_selector.c_str()
                : "<unnamed>",
            static_cast<unsigned>(receipt.terminal),
            receipt.identity.sequence.value(),
            receipt.event ? receipt.event->evidence.hit_pc : 0,
            AwaitedPcs(impl_->active->request).c_str());
    }
    if (receipt.terminal == StopRouteTerminal::ForegroundMatched)
    {
        static std::atomic<std::uint64_t> next_stop_transition{1};
        receipt.stop_transition_id =
            next_stop_transition.fetch_add(1, std::memory_order_relaxed);
        if (receipt.execution_operation_id != 0 &&
            receipt.execution_operation_id != impl_->active->id.value())
        {
            impl_->BeginFinish(
                ExecutionTerminalStatus::BackendFailure,
                Error(
                    ExecutionErrorCode::StopPointFailure,
                    "foreground stop belongs to a different execution operation"),
                std::move(receipt));
            return;
        }
    }
    if (impl_->active->pending_terminal)
    {
        const ExecutionTerminalStatus pending =
            *impl_->active->pending_terminal;
        if (receipt.terminal == StopRouteTerminal::ForegroundMatched &&
            (pending == ExecutionTerminalStatus::CursorOverrun ||
             pending == ExecutionTerminalStatus::MovieEnded))
        {
            // A routed breakpoint is the more precise terminal observation.
            // Preserve cancellation and infrastructure terminals, but allow
            // an in-flight pause requested for coarse movie observation to
            // be superseded by exact routed evidence.
            impl_->active->pending_terminal =
                ExecutionTerminalStatus::RequestedCompletion;
            impl_->active->pending_error.reset();
            impl_->active->pending_stop = std::move(receipt);
        }
        return;
    }
    if (receipt.identity.workset_epoch &&
        receipt.identity.workset_epoch != impl_->epoch)
    {
        if (receipt.event && receipt.event->authoritative)
        {
            impl_->BeginFinish(
                ExecutionTerminalStatus::WorksetEpochMismatch,
                Error(
                    ExecutionErrorCode::WorksetEpochMismatch,
                    "authoritative stop receipt belongs to another WorksetEpoch"),
                std::move(receipt));
        }
        return;
    }

    switch (receipt.terminal)
    {
    case StopRouteTerminal::None:
        return;
    case StopRouteTerminal::Stale:
        if (receipt.core_must_remain_stopped ||
            (receipt.event && receipt.event->authoritative))
        {
            impl_->BeginFinish(
                ExecutionTerminalStatus::BackendFailure,
                Error(
                    ExecutionErrorCode::StopPointFailure,
                    receipt.error.message.empty()
                        ? "authoritative stop became stale before execution control consumed it"
                        : receipt.error.message),
                std::move(receipt));
        }
        return;
    case StopRouteTerminal::ForegroundMatched:
        impl_->BeginFinish(
            ExecutionTerminalStatus::RequestedCompletion,
            {},
            std::move(receipt));
        return;
    case StopRouteTerminal::Unclaimed:
        if (receipt.core_must_remain_stopped ||
            (receipt.event && receipt.event->authoritative))
        {
            impl_->BeginFinish(
                ExecutionTerminalStatus::UnexpectedStop,
                Error(
                    ExecutionErrorCode::StopPointFailure,
                    receipt.error.message.empty()
                        ? "an authoritative physical stop was unclaimed"
                        : receipt.error.message),
                std::move(receipt));
        }
        return;
    case StopRouteTerminal::RoutingFailure:
        impl_->BeginFinish(
            ExecutionTerminalStatus::BackendFailure,
            Error(
                ExecutionErrorCode::StopPointFailure,
                receipt.error.message.empty()
                    ? "stop-point routing failed"
                    : receipt.error.message),
            std::move(receipt));
        return;
    case StopRouteTerminal::Overflow:
        impl_->BeginFinish(
            ExecutionTerminalStatus::BackendFailure,
            Error(
                ExecutionErrorCode::StopPointFailure,
                receipt.error.message.empty()
                    ? "authoritative stop-point ingress overflowed"
                    : receipt.error.message,
                BackendIntegrity::Unknown),
            std::move(receipt));
        return;
    case StopRouteTerminal::InterruptionRequested:
        break;
    }

    if (!receipt.interruption_handler_request)
    {
        impl_->BeginFinish(
            ExecutionTerminalStatus::InterruptionFailed,
            Error(
                ExecutionErrorCode::StopPointFailure,
                "router requested an interruption without a typed request"),
            std::move(receipt));
        return;
    }
    if (InterruptionPolicyOf(impl_->active->request) !=
        ExecutionInterruptionPolicy::AllowKnown)
    {
        impl_->BeginFinish(
            ExecutionTerminalStatus::InterruptionUnavailable,
            Error(
                ExecutionErrorCode::InterruptionPolicyViolation,
                "active execution operation rejects interruption handlers"),
            std::move(receipt));
        return;
    }

    const std::string& key =
        receipt.interruption_handler_request->interruption_handler_key;
    const auto descriptor = impl_->handler_registry.find(key);
    if (descriptor == impl_->handler_registry.end())
    {
        impl_->BeginFinish(
            ExecutionTerminalStatus::InterruptionUnavailable,
            Error(
                ExecutionErrorCode::InterruptionUnavailable,
                "no trusted interruption handler is registered for key " + key),
            std::move(receipt));
        return;
    }
    if (impl_->handlers.size() >= 8 ||
        impl_->handlers.size() >= descriptor->second.maximum_depth)
    {
        impl_->BeginFinish(
            ExecutionTerminalStatus::InterruptionDepthExceeded,
            Error(
                ExecutionErrorCode::InterruptionDepthExceeded,
                "interruption-handler depth limit was reached"),
            std::move(receipt));
        return;
    }
    if (!impl_->handlers.empty())
    {
        const InterruptionHandlerDescriptor& parent =
            impl_->handlers.back().descriptor;
        const bool self = parent.key == key;
        const bool declared =
            std::find(
                parent.permitted_nested_keys.begin(),
                parent.permitted_nested_keys.end(),
                key) != parent.permitted_nested_keys.end();
        if ((self && !parent.allow_self_recursion) || (!self && !declared))
        {
            impl_->BeginFinish(
                ExecutionTerminalStatus::InterruptionUnavailable,
                Error(
                    ExecutionErrorCode::InterruptionPolicyViolation,
                    "nested interruption is not declared by its parent"),
                std::move(receipt));
            return;
        }
    }

    if (ExecutionError parked =
            impl_->ParkWakeGroup(*impl_->active))
    {
        impl_->BeginFinish(
            ExecutionTerminalStatus::InterruptionFailed,
            std::move(parked),
            std::move(receipt));
        return;
    }

    const InterruptionFrameId frame_id = impl_->NextFrameId();
    if (!frame_id)
    {
        impl_->BeginFinish(
            ExecutionTerminalStatus::InterruptionFailed,
            Error(
                ExecutionErrorCode::InvalidState,
                "interruption frame IDs are exhausted",
                BackendIntegrity::Unknown),
            std::move(receipt));
        return;
    }
    const auto suspended_at = impl_->now();
    Impl::ActiveOperation parent = std::move(*impl_->active);
    parent.health_eligible = false;
    parent.health_baseline = suspended_at;
    parent.stall_suspected = false;
    impl_->active.reset();
    impl_->handlers.push_back({
        frame_id,
        descriptor->second,
        std::move(parent),
        false,
        suspended_at + impl_->config.pause_confirmation_timeout});
    ExecutionObservation observed = impl_->Query();
    if (!observed.result.ok)
    {
        Impl::SuspendedFrame failed =
            std::move(impl_->handlers.back());
        impl_->handlers.pop_back();
        impl_->active.emplace(std::move(failed.parent));
        impl_->BeginFinish(
            ExecutionTerminalStatus::CleanupFailure,
            BackendError(
                "interruption pause-confirmation snapshot failed",
                observed.result),
            std::move(receipt));
        return;
    }
    if (observed.core_state == BackendCoreState::Paused &&
        observed.pause_confirmed)
    {
        impl_->handlers.back().pause_confirmed = true;
        impl_->handlers.back().pause_confirmation_deadline.reset();
        impl_->PublishState(&observed);
        return;
    }

    const BackendResult pause = impl_->SubmitBackendControl(
        "interruption pause confirmation",
        BackendControlCommandKind::Pause);
    if (!pause.ok)
    {
        Impl::SuspendedFrame failed =
            std::move(impl_->handlers.back());
        impl_->handlers.pop_back();
        impl_->active.emplace(std::move(failed.parent));
        impl_->BeginFinish(
            ExecutionTerminalStatus::InterruptionFailed,
            BackendError(
                "interruption pause request failed",
                pause),
            std::move(receipt));
        return;
    }
    observed = impl_->Query();
    if (!observed.result.ok)
    {
        Impl::SuspendedFrame failed =
            std::move(impl_->handlers.back());
        impl_->handlers.pop_back();
        impl_->active.emplace(std::move(failed.parent));
        impl_->BeginFinish(
            ExecutionTerminalStatus::CleanupFailure,
            BackendError(
                "post-request interruption pause snapshot failed",
                observed.result),
            std::move(receipt));
        return;
    }
    if (observed.core_state == BackendCoreState::Paused &&
        observed.pause_confirmed)
    {
        impl_->handlers.back().pause_confirmed = true;
        impl_->handlers.back().pause_confirmation_deadline.reset();
    }
    impl_->PublishState(&observed);
}

void ExecutionControlCore::Pump()
{
    if (!impl_->OnOwnerThread() || !impl_->initialized ||
        impl_->stopping)
    {
        return;
    }
    const auto current = impl_->now();
    const auto drain_authoritative_ingress = [this] {
        for (StopRouteReceipt& receipt : impl_->stop_points.DrainIngress())
            HandleStopPointReceipt(std::move(receipt));
    };

    // Native stop packets are published before CPU::Break. Consume anything
    // already authoritative before observing Dolphin, then recheck after the
    // observation so a stop crossing this boundary cannot be misclassified as
    // an unexplained pause.
    drain_authoritative_ingress();

    const bool suspended_parent_cancelled = std::any_of(
        impl_->handlers.begin(),
        impl_->handlers.end(),
        [](const Impl::SuspendedFrame& frame) {
            return CancellationOf(frame.parent.request)
                .is_cancellation_requested();
        });
    if (suspended_parent_cancelled)
    {
        (void)Cancel(CancellationReason::ExternalRequest);
        return;
    }

    if (!impl_->active)
    {
        if (impl_->handlers.empty())
            return;
        const ExecutionObservation observed = impl_->Query();
        if (!observed.result.ok)
        {
            Impl::SuspendedFrame failed =
                std::move(impl_->handlers.back());
            impl_->handlers.pop_back();
            impl_->active.emplace(std::move(failed.parent));
            impl_->BeginFinish(
                ExecutionTerminalStatus::CleanupFailure,
                BackendError(
                    "interruption maintenance snapshot failed",
                    observed.result));
            return;
        }
        Impl::SuspendedFrame& frame = impl_->handlers.back();
        impl_->PumpSuspendedHostActivityWarnings(frame.parent);
        if (!frame.pause_confirmed)
        {
            if (observed.core_state == BackendCoreState::Paused &&
                observed.pause_confirmed)
            {
                frame.pause_confirmed = true;
                frame.pause_confirmation_deadline.reset();
            }
            else if (frame.pause_confirmation_deadline &&
                current >= *frame.pause_confirmation_deadline)
            {
                Impl::SuspendedFrame failed = std::move(frame);
                impl_->handlers.pop_back();
                impl_->active.emplace(std::move(failed.parent));
                impl_->BeginFinish(
                    ExecutionTerminalStatus::InterruptionFailed,
                    Error(
                        ExecutionErrorCode::BackendFailure,
                        "interruption pause confirmation exceeded its host-operation bound",
                        BackendIntegrity::Unknown));
                return;
            }
        }
        const ExecutionEnvironmentEvidence evidence =
            ConvertEvidence(observed);
        if (impl_->snapshot.evidence.core_state != evidence.core_state ||
            impl_->snapshot.evidence.pause_confirmed !=
                evidence.pause_confirmed ||
            impl_->snapshot.evidence.vi_count != evidence.vi_count ||
            impl_->snapshot.evidence.movie_state !=
                evidence.movie_state ||
            impl_->snapshot.evidence.throttle_disabled !=
                evidence.throttle_disabled)
        {
            impl_->PublishState(&observed);
        }
        return;
    }

    const ExecutionOperationId observed_operation = impl_->active->id;
    ExecutionObservation observed = impl_->Query();
    if (!observed.result.ok)
    {
        Impl::ActiveOperation failed = std::move(*impl_->active);
        impl_->active.reset();
        (void)impl_->SubmitBackendControl(
            "execution emergency pause after maintenance snapshot failure",
            BackendControlCommandKind::Pause);
        impl_->EmitTerminal(
            std::move(failed),
            ExecutionTerminalStatus::CleanupFailure,
            BackendError(
                "execution maintenance snapshot failed",
                observed.result),
            std::nullopt,
            &observed);
        return;
    }
    drain_authoritative_ingress();
    if (!impl_->active || impl_->active->id != observed_operation)
        return;
    Impl::ActiveOperation& operation = *impl_->active;
    impl_->LogHeartbeat(operation, observed, current);

    if (operation.pending_terminal)
    {
        if (observed.core_state == BackendCoreState::Paused &&
            observed.pause_confirmed)
        {
            Impl::ActiveOperation finished = std::move(operation);
            ExecutionTerminalStatus status =
                *finished.pending_terminal;
            ExecutionError error =
                finished.pending_error.value_or(ExecutionError{});
            if (finished.kind ==
                    ExecutionOperationKind::ContinueUntilInputObserved &&
                status == ExecutionTerminalStatus::InputObserved)
            {
                const auto& request =
                    std::get<ContinueUntilInputObservedRequest>(
                        finished.request);
                if (observed.movie_state != MovieState::Recording ||
                    observed.movie_input_count <=
                        request.expected_movie_input_count)
                {
                    status = ExecutionTerminalStatus::BackendFailure;
                    error = Error(
                        ExecutionErrorCode::InvalidState,
                        "observed input did not advance the recording beyond the checkpoint cursor");
                }
            }
            impl_->ApplyCoreStallHealthProof(status, error);
            std::optional<StopRouteReceipt> stop =
                std::move(finished.pending_stop);
            impl_->EmitTerminal(
                std::move(finished),
                status,
                std::move(error),
                std::move(stop),
                &observed);
        }
        else if (operation.pause_control_deadline &&
            current >= *operation.pause_control_deadline)
        {
            Impl::ActiveOperation failed = std::move(operation);
            const std::optional<StopRouteReceipt> stop =
                std::move(failed.pending_stop);
            ExecutionError timeout_error =
                failed.kind == ExecutionOperationKind::SafePause &&
                    failed.pending_error
                ? std::move(*failed.pending_error)
                : Error(
                      ExecutionErrorCode::BackendFailure,
                      "Dolphin pause confirmation exceeded its cleanup bound",
                      BackendIntegrity::Unknown);
            timeout_error.integrity = BackendIntegrity::Unknown;
            impl_->EmitTerminal(
                std::move(failed),
                ExecutionTerminalStatus::CleanupFailure,
                std::move(timeout_error),
                stop,
                &observed);
        }
        return;
    }

    const CancellationToken cancellation =
        CancellationOf(operation.request);
    if (cancellation.is_cancellation_requested())
    {
        impl_->BeginFinish(ExecutionTerminalStatus::Cancelled);
        return;
    }
    if (operation.kind == ExecutionOperationKind::SafePause &&
        operation.pause_control_deadline &&
        current >= *operation.pause_control_deadline)
    {
        impl_->BeginFinish(
            ExecutionTerminalStatus::CleanupFailure,
            Error(
                ExecutionErrorCode::BackendFailure,
                "Dolphin pause confirmation exceeded its host-operation bound",
                BackendIntegrity::Unknown));
        return;
    }

    if (operation.kind == ExecutionOperationKind::ContinueUntil)
    {
        const auto& request =
            std::get<ContinueUntilRequest>(operation.request);
        if (request.expected_movie_input_count &&
            observed.movie_input_count >
                *request.expected_movie_input_count)
        {
            impl_->BeginFinish(
                ExecutionTerminalStatus::CursorOverrun);
            return;
        }
    }

    if (const ExecutionRequestPolicy* policy =
            PolicyOf(operation.request))
    {
        if (policy->movie_ended != MovieEndedPolicy::Ignore &&
            observed.movie_state == MovieState::PlaybackEnded)
        {
            impl_->BeginFinish(
                ExecutionTerminalStatus::MovieEnded,
                policy->movie_ended == MovieEndedPolicy::Fail
                    ? Error(
                          ExecutionErrorCode::InvalidState,
                          "movie ended before the requested completion")
                    : ExecutionError{});
            return;
        }
    }

    const bool resume_driven =
        operation.kind == ExecutionOperationKind::ContinueUntil ||
        operation.kind == ExecutionOperationKind::ContinueUntilInputObserved ||
        operation.kind == ExecutionOperationKind::InteractiveResume;
    if (resume_driven && !operation.observed_running &&
        observed.core_state == BackendCoreState::Paused)
    {
        // SetState(Running) and the CPU run loop do not become observable as
        // one atomic host transition. A paused observation immediately after
        // resume still belongs to that admission boundary; it cannot authorize
        // a newer pause command or an unexpected-stop terminal.
        operation.health_eligible = false;
        impl_->RebaselineHealth(operation, observed, current);
        impl_->PublishState(&observed);
        return;
    }

    if (operation.kind != ExecutionOperationKind::SafePause &&
        observed.core_state == BackendCoreState::Paused &&
        !observed.pause_confirmed)
    {
        operation.health_eligible = false;
        impl_->RebaselineHealth(operation, observed, current);
        if (!operation.unconfirmed_pause_deadline)
        {
            // Dolphin's pause-at-movie-end path pauses the core without going
            // through SAVOR's pause synchronizer. Confirm that already-paused
            // state before MovieService performs its one paused inspection.
            // This requests no movie observation and does not resume the core.
            const BackendResult pause = impl_->SubmitBackendControl(
                "confirm externally paused Dolphin core",
                BackendControlCommandKind::Pause);
            if (!pause.ok)
            {
                impl_->BeginFinish(
                    ExecutionTerminalStatus::BackendFailure,
                    BackendError(
                        "failed confirming externally paused Dolphin core",
                        pause));
                return;
            }
            operation.unconfirmed_pause_deadline =
                current + impl_->config.pause_confirmation_timeout;
        }
        else if (current >= *operation.unconfirmed_pause_deadline)
        {
            const BackendHealthReport health =
                impl_->CheckBackendHealth();
            impl_->BeginFinish(
                ExecutionTerminalStatus::BackendFailure,
                Error(
                    ExecutionErrorCode::BackendFailure,
                    health.diagnostic.empty()
                        ? "execution backend did not confirm its paused state"
                        : "execution backend did not confirm its paused state: " +
                            health.diagnostic,
                    BackendIntegrity::Unknown));
        }
        return;
    }
    operation.unconfirmed_pause_deadline.reset();

    if (operation.kind != ExecutionOperationKind::SafePause &&
        observed.core_state != BackendCoreState::Running &&
        observed.core_state != BackendCoreState::Paused)
    {
        const BackendHealthReport health =
            impl_->CheckBackendHealth();
        impl_->BeginFinish(
            ExecutionTerminalStatus::BackendFailure,
            Error(
                ExecutionErrorCode::BackendFailure,
                health.diagnostic.empty()
                    ? "execution backend left Running without an authoritative pause"
                    : "execution backend left Running: " +
                        health.diagnostic,
                BackendIntegrity::Unknown));
        return;
    }

    if (observed.core_state == BackendCoreState::Running)
        operation.observed_running = true;
    if (impl_->PumpCoreHealth(operation, observed, current))
        return;

    switch (operation.kind)
    {
    case ExecutionOperationKind::SafePause:
        if (observed.core_state == BackendCoreState::Paused &&
            observed.pause_confirmed)
            impl_->BeginFinish(ExecutionTerminalStatus::Paused);
        break;
    case ExecutionOperationKind::StepFrames:
        if (impl_->AdvanceCompleted(operation, observed))
            impl_->CompleteAdvance(observed);
        break;
    case ExecutionOperationKind::ContinueUntil:
    case ExecutionOperationKind::InteractiveResume:
        if (observed.core_state == BackendCoreState::Paused &&
            observed.pause_confirmed)
        {
            const std::uint64_t incident = impl_->LogUnroutedPause(
                operation,
                observed,
                "no_routed_completion");
            impl_->BeginFinish(
                ExecutionTerminalStatus::UnexpectedStop,
                Error(
                    ExecutionErrorCode::BackendFailure,
                    "Dolphin paused without a routed completion; incident=" +
                        std::to_string(incident)));
        }
        break;
    case ExecutionOperationKind::ContinueUntilInputObserved:
    {
        const auto relationship = InputRelationshipOf(operation.request);
        const InputExecutionRelationshipInspection inspection =
            relationship && impl_->config.input_relationships
            ? impl_->config.input_relationships->Inspect(
                  *relationship,
                  impl_->epoch)
            : InputExecutionRelationshipInspection{};
        if (!inspection.ok)
        {
            impl_->BeginFinish(
                ExecutionTerminalStatus::BackendFailure,
                Error(
                    ExecutionErrorCode::InputUnavailable,
                    inspection.message.empty()
                        ? "input-observation inspection failed"
                        : inspection.message));
            break;
        }
        if (inspection.exact_publication_observed)
        {
            impl_->BeginFinish(ExecutionTerminalStatus::InputObserved);
            break;
        }
        if (observed.core_state == BackendCoreState::Paused &&
            observed.pause_confirmed)
        {
            const std::uint64_t incident = impl_->LogUnroutedPause(
                operation,
                observed,
                "input_publication_not_observed");
            impl_->BeginFinish(
                ExecutionTerminalStatus::UnexpectedStop,
                Error(
                    ExecutionErrorCode::BackendFailure,
                    "Dolphin paused before the exact input publication was observed; incident=" +
                        std::to_string(incident)));
        }
        break;
    }
    }
}

std::vector<ExecutionEvent> ExecutionControlCore::DrainEvents()
{
    if (!impl_->OnOwnerThread())
        return {};
    std::vector<ExecutionEvent> events;
    events.swap(impl_->events);
    return events;
}

ExecutionSnapshot ExecutionControlCore::snapshot() const
{
    return impl_->snapshot;
}

bool ExecutionControlCore::has_active_operation() const noexcept
{
    return impl_->active.has_value() || !impl_->handlers.empty();
}

std::optional<Clock::time_point> ExecutionControlCore::next_wake() const
{
    if (impl_->stopping ||
        (!impl_->active && impl_->handlers.empty()))
        return std::nullopt;
    const Clock::time_point maintenance =
        impl_->now() + impl_->config.maintenance_interval;
    Clock::time_point wake = maintenance;
    if (impl_->active && impl_->active->pause_control_deadline)
    {
        wake = std::min(
            wake,
            *impl_->active->pause_control_deadline);
    }
    if (impl_->active &&
        impl_->active->unconfirmed_pause_deadline)
    {
        wake = std::min(
            wake,
            *impl_->active->unconfirmed_pause_deadline);
    }
    if (!impl_->handlers.empty() &&
        !impl_->handlers.back().pause_confirmed &&
        impl_->handlers.back().pause_confirmation_deadline)
    {
        wake = std::min(
            wake,
            *impl_->handlers.back().pause_confirmation_deadline);
    }
    return wake;
}


BackendResult ExecutionControlCore::Shutdown()
{
    if (!impl_->OnOwnerThread())
    {
        return BackendResult::Failure(
            BackendErrorCode::InvalidState,
            "ExecutionControlCore shutdown called outside its owner thread");
    }
    if (impl_->stopping)
        return BackendResult::Success();

    BackendResult result = BackendResult::Success();
    if (impl_->active || !impl_->handlers.empty())
    {
        (void)Cancel(CancellationReason::Shutdown);
    }

    ExecutionObservation observed = impl_->Query();
    if (!observed.result.ok ||
        observed.core_state != BackendCoreState::Paused ||
        !observed.pause_confirmed)
    {
        const BackendResult pause = impl_->SubmitBackendControl(
            "execution shutdown pause",
            BackendControlCommandKind::Pause);
        if (!pause.ok)
            result = pause;
        observed = impl_->Query();
    }

    if (impl_->active)
    {
        Impl::ActiveOperation operation = std::move(*impl_->active);
        impl_->active.reset();
        const bool confirmed = observed.result.ok &&
            observed.core_state == BackendCoreState::Paused &&
            observed.pause_confirmed;
        if (!confirmed)
        {
            for (Impl::PendingParentTerminal& parent :
                impl_->pending_parent_terminals)
            {
                parent.status = ExecutionTerminalStatus::CleanupFailure;
                parent.error = Error(
                    ExecutionErrorCode::BackendFailure,
                    "shutdown could not confirm Dolphin paused",
                    BackendIntegrity::Unknown);
            }
        }
        impl_->EmitTerminal(
            std::move(operation),
            confirmed
                ? ExecutionTerminalStatus::Cancelled
                : ExecutionTerminalStatus::CleanupFailure,
            confirmed
                ? ExecutionError{}
                : Error(
                      ExecutionErrorCode::BackendFailure,
                      "shutdown could not confirm Dolphin paused",
                      BackendIntegrity::Unknown),
            std::nullopt,
            &observed);
        if (!confirmed)
        {
            result = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "shutdown could not confirm Dolphin paused",
                BackendIntegrity::Unknown);
        }
    }

    while (!impl_->handlers.empty())
    {
        Impl::ActiveOperation parent =
            std::move(impl_->handlers.back().parent);
        impl_->handlers.pop_back();
        impl_->EmitTerminal(
            std::move(parent),
            ExecutionTerminalStatus::CleanupFailure,
            Error(
                ExecutionErrorCode::BackendFailure,
                "shutdown found an unterminated interruption parent",
                BackendIntegrity::Unknown),
            std::nullopt,
            &observed);
        result = BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            "shutdown found an unterminated interruption parent",
            BackendIntegrity::Unknown);
    }

    if (!observed.result.ok ||
        observed.core_state != BackendCoreState::Paused ||
        !observed.pause_confirmed)
    {
        result = BackendResult::Failure(
            BackendErrorCode::OperationFailed,
            observed.result.message.empty()
                ? "ExecutionControlCore shutdown did not reach a confirmed pause"
                : observed.result.message,
            BackendIntegrity::Unknown);
    }

    impl_->stopping = true;
    impl_->initialized = false;
    impl_->snapshot.activity = ExecutionActivity::Closed;
    impl_->snapshot.active_operation.reset();
    impl_->snapshot.active_interruption_frame.reset();
    impl_->events.push_back({
        ExecutionEventKind::StateChanged,
        impl_->snapshot,
        std::nullopt});
    return result;
}

void ExecutionControlCore::OnStopPoint(const StopDelivery&)
{
    // StopPointRouter invokes consumers on the actor while constructing the
    // authoritative route receipt. ExecutionControlCore consumes that complete
    // receipt through HandleStopPointReceipt so policy is evaluated once.
}

} // namespace savor::runtime
