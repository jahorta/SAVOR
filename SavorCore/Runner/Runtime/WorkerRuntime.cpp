#include "WorkerRuntime.h"

#include "DolphinWrapperBackend.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace savor::runtime {
namespace {

template <class... Ts>
struct Overloaded : Ts...
{
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

[[nodiscard]] WorkerCommandKind CommandKind(const WorkerCommand& command)
{
    return std::visit(
        Overloaded{
            [](const OpenSessionCommand&) { return WorkerCommandKind::OpenSession; },
            [](const PrepareModuleCommand&) { return WorkerCommandKind::PrepareModule; },
            [](const InvokeProgramCommand&) { return WorkerCommandKind::InvokeProgram; },
            [](const CancelInvocationCommand&) { return WorkerCommandKind::CancelInvocation; },
            [](const CaptureScreenshotCommand&) { return WorkerCommandKind::CaptureScreenshot; },
            [](const ControlExecutionCommand&) { return WorkerCommandKind::ControlExecution; },
            [](const ShutdownCommand&) { return WorkerCommandKind::Shutdown; }},
        command);
}

[[nodiscard]] WorkerRejectionCode MapBackendError(BackendErrorCode code) noexcept
{
    switch (code)
    {
    case BackendErrorCode::InvalidArgument:
        return WorkerRejectionCode::InvalidArgument;
    case BackendErrorCode::InvalidState:
        return WorkerRejectionCode::InvalidState;
    case BackendErrorCode::None:
        return WorkerRejectionCode::None;
    default:
        return WorkerRejectionCode::BackendFailure;
    }
}

[[nodiscard]] WorkerRejectionCode MapExecutionError(
    ExecutionErrorCode code) noexcept
{
    switch (code)
    {
    case ExecutionErrorCode::None:
        return WorkerRejectionCode::None;
    case ExecutionErrorCode::InvalidArgument:
        return WorkerRejectionCode::InvalidArgument;
    case ExecutionErrorCode::InvalidState:
    case ExecutionErrorCode::Busy:
        return WorkerRejectionCode::InvalidState;
    case ExecutionErrorCode::StateEpochMismatch:
        return WorkerRejectionCode::StateEpochMismatch;
    case ExecutionErrorCode::Unsupported:
    case ExecutionErrorCode::InputUnavailable:
        return WorkerRejectionCode::Unsupported;
    case ExecutionErrorCode::RuntimeStopping:
        return WorkerRejectionCode::RuntimeStopping;
    case ExecutionErrorCode::WrongThread:
        return WorkerRejectionCode::InternalFailure;
    case ExecutionErrorCode::StopPointFailure:
    case ExecutionErrorCode::BackendFailure:
        return WorkerRejectionCode::BackendFailure;
    case ExecutionErrorCode::InterruptionUnavailable:
    case ExecutionErrorCode::InterruptionPolicyViolation:
    case ExecutionErrorCode::InterruptionDepthExceeded:
        return WorkerRejectionCode::InvalidState;
    }
    return WorkerRejectionCode::InternalFailure;
}

[[nodiscard]] bool IsSuccessfulExecutionTerminal(
    WorkerExecutionControlKind control,
    ExecutionTerminalStatus status) noexcept
{
    switch (control)
    {
    case WorkerExecutionControlKind::Pause:
        return status == ExecutionTerminalStatus::Paused;
    case WorkerExecutionControlKind::StepInstruction:
    case WorkerExecutionControlKind::StepFrame:
        return status == ExecutionTerminalStatus::StepsCompleted;
    case WorkerExecutionControlKind::Resume:
        return false;
    }
    return false;
}

std::atomic<std::uint64_t> g_next_production_session_id{1};

} // namespace

struct WorkerRuntime::Impl
{
    struct QueuedCommand
    {
        WireRequestId request_id;
        WorkerCommandSequence sequence;
        WorkerCommand command;
        std::promise<WorkerCommandResult> completion;
        bool completed = false;
    };

    enum class MailboxItemKind : std::uint8_t
    {
        Command,
        ProgramEvent,
        ProgramActionRequest,
        ProgramActionCompletion,
        ProgramPump,
        HostEvent,
        ForceStop,
    };

    struct PendingHostEvent
    {
        HostEventSequence sequence;
        SessionId observed_session_id;
        StateEpoch observed_state_epoch;
        std::string name;
        std::vector<std::uint8_t> encoded_payload;
    };

    struct MailboxItem
    {
        MailboxItemKind kind = MailboxItemKind::ForceStop;
        std::shared_ptr<QueuedCommand> command;
        std::optional<ProgramRuntimeEvent> program_event;
        std::optional<program::ProgramActionRequest>
            program_action_request;
        std::optional<program::ProgramActionCompletion>
            program_action_completion;
        std::optional<PendingHostEvent> host_event;
    };

    struct Mailbox
    {
        std::mutex mutex;
        std::condition_variable available;
        std::deque<MailboxItem> items;
        std::atomic<std::uint64_t> wake_generation{0};
        std::atomic<std::uint64_t> ingress_generation{0};
        std::uint64_t next_command_sequence = 1;
        std::uint64_t next_host_event_sequence = 1;
        bool accept_commands = true;
        bool accept_program_events = true;
        bool accept_program_actions = true;
        bool accept_host_events = true;
        bool program_pump_queued = false;
    };

    static void SignalMailbox(Mailbox& mailbox) noexcept
    {
        mailbox.wake_generation.fetch_add(1, std::memory_order_release);
        mailbox.available.notify_one();
    }

    static void NotifyStopPointIngress(void* context) noexcept
    {
        if (context)
            static_cast<Mailbox*>(context)->available.notify_one();
    }

    class ProgramEventIngress final : public IProgramRuntimeEventSink
    {
    public:
        explicit ProgramEventIngress(std::weak_ptr<Mailbox> mailbox)
            : mailbox_(std::move(mailbox))
        {
        }

        void Publish(ProgramRuntimeEvent event) override
        {
            const std::shared_ptr<Mailbox> mailbox = mailbox_.lock();
            if (!mailbox)
                return;

            {
                std::lock_guard lock(mailbox->mutex);
                if (!mailbox->accept_program_events)
                    return;
                MailboxItem item;
                item.kind = MailboxItemKind::ProgramEvent;
                item.program_event.emplace(std::move(event));
                mailbox->items.push_back(std::move(item));
            }
            SignalMailbox(*mailbox);
        }

    private:
        std::weak_ptr<Mailbox> mailbox_;
    };

    class ProgramActionIngress final
        : public program::IProgramActionRequestSink
    {
    public:
        explicit ProgramActionIngress(std::weak_ptr<Mailbox> mailbox)
            : mailbox_(std::move(mailbox))
        {
        }

        void Publish(program::ProgramActionRequest request) override
        {
            const std::shared_ptr<Mailbox> mailbox = mailbox_.lock();
            if (!mailbox)
                return;

            {
                std::lock_guard lock(mailbox->mutex);
                if (!mailbox->accept_program_actions)
                    return;
                MailboxItem item;
                item.kind = MailboxItemKind::ProgramActionRequest;
                item.program_action_request.emplace(std::move(request));
                mailbox->items.push_back(std::move(item));
            }
            SignalMailbox(*mailbox);
        }

    private:
        std::weak_ptr<Mailbox> mailbox_;
    };

    struct ActiveInvocation
    {
        InvocationId invocation_id;
        AttemptId attempt_id;
        StateEpoch origin_epoch;
        CancellationSource cancellation;

        ActiveInvocation(
            InvocationId invocation,
            AttemptId attempt,
            StateEpoch epoch)
            : invocation_id(invocation),
              attempt_id(attempt),
              origin_epoch(epoch),
              cancellation(invocation)
        {
        }
    };

    struct PendingExecutionCommand
    {
        std::shared_ptr<QueuedCommand> command;
        WorkerExecutionControlKind control =
            WorkerExecutionControlKind::Pause;
    };

    Impl(
        std::unique_ptr<EmulationSession> session,
        std::unique_ptr<IProgramRuntimePort> program_runtime,
        WorkerEventSink event_sink,
        std::shared_ptr<const WorkerRuntimeTestHooks> test_hooks,
        std::unique_ptr<program::IProgramActionHost> action_host)
        : mailbox(std::make_shared<Mailbox>()),
          program_event_ingress(std::make_shared<ProgramEventIngress>(mailbox)),
          program_action_ingress(
              std::make_shared<ProgramActionIngress>(mailbox)),
          session(std::move(session)),
          program_runtime(std::move(program_runtime)),
          program_action_host(std::move(action_host)),
          event_sink(std::move(event_sink)),
          test_hooks(std::move(test_hooks))
    {
        capabilities_value = kSlice1ProductionCapabilities;
        if (this->session)
        {
            const BackendExecutionCapabilityMask execution_capabilities =
                this->session->execution_capabilities();
            if (HasExecutionCapability(
                    execution_capabilities,
                    BackendExecutionCapability::Pause) &&
                HasExecutionCapability(
                    execution_capabilities,
                    BackendExecutionCapability::Resume) &&
                HasExecutionCapability(
                    execution_capabilities,
                    BackendExecutionCapability::FrameStep))
            {
                capabilities_value = AddCapability(
                    capabilities_value,
                    WorkerCapability::InteractiveVisualDebug);
            }
        }
        if (this->program_runtime && this->program_action_host &&
            HasCapability(
                this->program_runtime->capabilities(),
                WorkerCapability::ProgramInvocation))
        {
            capabilities_value = AddCapability(
                capabilities_value,
                WorkerCapability::ProgramInvocation);
        }
        if (this->program_runtime)
        {
            this->program_runtime->BindActionSink(
                program_action_ingress);
        }

        current_snapshot.state = WorkerState::Starting;
        current_snapshot.capabilities = capabilities_value;
        if (this->session)
        {
            (void)this->session->ConfigureStopPointIngressNotification(
                &mailbox->ingress_generation,
                mailbox.get(),
                &NotifyStopPointIngress);
            current_snapshot.session = this->session->snapshot();
            current_snapshot.execution =
                this->session->execution_snapshot();
        }

        actor = std::thread([this] { ActorMain(); });
    }

    ~Impl()
    {
        if (Snapshot().state != WorkerState::Stopped)
        {
            {
                std::lock_guard lock(mailbox->mutex);
                mailbox->accept_commands = false;
                mailbox->accept_program_events = false;
                mailbox->accept_program_actions = false;
                mailbox->accept_host_events = false;
                MailboxItem item;
                item.kind = MailboxItemKind::ForceStop;
                mailbox->items.push_front(std::move(item));
            }
            SignalMailbox(*mailbox);
        }
        WaitStopped();
    }

    std::future<WorkerCommandResult> Submit(
        WireRequestId request_id,
        WorkerCommand command)
    {
        auto queued = std::make_shared<QueuedCommand>();
        queued->request_id = request_id;
        queued->command = std::move(command);
        std::future<WorkerCommandResult> future = queued->completion.get_future();

        bool accepted = false;
        {
            std::lock_guard lock(mailbox->mutex);
            const std::uint64_t raw_sequence = mailbox->next_command_sequence++;
            queued->sequence = WorkerCommandSequence(raw_sequence);

            if (mailbox->accept_commands && raw_sequence != 0)
            {
                mailbox->items.push_back(
                    MailboxItem{MailboxItemKind::Command, queued, {}});
                accepted = true;
            }
        }

        if (accepted)
        {
            SignalMailbox(*mailbox);
            return future;
        }

        WorkerCommandResult result;
        result.request_id = request_id;
        result.command_sequence = queued->sequence;
        result.command_kind = CommandKind(queued->command);
        result.outcome = WorkerCommandOutcome::Rejected;
        result.snapshot = Snapshot();
        result.error = {
            WorkerRejectionCode::RuntimeStopping,
            "WorkerRuntime is no longer accepting commands"};
        queued->completed = true;
        queued->completion.set_value(std::move(result));
        return future;
    }

    WorkerSnapshot Snapshot() const
    {
        std::lock_guard lock(snapshot_mutex);
        return current_snapshot;
    }

    bool EnqueueHostEvent(
        std::string name,
        std::vector<std::uint8_t> encoded_payload)
    {
        const WorkerSnapshot observed = Snapshot();
        return EnqueueHostEvent(
            observed.session.session_id,
            observed.session.state_epoch,
            std::move(name),
            std::move(encoded_payload));
    }

    bool EnqueueHostEvent(
        SessionId observed_session_id,
        StateEpoch observed_state_epoch,
        std::string name,
        std::vector<std::uint8_t> encoded_payload)
    {
        if (name.empty())
            return false;

        {
            std::lock_guard lock(mailbox->mutex);
            if (!mailbox->accept_host_events)
                return false;

            const std::uint64_t raw_sequence = mailbox->next_host_event_sequence++;
            if (raw_sequence == 0)
                return false;

            MailboxItem item;
            item.kind = MailboxItemKind::HostEvent;
            item.host_event.emplace(PendingHostEvent{
                HostEventSequence(raw_sequence),
                observed_session_id,
                observed_state_epoch,
                std::move(name),
                std::move(encoded_payload)});
            mailbox->items.push_back(std::move(item));
        }
        SignalMailbox(*mailbox);
        return true;
    }

    void WaitStopped()
    {
        std::lock_guard lock(join_mutex);
        if (actor.joinable() && actor.get_id() != std::this_thread::get_id())
            actor.join();
    }

    void ActorMain()
    {
        if (!session)
        {
            ChangeState(WorkerState::Tainted);
            Publish(WorkerRuntimeDiagnosticEvent{
                WorkerRejectionCode::InternalFailure,
                "WorkerRuntime was constructed without an EmulationSession",
                {}});
        }
        else
        {
            ChangeState(WorkerState::AwaitingSession);
        }

        for (;;)
        {
            bool item_waiting_before_ingress = false;
            {
                std::lock_guard lock(mailbox->mutex);
                item_waiting_before_ingress =
                    !mailbox->items.empty();
            }

            const std::uint64_t stable_ingress_generation =
                DrainAuthoritativeIngressToStable(
                    item_waiting_before_ingress);
            PumpProgramActionHost();

            MailboxItem item;
            bool has_item = false;
            {
                std::lock_guard lock(mailbox->mutex);
                if (!mailbox->items.empty())
                {
                    item = std::move(mailbox->items.front());
                    mailbox->items.pop_front();
                    if (item.kind == MailboxItemKind::ProgramPump)
                        mailbox->program_pump_queued = false;
                    has_item = true;
                }
            }
            if (!has_item)
            {
                PumpExecutionEvents();
                PumpProgramActionHost();
                PumpProgramRuntime();
                const std::uint64_t observed_generation =
                    mailbox->wake_generation.load(
                        std::memory_order_acquire);
                const auto next_execution_wake = session
                    ? session->next_execution_wake()
                    : std::nullopt;
                const auto next_program_wake = program_runtime
                    ? program_runtime->next_wake()
                    : std::nullopt;
                std::optional<std::chrono::steady_clock::time_point>
                    next_wake = next_execution_wake;
                if (next_program_wake &&
                    (!next_wake ||
                     *next_program_wake < *next_wake))
                {
                    next_wake = next_program_wake;
                }
                std::unique_lock lock(mailbox->mutex);
                const auto ready = [&]() {
                    return !mailbox->items.empty() ||
                        mailbox->wake_generation.load(
                            std::memory_order_acquire) !=
                            observed_generation ||
                        mailbox->ingress_generation.load(
                            std::memory_order_acquire) !=
                            stable_ingress_generation;
                };
                if (next_wake)
                {
                    (void)mailbox->available.wait_until(
                        lock,
                        *next_wake,
                        ready);
                }
                else
                {
                    mailbox->available.wait(lock, ready);
                }
                continue;
            }

            if (item.kind == MailboxItemKind::ForceStop)
            {
                ForceStop();
                break;
            }

            if (item.kind == MailboxItemKind::ProgramEvent)
            {
                if (item.program_event)
                {
                    try
                    {
                        HandleProgramEvent(std::move(*item.program_event));
                    }
                    catch (const std::exception& ex)
                    {
                        EnterTainted(ex.what());
                        Publish(WorkerRuntimeDiagnosticEvent{
                            WorkerRejectionCode::InternalFailure,
                            std::string("ProgramRuntime event handling threw: ") + ex.what(),
                            {}});
                    }
                    catch (...)
                    {
                        EnterTainted(
                            "Unknown ProgramRuntime event handling failure");
                        Publish(WorkerRuntimeDiagnosticEvent{
                            WorkerRejectionCode::InternalFailure,
                            "ProgramRuntime event handling threw",
                            {}});
                    }
                }
            }
            else if (item.kind ==
                     MailboxItemKind::ProgramActionRequest)
            {
                if (item.program_action_request)
                {
                    HandleProgramActionRequest(
                        std::move(*item.program_action_request));
                }
            }
            else if (item.kind ==
                     MailboxItemKind::ProgramActionCompletion)
            {
                if (item.program_action_completion)
                {
                    HandleProgramActionCompletion(
                        std::move(*item.program_action_completion));
                }
            }
            else if (item.kind == MailboxItemKind::ProgramPump)
            {
                PumpProgramRuntime();
            }
            else if (item.kind == MailboxItemKind::HostEvent)
            {
                if (item.host_event)
                    HandleHostEvent(std::move(*item.host_event));
            }
            else if (item.command)
            {
                try
                {
                    HandleCommand(item.command);
                }
                catch (const std::exception& ex)
                {
                    EnterTainted(ex.what());
                    Reject(
                        item.command,
                        WorkerRejectionCode::InternalFailure,
                        std::string("Worker command handling threw: ") + ex.what());
                }
                catch (...)
                {
                    EnterTainted("Unknown worker command handling failure");
                    Reject(
                        item.command,
                        WorkerRejectionCode::InternalFailure,
                        "Worker command handling threw");
                }
            }

            (void)DrainAuthoritativeIngressToStable(false);
            PumpExecutionEvents();
            PumpProgramActionHost();

            if (Snapshot().state == WorkerState::Stopped)
                break;
        }
    }

    void HandleCommand(std::shared_ptr<QueuedCommand> queued)
    {
        {
            std::lock_guard lock(snapshot_mutex);
            current_snapshot.last_command_sequence = queued->sequence;
        }

        std::visit(
            Overloaded{
                [this, &queued](const OpenSessionCommand& command) {
                    HandleOpen(queued, command);
                },
                [this, &queued](const PrepareModuleCommand& command) {
                    HandlePrepareModule(queued, command);
                },
                [this, &queued](const InvokeProgramCommand& command) {
                    HandleInvoke(queued, command);
                },
                [this, &queued](const CancelInvocationCommand& command) {
                    HandleCancel(queued, command);
                },
                [this, &queued](const CaptureScreenshotCommand& command) {
                    HandleScreenshot(queued, command);
                },
                [this, &queued](const ControlExecutionCommand& command) {
                    HandleExecutionControl(queued, command);
                },
                [this, &queued](const ShutdownCommand&) {
                    HandleShutdown(queued);
                }},
            queued->command);
    }

    void HandleOpen(
        const std::shared_ptr<QueuedCommand>& queued,
        const OpenSessionCommand& command)
    {
        if (Snapshot().state != WorkerState::AwaitingSession)
        {
            Reject(
                queued,
                WorkerRejectionCode::InvalidState,
                "OpenSession is accepted only while awaiting a session");
            return;
        }

        SessionOperationReceipt receipt = session->Open(command.options);
        if (!receipt.ok)
        {
            RefreshSnapshot();
            if (receipt.disposition == SessionDisposition::Tainted)
                EnterTainted(receipt.backend.message);
            Reject(
                queued,
                MapBackendError(receipt.backend.code),
                receipt.backend.message,
                receipt);
            return;
        }
        if (test_hooks && test_hooks->session_opened)
            test_hooks->session_opened(*session);
        RefreshSnapshot();

        ChangeState(WorkerState::Ready);
        session_visual_intent = command.options.backend.visual;
        terminal_screenshot_directory = command.options.screenshot_directory;
        terminal_screenshot_timeout = command.options.screenshot_timeout;
        screenshot_on_terminal = command.options.screenshot_on_terminal;
        Complete(
            queued,
            WorkerCommandOutcome::Completed,
            {},
            {},
            std::move(receipt));
    }

    void HandlePrepareModule(
        const std::shared_ptr<QueuedCommand>& queued,
        const PrepareModuleCommand& command)
    {
        if (!RequireReadyProgramRuntime(queued))
            return;
        if (command.module.identity.canonical_id.empty() ||
            command.module.payload.empty())
        {
            Reject(
                queued,
                WorkerRejectionCode::InvalidArgument,
                "PrepareModule requires a canonical identity and encoded payload");
            return;
        }

        ProgramRuntimeSubmission submission;
        try
        {
            submission = program_runtime->PrepareModule(
                ModulePreparationRequest{queued->sequence, command.module},
                program_event_ingress);
        }
        catch (const std::exception& ex)
        {
            EnterTainted(
                std::string("ProgramRuntime module submission threw: ") + ex.what());
            Reject(
                queued,
                WorkerRejectionCode::InternalFailure,
                std::string("ProgramRuntime module submission threw: ") + ex.what());
            return;
        }
        catch (...)
        {
            EnterTainted("ProgramRuntime module submission threw");
            Reject(
                queued,
                WorkerRejectionCode::InternalFailure,
                "ProgramRuntime module submission threw");
            return;
        }

        if (!submission.accepted)
        {
            Reject(
                queued,
                submission.error.code == WorkerRejectionCode::None
                    ? WorkerRejectionCode::InternalFailure
                    : submission.error.code,
                submission.error.message);
            return;
        }

        Complete(queued, WorkerCommandOutcome::Accepted);
    }

    void HandleInvoke(
        const std::shared_ptr<QueuedCommand>& queued,
        const InvokeProgramCommand& command)
    {
        if (!RequireReadyProgramRuntime(queued))
            return;

        const EncodedInvocationEnvelope& invocation = command.invocation;
        if (!invocation.invocation_id ||
            !invocation.attempt_id ||
            invocation.module.canonical_id.empty() ||
            invocation.entrypoint.empty())
        {
            Reject(
                queued,
                WorkerRejectionCode::InvalidArgument,
                "InvokeProgram requires invocation, attempt, module, and entrypoint identities");
            return;
        }

        const SessionSnapshot session_snapshot = session->snapshot();
        if (!invocation.expected_state_epoch ||
            invocation.expected_state_epoch != session_snapshot.state_epoch)
        {
            Reject(
                queued,
                WorkerRejectionCode::StateEpochMismatch,
                "Invocation StateEpoch does not match the owned session");
            return;
        }

        active_invocation.emplace(
            invocation.invocation_id,
            invocation.attempt_id,
            session_snapshot.state_epoch);

        ProgramRuntimeSubmission submission;
        bool submission_threw = false;
        try
        {
            submission = program_runtime->StartInvocation(
                ProgramInvocationRequest{queued->sequence, invocation},
                active_invocation->cancellation.token(),
                program_event_ingress);
        }
        catch (const std::exception& ex)
        {
            submission_threw = true;
            submission = ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InternalFailure,
                std::string("ProgramRuntime invocation submission threw: ") + ex.what());
        }
        catch (...)
        {
            submission_threw = true;
            submission = ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InternalFailure,
                "ProgramRuntime invocation submission threw");
        }

        if (submission_threw)
        {
            const RuntimeError error = submission.error;
            EnterTainted(
                error.message.empty()
                    ? "ProgramRuntime invocation submission threw"
                    : error.message,
                false);
            Reject(
                queued,
                error.code == WorkerRejectionCode::None
                    ? WorkerRejectionCode::InternalFailure
                    : error.code,
                error.message);
            return;
        }

        if (!submission.accepted)
        {
            active_invocation.reset();
            RefreshSnapshot();
            Reject(
                queued,
                submission.error.code == WorkerRejectionCode::None
                    ? WorkerRejectionCode::InternalFailure
                    : submission.error.code,
                submission.error.message);
            return;
        }

        RefreshSnapshot();
        ChangeState(WorkerState::Running);
        Complete(
            queued,
            WorkerCommandOutcome::Accepted,
            invocation.invocation_id);
        QueueProgramPump();
    }

    void HandleCancel(
        const std::shared_ptr<QueuedCommand>& queued,
        const CancelInvocationCommand& command)
    {
        if (!active_invocation)
        {
            Reject(
                queued,
                WorkerRejectionCode::InvocationNotActive,
                "No invocation is active");
            return;
        }
        if (command.invocation_id != active_invocation->invocation_id)
        {
            Reject(
                queued,
                WorkerRejectionCode::InvocationMismatch,
                "Cancellation does not identify the active invocation");
            return;
        }
        if (active_invocation->cancellation.is_cancellation_requested())
        {
            Reject(
                queued,
                WorkerRejectionCode::DuplicateCancellation,
                "Cancellation has already been requested for this invocation");
            return;
        }

        ProgramRuntimeSubmission submission =
            RequestProgramCancellation(command.invocation_id);
        if (!submission.accepted)
        {
            const WorkerRejectionCode code =
                submission.error.code == WorkerRejectionCode::None
                ? WorkerRejectionCode::InternalFailure
                : submission.error.code;
            const std::string message =
                submission.error.message.empty()
                ? "ProgramRuntime rejected the cancellation notification"
                : submission.error.message;
            EnterTainted(
                "Cancellation could not be delivered to ProgramRuntime: " +
                    message,
                true,
                false);
            Reject(queued, code, message);
            return;
        }
        if (submission.terminal_already_published)
        {
            // ProgramRuntime has already selected and published the exact
            // invocation terminal, but that mailbox event has not reached
            // this actor yet. Cancellation loses this ordering race without
            // mutating the token, notifying the host, or tainting the
            // session.
            Reject(
                queued,
                WorkerRejectionCode::InvocationNotActive,
                "Invocation is already terminal");
            return;
        }

        if (!active_invocation->cancellation.request_cancellation(
                CancellationReason::ExternalRequest))
        {
            EnterTainted(
                "Cancellation was accepted by ProgramRuntime but could "
                "not be recorded by WorkerRuntime");
            Reject(
                queued,
                WorkerRejectionCode::InternalFailure,
                "Cancellation state could not be recorded");
            return;
        }
        ChangeState(WorkerState::Cancelling);
        if (program_action_host)
        {
            program_action_host->RequestCancellation(
                command.invocation_id,
                CancellationReason::ExternalRequest);
        }

        Complete(
            queued,
            WorkerCommandOutcome::Accepted,
            command.invocation_id);
    }

    void HandleScreenshot(
        const std::shared_ptr<QueuedCommand>& queued,
        const CaptureScreenshotCommand& command)
    {
        const WorkerState state = Snapshot().state;
        if (state != WorkerState::Ready &&
            state != WorkerState::Running &&
            state != WorkerState::Cancelling)
        {
            Reject(
                queued,
                state == WorkerState::Tainted
                    ? WorkerRejectionCode::SessionTainted
                    : WorkerRejectionCode::InvalidState,
                "Screenshot is unavailable in the current worker state");
            return;
        }

        const SessionSnapshot session_snapshot = session->snapshot();
        if (!command.session_id ||
            command.session_id != session_snapshot.session_id)
        {
            Reject(
                queued,
                WorkerRejectionCode::SessionMismatch,
                "Screenshot does not identify the owned session");
            return;
        }

        SessionOperationReceipt receipt =
            session->CaptureScreenshot(command.output_path, command.timeout);
        RefreshSnapshot();
        if (!receipt.ok)
        {
            if (receipt.disposition == SessionDisposition::Tainted)
                EnterTainted(receipt.backend.message);
            Reject(
                queued,
                MapBackendError(receipt.backend.code),
                receipt.backend.message,
                receipt);
            return;
        }

        Complete(
            queued,
            WorkerCommandOutcome::Completed,
            {},
            {},
            std::move(receipt));
    }

    void HandleExecutionControl(
        const std::shared_ptr<QueuedCommand>& queued,
        const ControlExecutionCommand& command)
    {
        const WorkerSnapshot worker = Snapshot();
        if (worker.state != WorkerState::Ready)
        {
            Reject(
                queued,
                worker.state == WorkerState::Tainted
                    ? WorkerRejectionCode::SessionTainted
                    : WorkerRejectionCode::InvalidState,
                "Execution control requires a ready worker");
            return;
        }
        if (active_invocation)
        {
            Reject(
                queued,
                WorkerRejectionCode::InvocationAlreadyActive,
                "Ready-session execution control is unavailable during an invocation");
            return;
        }
        if (!session_visual_intent)
        {
            Reject(
                queued,
                WorkerRejectionCode::Unsupported,
                "Execution control requires a session opened with visual intent");
            return;
        }
        if (!HasCapability(
                capabilities_value,
                WorkerCapability::InteractiveVisualDebug))
        {
            Reject(
                queued,
                WorkerRejectionCode::Unsupported,
                "Interactive visual-debug execution control is unavailable");
            return;
        }

        const SessionSnapshot session_snapshot = session->snapshot();
        if (!command.session_id ||
            command.session_id != session_snapshot.session_id)
        {
            Reject(
                queued,
                WorkerRejectionCode::SessionMismatch,
                "Execution control does not identify the owned session");
            return;
        }
        if (!command.expected_state_epoch ||
            command.expected_state_epoch != session_snapshot.state_epoch)
        {
            Reject(
                queued,
                WorkerRejectionCode::StateEpochMismatch,
                "Execution control StateEpoch does not match the owned session");
            return;
        }

        const bool is_step =
            command.control == WorkerExecutionControlKind::StepFrame ||
            command.control == WorkerExecutionControlKind::StepInstruction;
        if ((is_step && command.count == 0) ||
            (!is_step && command.count != 0))
        {
            Reject(
                queued,
                WorkerRejectionCode::InvalidArgument,
                "Only step execution controls accept a nonzero count");
            return;
        }
        if (command.control != WorkerExecutionControlKind::Resume &&
            command.timeout <= std::chrono::milliseconds::zero())
        {
            Reject(
                queued,
                WorkerRejectionCode::InvalidArgument,
                "Pause and step execution controls require a bounded timeout");
            return;
        }

        ExecutionRequest request;
        if (command.control == WorkerExecutionControlKind::Resume)
        {
            InteractiveResumeRequest resume;
            resume.expected_epoch = command.expected_state_epoch;
            request = std::move(resume);
        }
        else
        {
            ExecutionRequestPolicy policy;
            policy.expected_epoch = command.expected_state_epoch;
            policy.active_timeout = command.timeout;
            policy.interruptions = ExecutionInterruptionPolicy::Reject;
            switch (command.control)
            {
            case WorkerExecutionControlKind::Pause:
                request = SafePauseRequest{std::move(policy)};
                break;
            case WorkerExecutionControlKind::StepInstruction:
                request = StepInstructionsRequest{
                    std::move(policy),
                    command.count};
                break;
            case WorkerExecutionControlKind::StepFrame:
                request = StepFramesRequest{
                    std::move(policy),
                    command.count};
                break;
            case WorkerExecutionControlKind::Resume:
                break;
            }
        }

        ExecutionSubmissionReceipt submission =
            session->SubmitExecution(std::move(request));
        RefreshSnapshot();
        if (!submission.accepted)
        {
            if (submission.error.integrity != BackendIntegrity::Preserved)
                EnterTainted(submission.error.message);
            Reject(
                queued,
                MapExecutionError(submission.error.code),
                submission.error.message.empty()
                    ? "ExecutionEngine rejected execution control"
                    : submission.error.message);
            return;
        }

        if (command.control == WorkerExecutionControlKind::Resume)
        {
            Complete(
                queued,
                WorkerCommandOutcome::Accepted,
                {},
                {},
                {},
                submission.operation_id);
            PumpExecutionEvents();
            return;
        }

        pending_execution_commands.emplace(
            submission.operation_id.value(),
            PendingExecutionCommand{queued, command.control});
        PumpExecutionEvents();
    }

    void HandleShutdown(const std::shared_ptr<QueuedCommand>& queued)
    {
        const WorkerState state = Snapshot().state;
        if (state == WorkerState::Stopped)
        {
            Complete(queued, WorkerCommandOutcome::Completed);
            return;
        }

        {
            std::lock_guard lock(mailbox->mutex);
            mailbox->accept_commands = false;
        }

        pending_shutdown_commands.push_back(queued);
        ChangeState(WorkerState::Stopping);

        if (session &&
            session->execution_snapshot().activity !=
                ExecutionActivity::IdlePaused &&
            session->execution_snapshot().activity !=
                ExecutionActivity::Closed)
        {
            (void)session->CancelExecution(CancellationReason::Shutdown);
            PumpExecutionEvents();
            if (session &&
                session->execution_snapshot().activity !=
                    ExecutionActivity::IdlePaused &&
                session->execution_snapshot().activity !=
                    ExecutionActivity::Closed)
            {
                return;
            }
        }

        if (active_invocation)
        {
            if (program_runtime)
            {
                const ProgramRuntimeSubmission submission =
                    RequestProgramCancellation(active_invocation->invocation_id);
                if (!submission.accepted)
                {
                    const std::string message =
                        submission.error.message.empty()
                        ? "ProgramRuntime rejected shutdown cancellation"
                        : submission.error.message;
                    EnterTainted(
                        "Shutdown cancellation could not be delivered to "
                        "ProgramRuntime: " + message,
                        true,
                        false);
                    FinishShutdown(false);
                    return;
                }
                if (submission.terminal_already_published)
                    return;
            }
            if (!active_invocation->cancellation.request_cancellation(
                    CancellationReason::Shutdown))
            {
                EnterTainted(
                    "Shutdown cancellation could not be recorded by "
                    "WorkerRuntime");
                FinishShutdown(false);
                return;
            }
            if (program_action_host)
            {
                program_action_host->RequestCancellation(
                    active_invocation->invocation_id,
                    CancellationReason::Shutdown);
            }
            return;
        }

        FinishShutdown(false);
    }

    bool RequireReadyProgramRuntime(
        const std::shared_ptr<QueuedCommand>& queued)
    {
        const WorkerState state = Snapshot().state;
        if (active_invocation)
        {
            Reject(
                queued,
                WorkerRejectionCode::InvocationAlreadyActive,
                "Only one invocation may be active");
            return false;
        }
        if (state != WorkerState::Ready)
        {
            Reject(
                queued,
                state == WorkerState::Tainted
                    ? WorkerRejectionCode::SessionTainted
                    : WorkerRejectionCode::InvalidState,
                "Program command requires a ready worker");
            return false;
        }
        if (Snapshot().execution.activity != ExecutionActivity::IdlePaused)
        {
            Reject(
                queued,
                WorkerRejectionCode::InvalidState,
                "Program command requires an idle paused execution session");
            return false;
        }
        if (!program_runtime ||
            !HasCapability(capabilities_value, WorkerCapability::ProgramInvocation))
        {
            Reject(
                queued,
                WorkerRejectionCode::ProgramRuntimeUnavailable,
                "Canonical ProgramRuntime invocation is not available");
            return false;
        }
        return true;
    }

    ProgramRuntimeSubmission RequestProgramCancellation(
        InvocationId invocation_id) noexcept
    {
        if (!program_runtime)
        {
            return ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::ProgramRuntimeUnavailable,
                "Canonical ProgramRuntime invocation is not available");
        }

        try
        {
            return program_runtime->RequestCancellation(invocation_id);
        }
        catch (const std::exception& ex)
        {
            return ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InternalFailure,
                std::string("ProgramRuntime cancellation threw: ") + ex.what());
        }
        catch (...)
        {
            return ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InternalFailure,
                "ProgramRuntime cancellation threw");
        }
    }

    void QueueProgramPump()
    {
        if (!program_runtime)
            return;
        bool queued = false;
        {
            std::lock_guard lock(mailbox->mutex);
            if (mailbox->accept_program_events &&
                !mailbox->program_pump_queued)
            {
                MailboxItem item;
                item.kind = MailboxItemKind::ProgramPump;
                mailbox->items.push_back(std::move(item));
                mailbox->program_pump_queued = true;
                queued = true;
            }
        }
        if (queued)
            SignalMailbox(*mailbox);
    }

    void QueueProgramActionCompletion(
        program::ProgramActionCompletion completion)
    {
        bool queued = false;
        {
            std::lock_guard lock(mailbox->mutex);
            if (mailbox->accept_program_actions)
            {
                MailboxItem item;
                item.kind =
                    MailboxItemKind::ProgramActionCompletion;
                item.program_action_completion.emplace(
                    std::move(completion));
                mailbox->items.push_back(std::move(item));
                queued = true;
            }
        }
        if (queued)
            SignalMailbox(*mailbox);
    }

    [[nodiscard]] program::ProgramActionCompletion
    RejectProgramAction(
        const program::ProgramActionRequest& request,
        program::ProgramActionCompletionStatus status,
        std::string code,
        std::string message) const
    {
        const StateEpoch current_epoch =
            session ? session->snapshot().state_epoch : StateEpoch{};
        return {
            .request_id = request.request_id,
            .invocation_id = request.invocation_id,
            .attempt_id = request.attempt_id,
            .operation = request.operation,
            .status = status,
            .origin_epoch = request.expected_epoch,
            .resulting_epoch = current_epoch,
            .cleanup =
                status ==
                    program::ProgramActionCompletionStatus::CleanupFailed
                ? program::ProgramCleanupStatus::Tainted
                : program::ProgramCleanupStatus::Clean,
            .session_disposition = session
                ? session->snapshot().disposition
                : SessionDisposition::Closed,
            .code = std::move(code),
            .message = std::move(message),
        };
    }

    void HandleProgramActionRequest(
        program::ProgramActionRequest request)
    {
        if (!active_invocation ||
            request.invocation_id !=
                active_invocation->invocation_id ||
            request.attempt_id != active_invocation->attempt_id)
        {
            QueueProgramActionCompletion(RejectProgramAction(
                request,
                program::ProgramActionCompletionStatus::Rejected,
                "invocation_mismatch",
                "Program action does not identify the active invocation"));
            return;
        }
        if (!request.request_id)
        {
            QueueProgramActionCompletion(RejectProgramAction(
                request,
                program::ProgramActionCompletionStatus::Rejected,
                "invalid_request",
                "Program action request identity is zero"));
            return;
        }
        const SessionSnapshot current = session->snapshot();
        if (!request.expected_epoch ||
            request.expected_epoch != current.state_epoch)
        {
            QueueProgramActionCompletion(RejectProgramAction(
                request,
                program::ProgramActionCompletionStatus::StaleEpoch,
                "stale_epoch",
                "Program action expected a stale StateEpoch"));
            return;
        }
        if (!program_action_host)
        {
            QueueProgramActionCompletion(RejectProgramAction(
                request,
                program::ProgramActionCompletionStatus::Unsupported,
                "action_host_unavailable",
                "Worker has no actor-owned program action host"));
            return;
        }

        program::ProgramActionDispatchResult dispatched;
        try
        {
            dispatched = program_action_host->Dispatch(request);
        }
        catch (const std::exception& ex)
        {
            EnterTainted(
                std::string("Program action host dispatch threw: ") +
                ex.what());
            return;
        }
        catch (...)
        {
            EnterTainted("Program action host dispatch threw");
            return;
        }
        if (!dispatched.accepted)
        {
            QueueProgramActionCompletion(
                dispatched.immediate_completion
                    ? std::move(*dispatched.immediate_completion)
                    : RejectProgramAction(
                        request,
                        program::ProgramActionCompletionStatus::Rejected,
                        "action_rejected",
                        dispatched.diagnostic.empty()
                            ? "Program action host rejected the request"
                            : std::move(dispatched.diagnostic)));
            return;
        }
        // Even an immediate service result crosses the mailbox before it can
        // resume ProgramRuntime.
        if (dispatched.immediate_completion)
        {
            QueueProgramActionCompletion(
                std::move(*dispatched.immediate_completion));
        }
    }

    void HandleProgramActionCompletion(
        program::ProgramActionCompletion completion)
    {
        if (!program_runtime)
            return;
        if (!active_invocation)
        {
            Publish(WorkerRuntimeDiagnosticEvent{
                WorkerRejectionCode::InvocationNotActive,
                "Ignored a late program action completion after invocation termination",
                completion.invocation_id});
            return;
        }
        ProgramRuntimeSubmission delivered;
        try
        {
            delivered = program_runtime->DeliverActionCompletion(
                std::move(completion));
        }
        catch (const std::exception& ex)
        {
            EnterTainted(
                std::string(
                    "ProgramRuntime action completion threw: ") +
                ex.what());
            return;
        }
        catch (...)
        {
            EnterTainted("ProgramRuntime action completion threw");
            return;
        }
        if (!delivered.accepted)
        {
            EnterTainted(
                delivered.error.message.empty()
                    ? "ProgramRuntime rejected its currently awaited actor action completion"
                    : delivered.error.message);
            return;
        }
        QueueProgramPump();
    }

    void PumpProgramRuntime()
    {
        if (!program_runtime || !active_invocation)
            return;
        try
        {
            if (program_runtime->Pump())
                QueueProgramPump();
        }
        catch (const std::exception& ex)
        {
            EnterTainted(
                std::string("ProgramRuntime pump threw: ") + ex.what());
        }
        catch (...)
        {
            EnterTainted("ProgramRuntime pump threw");
        }
    }

    void PumpProgramActionHost()
    {
        if (!program_action_host)
            return;
        try
        {
            program_action_host->Pump();
            std::vector<program::ProgramActionCompletion> completions =
                program_action_host->DrainCompletions();
            if (completions.empty())
                return;

            bool queued = false;
            {
                std::lock_guard lock(mailbox->mutex);
                if (mailbox->accept_program_actions)
                {
                    // These completions are consequences of internal events
                    // already accepted before the current command
                    // linearization point. Place them ahead of external
                    // commands without handling them inline, preserving both
                    // actor ownership and authoritative-stop precedence.
                    std::vector<MailboxItem> prioritized;
                    prioritized.reserve(completions.size());
                    for (program::ProgramActionCompletion& completion :
                         completions)
                    {
                        MailboxItem item;
                        item.kind =
                            MailboxItemKind::ProgramActionCompletion;
                        item.program_action_completion.emplace(
                            std::move(completion));
                        prioritized.push_back(std::move(item));
                    }
                    const auto insertion = std::find_if(
                        mailbox->items.begin(),
                        mailbox->items.end(),
                        [](const MailboxItem& item) {
                            return item.kind ==
                                    MailboxItemKind::Command ||
                                item.kind ==
                                    MailboxItemKind::HostEvent;
                        });
                    mailbox->items.insert(
                        insertion,
                        std::make_move_iterator(
                            prioritized.begin()),
                        std::make_move_iterator(
                            prioritized.end()));
                    queued = true;
                }
            }
            if (queued)
                SignalMailbox(*mailbox);
        }
        catch (const std::exception& ex)
        {
            EnterTainted(
                std::string("Program action host pump threw: ") +
                ex.what());
        }
        catch (...)
        {
            EnterTainted("Program action host pump threw");
        }
    }

    void HandleProgramEvent(ProgramRuntimeEvent event)
    {
        std::visit(
            Overloaded{
                [this](ModulePreparationEvent& module) {
                    Publish(module);
                },
                [this](ProgramInvocationProgressEvent& progress) {
                    if (!active_invocation ||
                        progress.invocation_id !=
                            active_invocation->invocation_id ||
                        progress.attempt_id != active_invocation->attempt_id)
                    {
                        Publish(WorkerRuntimeDiagnosticEvent{
                            WorkerRejectionCode::InvocationMismatch,
                            "Ignored progress for a non-active invocation "
                            "attempt",
                            progress.invocation_id});
                        return;
                    }
                    Publish(progress);
                },
                [this](ProgramInvocationTerminalEvent& terminal) {
                    HandleInvocationTerminal(terminal);
                }},
            event);
    }

    void HandleHostEvent(PendingHostEvent event)
    {
        if (event.name == "Host_JitCacheInvalidation" && session &&
            session->snapshot().open)
        {
            const SessionOperationReceipt receipt =
                session->RevalidateStopPointsAfterJit();
            if (!receipt.ok)
            {
                EnterTainted(
                    receipt.backend.message.empty()
                        ? "Stop-point JIT revalidation failed"
                        : receipt.backend.message);
            }
        }
        else if (event.name == "Host_PPCBreakpointsChanged" && session &&
                 session->snapshot().open)
        {
            const SessionOperationReceipt receipt =
                session->ValidateBreakpointChangeNotification();
            if (!receipt.ok)
            {
                EnterTainted(
                    receipt.backend.message.empty()
                        ? "Unmanaged Dolphin breakpoint change detected"
                        : receipt.backend.message);
            }
        }
        Publish(HostRuntimeEvent{
            event.sequence,
            event.observed_session_id,
            event.observed_state_epoch,
            std::move(event.name),
            std::move(event.encoded_payload)});
    }

    void DrainStopPointIngress()
    {
        if (!session)
            return;
        for (StopRouteReceipt& receipt : session->DrainStopPointEvents())
        {
            const StopRouteTerminal terminal = receipt.terminal;
            const std::string diagnostic = receipt.error.message;
            session->HandleStopPointReceipt(std::move(receipt));
            if (terminal == StopRouteTerminal::Failed ||
                terminal == StopRouteTerminal::Overflow)
            {
                // Preserve the engine's typed terminal before shutdown tears
                // down its event queue. Pending control commands therefore
                // complete from the authoritative routed failure exactly once.
                DrainExecutionEvents();
                if (Snapshot().state != WorkerState::Tainted)
                {
                    EnterTainted(
                        diagnostic.empty()
                            ? "Authoritative stop-point routing failed"
                            : diagnostic);
                }
                break;
            }
        }
    }

    [[nodiscard]] std::uint64_t DrainAuthoritativeIngressToStable(
        bool expose_test_window)
    {
        for (;;)
        {
            const std::uint64_t observed_generation =
                mailbox->ingress_generation.load(std::memory_order_acquire);
            DrainStopPointIngress();
            DrainExecutionEvents();

            if (expose_test_window && test_hooks &&
                test_hooks->before_ingress_stability_check)
            {
                test_hooks->before_ingress_stability_check();
            }

            // This acquire is the command linearization point. A native stop
            // published before it either appeared in the completed drain or
            // changed the generation and forces another drain. A stop
            // published afterward belongs to the next actor turn.
            if (mailbox->ingress_generation.load(
                    std::memory_order_acquire) == observed_generation)
            {
                return observed_generation;
            }
        }
    }

    void PumpExecutionEvents()
    {
        if (!session)
            return;

        session->PumpExecution();
        DrainExecutionEvents();
    }

    void DrainExecutionEvents()
    {
        if (!session)
            return;

        std::vector<ExecutionEvent> events =
            session->DrainExecutionEvents();
        for (ExecutionEvent& event : events)
        {
            RefreshSnapshot();
            if (program_action_host)
            {
                try
                {
                    program_action_host->HandleExecutionEvent(event);
                }
                catch (const std::exception& ex)
                {
                    EnterTainted(
                        std::string(
                            "Program action host execution ingress threw: ") +
                        ex.what());
                    return;
                }
                catch (...)
                {
                    EnterTainted(
                        "Program action host execution ingress threw");
                    return;
                }
            }
            bool taint_after_publish = false;
            std::string taint_diagnostic;
            if (event.terminal)
            {
                const ExecutionTerminalResult& terminal = *event.terminal;
                const auto pending = pending_execution_commands.find(
                    terminal.operation_id.value());
                if (pending != pending_execution_commands.end())
                {
                    const PendingExecutionCommand command = pending->second;
                    pending_execution_commands.erase(pending);
                    if (IsSuccessfulExecutionTerminal(
                            command.control,
                            terminal.status))
                    {
                        Complete(
                            command.command,
                            WorkerCommandOutcome::Completed,
                            {},
                            {},
                            {},
                            terminal.operation_id,
                            terminal);
                    }
                    else
                    {
                        WorkerRejectionCode code =
                            MapExecutionError(terminal.error.code);
                        if (code == WorkerRejectionCode::None)
                        {
                            switch (terminal.status)
                            {
                            case ExecutionTerminalStatus::Unsupported:
                                code = WorkerRejectionCode::Unsupported;
                                break;
                            case ExecutionTerminalStatus::StateEpochMismatch:
                                code = WorkerRejectionCode::StateEpochMismatch;
                                break;
                            case ExecutionTerminalStatus::BackendFailure:
                            case ExecutionTerminalStatus::CleanupFailure:
                                code = WorkerRejectionCode::BackendFailure;
                                break;
                            default:
                                code = WorkerRejectionCode::InvalidState;
                                break;
                            }
                        }
                        Complete(
                            command.command,
                            WorkerCommandOutcome::Rejected,
                            {},
                            RuntimeError{
                                code,
                                terminal.error.message.empty()
                                    ? "Execution control did not complete successfully"
                                    : terminal.error.message},
                            {},
                            terminal.operation_id,
                            terminal);
                    }
                }

                if (terminal.integrity != BackendIntegrity::Preserved ||
                    terminal.status ==
                        ExecutionTerminalStatus::CleanupFailure)
                {
                    taint_after_publish = true;
                    taint_diagnostic = terminal.error.message.empty()
                        ? "Execution control cleanup did not preserve session integrity"
                        : terminal.error.message;
                }
            }

            Publish(WorkerExecutionEvent{
                session->snapshot().session_id,
                std::move(event)});
            if (taint_after_publish)
            {
                EnterTainted(std::move(taint_diagnostic));
                break;
            }
        }
        PumpProgramActionHost();
        if (!finishing_shutdown &&
            !pending_shutdown_commands.empty() &&
            !active_invocation && session &&
            (session->execution_snapshot().activity ==
                    ExecutionActivity::IdlePaused ||
                session->execution_snapshot().activity ==
                    ExecutionActivity::Closed))
        {
            FinishShutdown(false);
        }
    }

    void HandleInvocationTerminal(ProgramInvocationTerminalEvent terminal)
    {
        if (!active_invocation ||
            terminal.invocation_id != active_invocation->invocation_id ||
            terminal.attempt_id != active_invocation->attempt_id)
        {
            Publish(WorkerRuntimeDiagnosticEvent{
                WorkerRejectionCode::InvocationMismatch,
                "Ignored stale, duplicate, or mismatched invocation completion",
                terminal.invocation_id});
            return;
        }

        ProgramRuntimeSubmission acknowledged;
        try
        {
            acknowledged = program_runtime
                ? program_runtime->AcknowledgeTerminal(
                      terminal.invocation_id,
                      terminal.attempt_id)
                : ProgramRuntimeSubmission::Rejected(
                      WorkerRejectionCode::ProgramRuntimeUnavailable,
                      "ProgramRuntime is unavailable for terminal "
                      "acknowledgement");
        }
        catch (const std::exception& ex)
        {
            acknowledged = ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InternalFailure,
                std::string(
                    "ProgramRuntime terminal acknowledgement threw: ") +
                    ex.what());
        }
        catch (...)
        {
            acknowledged = ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InternalFailure,
                "ProgramRuntime terminal acknowledgement threw");
        }

        // The actor queue is the cancellation/completion arbitration point. Once
        // an exact cancellation command has been accepted, a later successful
        // terminal cannot resurrect the invocation as completed.
        if (active_invocation->cancellation.is_cancellation_requested() &&
            terminal.status == InvocationTerminalStatus::Completed)
        {
            terminal.status = InvocationTerminalStatus::Cancelled;
            terminal.output_payload.clear();
        }

        std::string taint_reason;
        if (!acknowledged.accepted)
        {
            taint_reason = acknowledged.error.message.empty()
                ? "ProgramRuntime could not acknowledge its terminal"
                : acknowledged.error.message;
            terminal.status =
                InvocationTerminalStatus::InfrastructureFailure;
            terminal.cleanup = CleanupStatus::Failed;
            terminal.error = {
                acknowledged.error.code ==
                        WorkerRejectionCode::None
                    ? WorkerRejectionCode::InternalFailure
                    : acknowledged.error.code,
                taint_reason};
        }
        else if (terminal.origin_state_epoch !=
                 active_invocation->origin_epoch)
        {
            taint_reason =
                "ProgramRuntime returned an invocation completion for a stale StateEpoch";
            terminal.status = InvocationTerminalStatus::InfrastructureFailure;
            terminal.cleanup = CleanupStatus::Failed;
            terminal.error = {
                WorkerRejectionCode::StateEpochMismatch,
                taint_reason};
        }
        else if (terminal.cleanup == CleanupStatus::Failed ||
                 terminal.status == InvocationTerminalStatus::CleanupFailure ||
                 terminal.session_disposition == SessionDisposition::Tainted ||
                 terminal.session_disposition == SessionDisposition::Closed)
        {
            taint_reason = terminal.error.message.empty()
                ? "Invocation cleanup did not prove a reusable session"
                : terminal.error.message;
            terminal.status = InvocationTerminalStatus::CleanupFailure;
            terminal.cleanup = CleanupStatus::Failed;
            if (!terminal.error)
            {
                terminal.error = {
                    WorkerRejectionCode::SessionTainted,
                    taint_reason};
            }
        }

        if (taint_reason.empty() &&
            screenshot_on_terminal &&
            !terminal_screenshot_directory.empty())
        {
            const std::filesystem::path screenshot_path =
                terminal_screenshot_directory /
                ("invocation-" +
                 std::to_string(terminal.invocation_id.value()) +
                 "-terminal.png");
            SessionOperationReceipt screenshot = session->CaptureScreenshot(
                screenshot_path,
                terminal_screenshot_timeout);
            RefreshSnapshot();
            if (!screenshot.ok)
            {
                Publish(WorkerRuntimeDiagnosticEvent{
                    MapBackendError(screenshot.backend.code),
                    screenshot.backend.message.empty()
                        ? "Terminal screenshot capture failed"
                        : screenshot.backend.message,
                    terminal.invocation_id});
                if (screenshot.disposition == SessionDisposition::Tainted)
                {
                    taint_reason = screenshot.backend.message.empty()
                        ? "Terminal screenshot left session integrity unknown"
                        : screenshot.backend.message;
                    terminal.status = InvocationTerminalStatus::CleanupFailure;
                    terminal.cleanup = CleanupStatus::Failed;
                    terminal.error = {
                        WorkerRejectionCode::SessionTainted,
                        taint_reason};
                }
            }
        }

        if (!taint_reason.empty())
        {
            EnterTainted(taint_reason, false);
        }
        else if (terminal.cleanup == CleanupStatus::CleanWithDiagnostics ||
                 terminal.session_disposition ==
                     SessionDisposition::CleanWithDiagnostics)
        {
            session->MarkCleanWithDiagnostics(
                terminal.error.message.empty()
                    ? "Invocation cleanup completed with diagnostics"
                    : terminal.error.message);
            active_invocation.reset();
        }
        else
        {
            active_invocation.reset();
        }
        RefreshSnapshot();
        terminal.session_disposition = session->snapshot().disposition;
        Publish(terminal);

        if (!pending_shutdown_commands.empty() ||
            Snapshot().state == WorkerState::Stopping)
        {
            FinishShutdown(false);
            return;
        }

        if (session->snapshot().disposition != SessionDisposition::Tainted)
            ChangeState(WorkerState::Ready);
    }

    void EnterTainted(
        std::string diagnostic,
        bool synthesize_terminal = true,
        bool notify_program_runtime = true)
    {
        if (diagnostic.empty())
            diagnostic = "Session integrity could not be proven";

        const std::optional<InvocationId> invocation = active_invocation
            ? std::optional<InvocationId>(active_invocation->invocation_id)
            : std::nullopt;
        std::optional<ProgramInvocationTerminalEvent> synthetic_terminal;
        if (synthesize_terminal && active_invocation)
        {
            synthetic_terminal = ProgramInvocationTerminalEvent{
                active_invocation->invocation_id,
                active_invocation->attempt_id,
                InvocationTerminalStatus::InfrastructureFailure,
                CleanupStatus::Failed,
                SessionDisposition::Tainted,
                active_invocation->origin_epoch,
                {},
                RuntimeError{
                    WorkerRejectionCode::SessionTainted,
                    diagnostic}};
        }

        if (active_invocation)
        {
            (void)active_invocation->cancellation.request_cancellation(
                CancellationReason::RuntimeFailure);
            if (program_action_host)
            {
                program_action_host->RequestCancellation(
                    active_invocation->invocation_id,
                    CancellationReason::RuntimeFailure);
            }
            if (notify_program_runtime)
            {
                (void)RequestProgramCancellation(
                    active_invocation->invocation_id);
            }
        }

        ShutdownProgramRuntimeOnce();
        ShutdownProgramActionHostOnce();

        for (auto& [_, pending] : pending_execution_commands)
        {
            Reject(
                pending.command,
                WorkerRejectionCode::SessionTainted,
                diagnostic);
        }
        pending_execution_commands.clear();

        if (session)
        {
            session->MarkTainted(diagnostic);
            (void)session->Shutdown();
        }

        active_invocation.reset();
        RefreshSnapshot();
        ChangeState(WorkerState::Tainted);
        if (synthetic_terminal)
            Publish(std::move(*synthetic_terminal));
        Publish(WorkerRuntimeDiagnosticEvent{
            WorkerRejectionCode::SessionTainted,
            std::move(diagnostic),
            invocation});
    }

    void ForceStop()
    {
        {
            std::lock_guard lock(mailbox->mutex);
            mailbox->accept_commands = false;
            mailbox->accept_program_events = false;
            mailbox->accept_program_actions = false;
            mailbox->accept_host_events = false;
        }

        if (active_invocation)
            EnterTainted(
                "WorkerRuntime was force-stopped during an active invocation");
        FinishShutdown(true);
    }

    void FinishShutdown(bool forced)
    {
        if (finishing_shutdown)
            return;
        finishing_shutdown = true;
        ShutdownProgramRuntimeOnce();
        ShutdownProgramActionHostOnce();

        SessionOperationReceipt shutdown_receipt;
        if (session)
            shutdown_receipt = session->Shutdown();
        else
            shutdown_receipt = {};
        if (session)
            DrainExecutionEvents();
        if (session &&
            session->snapshot().disposition ==
                SessionDisposition::Tainted &&
            shutdown_receipt.ok)
        {
            shutdown_receipt.ok = false;
            shutdown_receipt.disposition =
                SessionDisposition::Tainted;
            shutdown_receipt.backend = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                session->taint_diagnostic().empty()
                    ? "Execution shutdown tainted the session"
                    : session->taint_diagnostic(),
                BackendIntegrity::Unknown);
        }

        active_invocation.reset();
        RefreshSnapshot();
        ChangeState(WorkerState::Stopped);

        {
            std::lock_guard lock(mailbox->mutex);
            mailbox->accept_commands = false;
            mailbox->accept_program_events = false;
            mailbox->accept_program_actions = false;
            mailbox->accept_host_events = false;
        }

        for (const auto& command : pending_shutdown_commands)
        {
            if (forced)
            {
                Reject(
                    command,
                    WorkerRejectionCode::InternalFailure,
                    "WorkerRuntime was force-stopped before graceful unwind completed",
                    shutdown_receipt);
            }
            else if (!shutdown_receipt.ok ||
                     shutdown_receipt.disposition == SessionDisposition::Tainted)
            {
                Reject(
                    command,
                    shutdown_receipt.disposition ==
                            SessionDisposition::Tainted
                        ? WorkerRejectionCode::SessionTainted
                        : WorkerRejectionCode::BackendFailure,
                    shutdown_receipt.backend.message.empty()
                        ? "Worker stopped with a tainted session disposition"
                        : shutdown_receipt.backend.message,
                    shutdown_receipt);
            }
            else
            {
                Complete(
                    command,
                    WorkerCommandOutcome::Completed,
                    {},
                    {},
                    shutdown_receipt);
            }
        }
        pending_shutdown_commands.clear();

        for (auto& [_, pending] : pending_execution_commands)
        {
            Reject(
                pending.command,
                WorkerRejectionCode::RuntimeStopping,
                forced
                    ? "WorkerRuntime was force-stopped during execution control"
                    : "WorkerRuntime stopped before execution control completed");
        }
        pending_execution_commands.clear();

        DrainQueuedCommands(forced);
    }

    void ShutdownProgramRuntimeOnce() noexcept
    {
        StopProgramEventIngress();
        if (!program_runtime || program_runtime_shutdown)
            return;
        program_runtime_shutdown = true;
        program_runtime->Shutdown();
    }

    void ShutdownProgramActionHostOnce() noexcept
    {
        if (!program_action_host || program_action_host_shutdown)
            return;
        program_action_host_shutdown = true;
        program_action_host->Shutdown();
    }

    void StopProgramEventIngress() noexcept
    {
        std::lock_guard lock(mailbox->mutex);
        mailbox->accept_program_events = false;
        mailbox->accept_program_actions = false;
        mailbox->program_pump_queued = false;
        for (auto it = mailbox->items.begin(); it != mailbox->items.end();)
        {
            if (it->kind == MailboxItemKind::ProgramEvent ||
                it->kind ==
                    MailboxItemKind::ProgramActionRequest ||
                it->kind ==
                    MailboxItemKind::ProgramActionCompletion ||
                it->kind == MailboxItemKind::ProgramPump)
                it = mailbox->items.erase(it);
            else
                ++it;
        }
    }

    void DrainQueuedCommands(bool forced)
    {
        std::deque<MailboxItem> remaining;
        {
            std::lock_guard lock(mailbox->mutex);
            remaining.swap(mailbox->items);
        }

        for (MailboxItem& item : remaining)
        {
            if (!item.command)
                continue;
            if (!forced &&
                CommandKind(item.command->command) == WorkerCommandKind::Shutdown)
            {
                Complete(item.command, WorkerCommandOutcome::Completed);
            }
            else
            {
                Reject(
                    item.command,
                    WorkerRejectionCode::RuntimeStopping,
                    "WorkerRuntime stopped before the command could run");
            }
        }
    }

    void ChangeState(WorkerState next)
    {
        WorkerState previous;
        WorkerSnapshot current;
        {
            std::lock_guard lock(snapshot_mutex);
            previous = current_snapshot.state;
            current_snapshot.state = next;
            if (session)
            {
                current_snapshot.session = session->snapshot();
                current_snapshot.execution =
                    session->execution_snapshot();
            }
            current_snapshot.active_invocation = active_invocation
                ? std::optional<InvocationId>(active_invocation->invocation_id)
                : std::nullopt;
            current = current_snapshot;
        }
        if (previous != next)
            Publish(WorkerStateChangedEvent{previous, std::move(current)});
    }

    void RefreshSnapshot()
    {
        std::lock_guard lock(snapshot_mutex);
        if (session)
        {
            current_snapshot.session = session->snapshot();
            current_snapshot.execution = session->execution_snapshot();
        }
        current_snapshot.active_invocation = active_invocation
            ? std::optional<InvocationId>(active_invocation->invocation_id)
            : std::nullopt;
    }

    void Complete(
        const std::shared_ptr<QueuedCommand>& queued,
        WorkerCommandOutcome outcome,
        std::optional<InvocationId> invocation_id = {},
        RuntimeError error = {},
        std::optional<SessionOperationReceipt> session_receipt = {},
        std::optional<ExecutionOperationId> execution_operation_id = {},
        std::optional<ExecutionTerminalResult> execution_terminal = {})
    {
        if (queued->completed)
            return;

        WorkerCommandResult result;
        result.request_id = queued->request_id;
        result.command_sequence = queued->sequence;
        result.command_kind = CommandKind(queued->command);
        result.outcome = outcome;
        result.snapshot = Snapshot();
        result.invocation_id = invocation_id;
        result.session_receipt = std::move(session_receipt);
        if (const auto* execution =
                std::get_if<ControlExecutionCommand>(&queued->command))
        {
            result.execution_control = execution->control;
        }
        result.execution_operation_id = execution_operation_id;
        result.execution_terminal = std::move(execution_terminal);
        result.error = std::move(error);

        queued->completed = true;
        queued->completion.set_value(result);
        Publish(WorkerCommandCompletedEvent{std::move(result)});
    }

    void Reject(
        const std::shared_ptr<QueuedCommand>& queued,
        WorkerRejectionCode code,
        std::string message,
        std::optional<SessionOperationReceipt> session_receipt = {})
    {
        Complete(
            queued,
            WorkerCommandOutcome::Rejected,
            {},
            RuntimeError{code, std::move(message)},
            std::move(session_receipt));
    }

    template <typename Event>
    void Publish(Event event)
    {
        if (!event_sink)
            return;
        try
        {
            event_sink(WorkerEvent(std::move(event)));
        }
        catch (...)
        {
            // Event publication cannot break command serialization or session cleanup.
        }
    }

    std::shared_ptr<Mailbox> mailbox;
    std::shared_ptr<ProgramEventIngress> program_event_ingress;
    std::shared_ptr<ProgramActionIngress> program_action_ingress;
    std::unique_ptr<EmulationSession> session;
    std::unique_ptr<IProgramRuntimePort> program_runtime;
    std::unique_ptr<program::IProgramActionHost>
        program_action_host;
    WorkerEventSink event_sink;
    std::shared_ptr<const WorkerRuntimeTestHooks> test_hooks;
    WorkerCapabilityMask capabilities_value = 0;
    bool program_runtime_shutdown = false;
    bool program_action_host_shutdown = false;
    std::filesystem::path terminal_screenshot_directory;
    std::chrono::milliseconds terminal_screenshot_timeout{3000};
    bool screenshot_on_terminal = false;
    bool session_visual_intent = false;

    mutable std::mutex snapshot_mutex;
    WorkerSnapshot current_snapshot;
    std::optional<ActiveInvocation> active_invocation;
    std::unordered_map<std::uint64_t, PendingExecutionCommand>
        pending_execution_commands;
    std::vector<std::shared_ptr<QueuedCommand>> pending_shutdown_commands;
    bool finishing_shutdown = false;
    std::thread actor;
    std::mutex join_mutex;
};

WorkerRuntime::WorkerRuntime(
    std::unique_ptr<EmulationSession> session,
    std::unique_ptr<IProgramRuntimePort> program_runtime,
    WorkerEventSink event_sink,
    std::shared_ptr<const WorkerRuntimeTestHooks> test_hooks,
    std::unique_ptr<program::IProgramActionHost> action_host)
    : impl_(std::make_unique<Impl>(
          std::move(session),
          std::move(program_runtime),
          std::move(event_sink),
          std::move(test_hooks),
          std::move(action_host)))
{
}

WorkerRuntime::~WorkerRuntime() = default;

std::future<WorkerCommandResult> WorkerRuntime::Submit(
    WireRequestId request_id,
    WorkerCommand command)
{
    return impl_->Submit(request_id, std::move(command));
}

WorkerSnapshot WorkerRuntime::snapshot() const
{
    return impl_->Snapshot();
}

WorkerCapabilityMask WorkerRuntime::capabilities() const noexcept
{
    return impl_->capabilities_value;
}

bool WorkerRuntime::EnqueueHostEvent(
    std::string name,
    std::vector<std::uint8_t> encoded_payload)
{
    return impl_->EnqueueHostEvent(
        std::move(name),
        std::move(encoded_payload));
}

bool WorkerRuntime::EnqueueHostEvent(
    SessionId observed_session_id,
    StateEpoch observed_state_epoch,
    std::string name,
    std::vector<std::uint8_t> encoded_payload)
{
    return impl_->EnqueueHostEvent(
        observed_session_id,
        observed_state_epoch,
        std::move(name),
        std::move(encoded_payload));
}

void WorkerRuntime::WaitStopped()
{
    impl_->WaitStopped();
}

std::unique_ptr<WorkerRuntime> MakeProductionWorkerRuntime(
    SessionId session_id,
    WorkerEventSink event_sink)
{
    if (!session_id)
    {
        std::uint64_t generated =
            g_next_production_session_id.fetch_add(1, std::memory_order_relaxed);
        if (generated == 0)
        {
            generated =
                g_next_production_session_id.fetch_add(1, std::memory_order_relaxed);
        }
        session_id = SessionId(generated);
    }

    return std::make_unique<WorkerRuntime>(
        std::make_unique<EmulationSession>(
            session_id,
            MakeDolphinWrapperBackend()),
        nullptr,
        std::move(event_sink));
}

} // namespace savor::runtime
