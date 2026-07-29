#pragma once

#include "EmulationSession.h"
#include "IProgramRuntimePort.h"
#include "RuntimeTypes.h"
#include "Worksets/WorksetTypes.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace savor::runtime {

class ProgramBaselineComponentRegistry;

struct OpenSessionCommand
{
    SessionOpenOptions options;
};

struct PrepareModuleCommand
{
    EncodedModuleEnvelope module;
};

struct InvokeProgramCommand
{
    EncodedInvocationEnvelope invocation;
};

struct CancelInvocationCommand
{
    InvocationId invocation_id;
};

struct SubmitWorksetCommand
{
    WorkerWorksetDefinition definition;
};

struct CancelWorksetItemCommand
{
    WorkerWorksetId workset_id;
    WorkerWorksetItemId item_id;
};

struct CancelWorksetCommand
{
    WorkerWorksetId workset_id;
};

struct AcknowledgeTerminalCommand
{
    WorkerItemTerminalCorrelation correlation;
};

struct CaptureScreenshotCommand
{
    SessionId session_id;
    std::filesystem::path output_path;
    std::chrono::milliseconds timeout{3000};
};

enum class WorkerExecutionControlKind : std::uint8_t
{
    Pause,
    Resume,
    StepFrame,
};

struct ControlExecutionCommand
{
    WorkerExecutionControlKind control = WorkerExecutionControlKind::Pause;
    SessionId session_id;
    StateEpoch expected_state_epoch;
    std::uint32_t count = 0;
    std::chrono::milliseconds timeout{};
};

struct ShutdownCommand
{
};

using WorkerCommand = std::variant<
    OpenSessionCommand,
    PrepareModuleCommand,
    InvokeProgramCommand,
    SubmitWorksetCommand,
    CancelInvocationCommand,
    CancelWorksetItemCommand,
    CancelWorksetCommand,
    AcknowledgeTerminalCommand,
    CaptureScreenshotCommand,
    ControlExecutionCommand,
    ShutdownCommand>;

struct WorkerSnapshot
{
    WorkerState state = WorkerState::Starting;
    WorkerCapabilityMask capabilities = 0;
    SessionSnapshot session;
    ExecutionSnapshot execution;
    std::optional<InvocationId> active_invocation;
    std::optional<WorkerWorksetId> active_workset;
    std::optional<WorkerWorksetId> staged_workset;
    std::optional<WorkerWorksetItemId> active_workset_item;
    std::uint32_t available_item_credits = 0;
    std::uint32_t retained_terminal_count = 0;
    std::size_t retained_terminal_bytes = 0;
    WorkerCommandSequence last_command_sequence;
};

struct WorkerWorksetStateEvent
{
    WorkerOutboundSequence outbound_sequence;
    WorkerWorksetId workset_id;
    WorkerWorksetState state = WorkerWorksetState::Validating;
    std::uint32_t next_item_ordinal = 0;
    RuntimeError error;
};

struct WorkerWorksetItemStartedEvent
{
    WorkerOutboundSequence outbound_sequence;
    WorkerWorksetId workset_id;
    WorkerWorksetItemId item_id;
    std::uint32_t item_ordinal = 0;
    InvocationId invocation_id;
    AttemptId attempt_id;
    SessionId session_id;
    StateEpoch state_epoch;
    PreparedProgramBaselineReceipt baseline;
};

struct WorkerWorksetItemTerminalEvent
{
    WorkerOutboundSequence outbound_sequence;
    WorkerItemTerminalCorrelation correlation;
    ProgramInvocationTerminalEvent terminal;
    bool unstarted = false;
};

struct WorkerWorksetCreditEvent
{
    WorkerOutboundSequence outbound_sequence;
    std::uint32_t available_item_credits = 0;
    std::uint32_t active_and_staged_items = 0;
    std::uint32_t retained_terminals = 0;
};

struct WorkerWorksetTerminalSummaryEvent
{
    WorkerOutboundSequence outbound_sequence;
    WorkerWorksetId workset_id;
    std::uint32_t item_count = 0;
    std::uint32_t completed_count = 0;
    std::uint32_t unstarted_count = 0;
};

struct WorkerCommandResult
{
    WireRequestId request_id;
    WorkerCommandSequence command_sequence;
    WorkerCommandKind command_kind = WorkerCommandKind::Shutdown;
    WorkerCommandOutcome outcome = WorkerCommandOutcome::Rejected;
    WorkerSnapshot snapshot;
    std::optional<InvocationId> invocation_id;
    std::optional<SessionOperationReceipt> session_receipt;
    std::optional<WorkerExecutionControlKind> execution_control;
    std::optional<ExecutionOperationId> execution_operation_id;
    std::optional<ExecutionTerminalResult> execution_terminal;
    RuntimeError error;
};

struct WorkerStateChangedEvent
{
    WorkerState previous = WorkerState::Starting;
    WorkerSnapshot current;
};

struct WorkerCommandCompletedEvent
{
    WorkerCommandResult result;
};

struct WorkerRuntimeDiagnosticEvent
{
    WorkerRejectionCode code = WorkerRejectionCode::InternalFailure;
    std::string message;
    std::optional<InvocationId> invocation_id;
};

struct HostRuntimeEvent
{
    HostEventSequence event_sequence;
    SessionId session_id;
    StateEpoch state_epoch;
    std::string name;
    std::vector<std::uint8_t> encoded_payload;
};

struct WorkerExecutionEvent
{
    SessionId session_id;
    ExecutionEvent event;
};

using WorkerEvent = std::variant<
    WorkerStateChangedEvent,
    WorkerCommandCompletedEvent,
    ModulePreparationEvent,
    ProgramInvocationProgressEvent,
    ProgramInvocationTerminalEvent,
    WorkerWorksetStateEvent,
    WorkerWorksetItemStartedEvent,
    WorkerWorksetItemTerminalEvent,
    WorkerWorksetCreditEvent,
    WorkerWorksetTerminalSummaryEvent,
    WorkerExecutionEvent,
    HostRuntimeEvent,
    WorkerRuntimeDiagnosticEvent>;

using WorkerEventSink = std::function<void(const WorkerEvent&)>;

struct WorkerRuntimeTestHooks
{
    // Test-only actor checkpoints used to make ingress/command races
    // deterministic without timing sleeps.
    std::function<void(EmulationSession&)> session_opened;
    std::function<void()> before_ingress_stability_check;
};

class WorkerRuntime final
{
public:
    WorkerRuntime(
        std::unique_ptr<EmulationSession> session,
        std::unique_ptr<IProgramRuntimePort> program_runtime = {},
        WorkerEventSink event_sink = {},
        std::shared_ptr<const WorkerRuntimeTestHooks> test_hooks = {},
        std::unique_ptr<program::IProgramActionHost> action_host = {},
        std::shared_ptr<ProgramBaselineComponentRegistry>
            baseline_components = {});
    ~WorkerRuntime();

    WorkerRuntime(const WorkerRuntime&) = delete;
    WorkerRuntime& operator=(const WorkerRuntime&) = delete;

    [[nodiscard]] std::future<WorkerCommandResult> Submit(
        WireRequestId request_id,
        WorkerCommand command);

    [[nodiscard]] WorkerSnapshot snapshot() const;
    [[nodiscard]] WorkerCapabilityMask capabilities() const noexcept;
    [[nodiscard]] WorkerRuntimeManifest runtime_manifest() const;
    [[nodiscard]] bool EnqueueHostEvent(
        std::string name,
        std::vector<std::uint8_t> encoded_payload);
    [[nodiscard]] bool EnqueueHostEvent(
        SessionId observed_session_id,
        StateEpoch observed_state_epoch,
        std::string name,
        std::vector<std::uint8_t> encoded_payload);
    // Await a previously submitted Shutdown command. This must not be called
    // reentrantly from WorkerEventSink, which runs on the actor publisher path.
    void WaitStopped();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::unique_ptr<WorkerRuntime> MakeProductionWorkerRuntime(
    SessionId session_id,
    WorkerEventSink event_sink = {});

} // namespace savor::runtime
