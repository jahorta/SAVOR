#pragma once

#include "EmulationSession.h"
#include "IProgramRuntimePort.h"
#include "RuntimeTypes.h"

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

struct CaptureScreenshotCommand
{
    SessionId session_id;
    std::filesystem::path output_path;
    std::chrono::milliseconds timeout{3000};
};

struct ShutdownCommand
{
};

using WorkerCommand = std::variant<
    OpenSessionCommand,
    PrepareModuleCommand,
    InvokeProgramCommand,
    CancelInvocationCommand,
    CaptureScreenshotCommand,
    ShutdownCommand>;

struct WorkerSnapshot
{
    WorkerState state = WorkerState::Starting;
    WorkerCapabilityMask capabilities = 0;
    SessionSnapshot session;
    std::optional<InvocationId> active_invocation;
    WorkerCommandSequence last_command_sequence;
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

using WorkerEvent = std::variant<
    WorkerStateChangedEvent,
    WorkerCommandCompletedEvent,
    ModulePreparationEvent,
    ProgramInvocationProgressEvent,
    ProgramInvocationTerminalEvent,
    HostRuntimeEvent,
    WorkerRuntimeDiagnosticEvent>;

using WorkerEventSink = std::function<void(const WorkerEvent&)>;

class WorkerRuntime final
{
public:
    WorkerRuntime(
        std::unique_ptr<EmulationSession> session,
        std::unique_ptr<IProgramRuntimePort> program_runtime = {},
        WorkerEventSink event_sink = {});
    ~WorkerRuntime();

    WorkerRuntime(const WorkerRuntime&) = delete;
    WorkerRuntime& operator=(const WorkerRuntime&) = delete;

    [[nodiscard]] std::future<WorkerCommandResult> Submit(
        WireRequestId request_id,
        WorkerCommand command);

    [[nodiscard]] WorkerSnapshot snapshot() const;
    [[nodiscard]] WorkerCapabilityMask capabilities() const noexcept;
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
