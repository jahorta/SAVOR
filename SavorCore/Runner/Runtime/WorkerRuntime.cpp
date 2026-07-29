#include "WorkerRuntime.h"

#include "DolphinWrapperBackend.h"
#include "ProgramRuntime/Actions/SessionProgramActionHost.h"
#include "ProgramRuntime/Codec/ProgramCodecV1.h"
#include "ProgramRuntime/ProgramRuntime.h"
#include "ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "Worksets/ProgramBaseline.h"
#include "Worksets/StateArtifactFinalizer.h"
#include "Worksets/WorkerCompletionLedger.h"
#include "Worksets/WorksetStager.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
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
            [](const SubmitWorksetCommand&) { return WorkerCommandKind::SubmitWorkset; },
            [](const CancelInvocationCommand&) { return WorkerCommandKind::CancelInvocation; },
            [](const CancelWorksetItemCommand&) { return WorkerCommandKind::CancelWorksetItem; },
            [](const CancelWorksetCommand&) { return WorkerCommandKind::CancelWorkset; },
            [](const AcknowledgeTerminalCommand&) { return WorkerCommandKind::AcknowledgeTerminal; },
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

    class WorksetStagerNotifier final
        : public IWorksetStagerNotifier
    {
    public:
        explicit WorksetStagerNotifier(
            std::weak_ptr<Mailbox> mailbox)
            : mailbox_(std::move(mailbox))
        {
        }

        void NotifyWorksetStagingCompletion() noexcept override
        {
            if (const auto mailbox = mailbox_.lock())
                SignalMailbox(*mailbox);
        }

    private:
        std::weak_ptr<Mailbox> mailbox_;
    };

    class ArtifactFinalizerNotifier final
        : public IStateArtifactFinalizerNotifier
    {
    public:
        explicit ArtifactFinalizerNotifier(
            std::weak_ptr<Mailbox> mailbox)
            : mailbox_(std::move(mailbox))
        {
        }

        void NotifyStateArtifactFinalizerCompletion() noexcept override
        {
            if (const auto mailbox = mailbox_.lock())
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
        std::optional<WorkerWorksetId> workset_id;
        std::optional<WorkerWorksetItemId> workset_item_id;
        std::uint32_t workset_item_ordinal = 0;
        std::vector<StateArtifactFinalizationId>
            artifact_finalizations;
        bool artifact_publication_promoted = false;
        std::string artifact_finalization_failure;

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

    struct WorksetPackage
    {
        WorkerWorksetDefinition definition;
        std::vector<PreparedInvocationTemplateReceipt> prepared;
        std::vector<bool> cancelled;
        std::vector<bool> terminalized;
        std::uint32_t next_item = 0;
        WorkerWorksetState state = WorkerWorksetState::Validating;
        PreparedProgramBaselineReceipt baseline;
        std::uint32_t terminal_count = 0;
        std::uint32_t unstarted_count = 0;
        bool admission_closed = false;
        std::string admission_close_reason;
    };

    struct DrainingWorkset
    {
        std::uint32_t item_count = 0;
        std::uint32_t terminal_count = 0;
        std::uint32_t unstarted_count = 0;
        std::uint32_t unacknowledged = 0;
    };

    struct RetainedTerminal
    {
        WorkerWorksetItemTerminalEvent event;
        std::size_t retained_bytes = 0;
    };

    struct PendingWorksetStaging
    {
        WorksetStagingId staging_id;
        WorkerWorksetId workset_id;
        std::shared_ptr<QueuedCommand> command;
        bool cancelled = false;
    };

    struct ArtifactPublication
    {
        WorkerItemExecutionCorrelation item;
        StateArtifactId state_artifact_id;
        std::string logical_artifact_id;
        bool completed = false;
        std::optional<program::ArtifactReferenceValue> artifact;
        std::string failure;
        WorkerTerminalId terminal_id;
    };

    struct PendingFinalizedTerminal
    {
        WorkerWorksetItemTerminalEvent event;
        std::size_t reserved_bytes = 0;
        std::vector<StateArtifactFinalizationId> finalizations;
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
        std::unique_ptr<program::IProgramActionHost> action_host,
        std::shared_ptr<ProgramBaselineComponentRegistry>
            injected_baseline_components)
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
        baseline_components = injected_baseline_components
            ? std::move(injected_baseline_components)
            : std::make_shared<ProgramBaselineComponentRegistry>();
        baseline_components->Freeze();
        workset_stager = std::make_unique<WorksetStager>(
            workset_limits,
            baseline_components,
            std::make_shared<WorksetStagerNotifier>(mailbox));
        artifact_finalizer =
            std::make_unique<StateArtifactFinalizer>(
                workset_limits,
                std::make_shared<ArtifactFinalizerNotifier>(
                    mailbox));
        if (this->session)
        {
            workset_state = std::make_unique<WorksetStateCoordinator>(
                *this->session,
                workset_limits,
                baseline_components);
        }
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
                WorkerCapability::WorksetDispatch))
        {
            capabilities_value = AddCapability(
                capabilities_value,
                WorkerCapability::WorksetDispatch);
        }
        else if (this->program_runtime &&
                 this->program_action_host &&
                 HasCapability(
                     this->program_runtime->capabilities(),
                     WorkerCapability::ProgramInvocation))
        {
            // Focused development fakes retain the scalar lifecycle tests.
            // Production ProgramRuntime advertises WorksetDispatch and never
            // exposes this capability or transport.
            capabilities_value = AddCapability(
                capabilities_value,
                WorkerCapability::ProgramInvocation);
        }
        BuildRuntimeManifest();
        if (this->program_runtime)
        {
            this->program_runtime->BindActionSink(
                program_action_ingress);
        }

        current_snapshot.state = WorkerState::Starting;
        current_snapshot.capabilities = capabilities_value;
        current_snapshot.available_item_credits =
            workset_limits.maximum_item_credits;
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

    WorkerRuntimeManifest RuntimeManifest() const
    {
        std::lock_guard lock(manifest_mutex);
        return runtime_manifest_value;
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
        const CompletionLedgerResult ledger_bound =
            completion_ledger.BindActorThread();
        if (!ledger_bound.ok || !session)
        {
            ChangeState(WorkerState::Tainted);
            Publish(WorkerRuntimeDiagnosticEvent{
                WorkerRejectionCode::InternalFailure,
                !ledger_bound.ok
                    ? ledger_bound.message
                    : "WorkerRuntime was constructed without an EmulationSession",
                {}});
        }
        else
        {
            ChangeState(WorkerState::AwaitingSession);
        }

        for (;;)
        {
            // Capture the generation before draining host completions. A
            // stager/finalizer can publish immediately after a drain; loading
            // the generation only when entering the wait would then absorb
            // that notification and leave the completed command stranded
            // until some unrelated mailbox event arrived.
            const std::uint64_t observed_wake_generation =
                mailbox->wake_generation.load(std::memory_order_acquire);
            DrainWorksetStager();
            const std::uint64_t stable_ingress_generation =
                DrainAuthoritativeIngressToStable(false);
            PumpProgramActionHost();
            DrainArtifactFinalizers();
            (void)PublishReadyWorksetTerminals();

            bool external_command_waiting = false;
            {
                std::lock_guard lock(mailbox->mutex);
                external_command_waiting =
                    !mailbox->items.empty() &&
                    mailbox->items.front().kind ==
                        MailboxItemKind::Command;
            }
            if (external_command_waiting)
            {
                // Do not remove an external command from the mailbox until
                // authoritative CPU ingress and the action completions it
                // produced have been drained. PumpProgramActionHost inserts
                // those completions ahead of queued external commands.
                (void)DrainAuthoritativeIngressToStable(true);
                PumpProgramActionHost();
            }

            MailboxItem item;
            bool has_item = false;
            {
                std::lock_guard lock(mailbox->mutex);
                // A command may arrive after the boundary peek above. Do not
                // pop that newly arrived command until the next actor turn
                // performs the authoritative-ingress stability check for it.
                // Internal events may still be consumed immediately.
                if (!mailbox->items.empty() &&
                    (external_command_waiting ||
                     mailbox->items.front().kind !=
                         MailboxItemKind::Command))
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
                            observed_wake_generation ||
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
                [this, &queued](const SubmitWorksetCommand& command) {
                    HandleSubmitWorkset(queued, command);
                },
                [this, &queued](const CancelInvocationCommand& command) {
                    HandleCancel(queued, command);
                },
                [this, &queued](const CancelWorksetItemCommand& command) {
                    HandleCancelWorksetItem(queued, command);
                },
                [this, &queued](const CancelWorksetCommand& command) {
                    HandleCancelWorkset(queued, command);
                },
                [this, &queued](const AcknowledgeTerminalCommand& command) {
                    HandleAcknowledgeTerminal(queued, command);
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

        pending_module_commands.emplace(
            queued->sequence.value(),
            queued);
    }

    [[nodiscard]] static bool BaselinePolicyMatches(
        ProgramBaselineStateKind baseline,
        program::InvocationStatePolicy policy) noexcept
    {
        switch (baseline)
        {
        case ProgramBaselineStateKind::Boot:
            return policy == program::InvocationStatePolicy::Boot;
        case ProgramBaselineStateKind::Artifact:
            return policy ==
                program::InvocationStatePolicy::RestoreBaseline;
        case ProgramBaselineStateKind::CurrentSession:
            return policy ==
                program::InvocationStatePolicy::ContinueSession;
        }
        return false;
    }

    [[nodiscard]] bool SessionIsCleanIdle() const
    {
        if (!session)
            return false;
        const SessionSnapshot current = session->snapshot();
        const ExecutionSnapshot execution =
            session->execution_snapshot();
        const bool clean =
            current.disposition == SessionDisposition::Clean ||
            current.disposition ==
                SessionDisposition::CleanWithDiagnostics;
        return current.open && clean &&
            current.core_state == BackendCoreState::Paused &&
            execution.activity == ExecutionActivity::IdlePaused &&
            !execution.active_operation &&
            execution.interruption_depth == 0;
    }

    [[nodiscard]] static std::size_t
    DeclaredTerminalBytes(const WorksetPackage& package) noexcept
    {
        std::size_t result = 0;
        for (std::size_t ordinal = 0;
             ordinal < package.definition.items.size();
             ++ordinal)
        {
            if (ordinal < package.terminalized.size() &&
                package.terminalized[ordinal])
            {
                continue;
            }
            const std::size_t bytes =
                package.definition.items[ordinal]
                    .declared_terminal_bytes;
            if (result >
                std::numeric_limits<std::size_t>::max() - bytes)
            {
                return std::numeric_limits<std::size_t>::max();
            }
            result += bytes;
        }
        return result;
    }

    [[nodiscard]] std::size_t
    ResidentDeclaredTerminalBytes() const noexcept
    {
        const std::size_t active = active_workset
            ? DeclaredTerminalBytes(*active_workset)
            : 0;
        const std::size_t staged = staged_workset
            ? DeclaredTerminalBytes(*staged_workset)
            : 0;
        std::size_t combined = 0;
        if (active >
            std::numeric_limits<std::size_t>::max() - staged)
        {
            return std::numeric_limits<std::size_t>::max();
        }
        combined = active + staged;
        if (pending_workset_staging)
        {
            const auto* submit =
                std::get_if<SubmitWorksetCommand>(
                    &pending_workset_staging->command->command);
            if (submit)
            {
                for (const WorksetItemTemplate& item :
                     submit->definition.items)
                {
                    if (combined >
                        std::numeric_limits<std::size_t>::max() -
                            item.declared_terminal_bytes)
                    {
                        return std::numeric_limits<std::size_t>::max();
                    }
                    combined += item.declared_terminal_bytes;
                }
            }
        }
        return combined;
    }

    [[nodiscard]] bool WorksetIdentityConflicts(
        const WorkerWorksetDefinition& candidate) const
    {
        if ((active_workset &&
             active_workset->definition.workset_id ==
                 candidate.workset_id) ||
            (staged_workset &&
             staged_workset->definition.workset_id ==
                 candidate.workset_id) ||
            draining_worksets.contains(candidate.workset_id.value()) ||
            (pending_workset_staging &&
             pending_workset_staging->workset_id ==
                 candidate.workset_id))
        {
            return true;
        }

        std::unordered_set<std::uint64_t> item_ids;
        std::unordered_set<std::uint64_t> invocation_ids;
        const auto collect_package =
            [&](const std::optional<WorksetPackage>& package)
        {
            if (!package)
                return;
            for (const WorksetItemTemplate& item :
                 package->definition.items)
            {
                item_ids.insert(item.item_id.value());
                invocation_ids.insert(
                    item.invocation.invocation_id.value());
            }
        };
        collect_package(active_workset);
        collect_package(staged_workset);
        if (pending_workset_staging)
        {
            if (const auto* pending =
                    std::get_if<SubmitWorksetCommand>(
                        &pending_workset_staging->command->command))
            {
                for (const WorksetItemTemplate& item :
                     pending->definition.items)
                {
                    item_ids.insert(item.item_id.value());
                    invocation_ids.insert(
                        item.invocation.invocation_id.value());
                }
            }
        }
        for (const auto& [_, terminal] : retained_terminals)
        {
            if (terminal.event.correlation.workset_id ==
                candidate.workset_id)
            {
                return true;
            }
            item_ids.insert(
                terminal.event.correlation.item_id.value());
            invocation_ids.insert(
                terminal.event.correlation.invocation_id.value());
        }
        for (const auto& [_, terminal] :
             pending_finalized_terminals)
        {
            if (terminal.event.correlation.workset_id ==
                candidate.workset_id)
            {
                return true;
            }
            item_ids.insert(
                terminal.event.correlation.item_id.value());
            invocation_ids.insert(
                terminal.event.correlation.invocation_id.value());
        }
        if (active_invocation)
        {
            invocation_ids.insert(
                active_invocation->invocation_id.value());
        }
        return std::ranges::any_of(
            candidate.items,
            [&](const WorksetItemTemplate& item)
            {
                return item_ids.contains(item.item_id.value()) ||
                    invocation_ids.contains(
                        item.invocation.invocation_id.value());
            });
    }

    [[nodiscard]] std::optional<WorksetPackage> ValidateAndStageWorkset(
        HostStagedWorksetPackage source,
        RuntimeError& error)
    {
        WorksetPackage package;
        package.definition = std::move(source.definition);
        const WorksetValidationResult validated =
            ValidateWorkerWorksetDefinition(
                package.definition,
                workset_limits);
        if (!validated.ok)
        {
            error = validated.error;
            return std::nullopt;
        }
        if (!program_runtime || !workset_state)
        {
            error = {
                WorkerRejectionCode::ProgramRuntimeUnavailable,
                "WorkerWorkset runtime services are unavailable"};
            return std::nullopt;
        }
        if (source.baseline_key !=
                package.definition.execution_key.baseline ||
            ComputeProgramBaselineKey(package.definition.baseline) !=
                source.baseline_key)
        {
            error = {
                WorkerRejectionCode::InvalidArgument,
                "Staged program baseline does not match its exact execution key"};
            return std::nullopt;
        }
        const ProgramRuntimeCatalogSnapshot catalog =
            program_runtime->catalog();
        if (catalog.runtime_profile_sha256.empty() ||
            package.definition.execution_key.runtime_profile_sha256 !=
                catalog.runtime_profile_sha256)
        {
            error = {
                WorkerRejectionCode::WorksetCatalogMismatch,
                "WorkerWorkset runtime profile does not match the installed runtime"};
            return std::nullopt;
        }

        package.cancelled.resize(
            package.definition.items.size(),
            false);
        package.terminalized.resize(
            package.definition.items.size(),
            false);
        package.prepared.reserve(package.definition.items.size());
        for (const WorksetItemTemplate& item :
             package.definition.items)
        {
            EncodedInvocationEnvelope envelope;
            envelope.invocation_id =
                item.invocation.invocation_id;
            envelope.attempt_id = item.invocation.attempt_id;
            envelope.module = item.invocation.module;
            envelope.entrypoint = item.invocation.entrypoint;
            envelope.expected_state_epoch = {};
            envelope.input_payload =
                item.invocation.template_payload;

            PreparedInvocationTemplateReceipt receipt;
            const ProgramRuntimeSubmission prepared =
                program_runtime->PrepareInvocationTemplate(
                    {
                        WorkerCommandSequence(
                            current_snapshot.last_command_sequence.value()),
                        std::move(envelope),
                    },
                    receipt);
            if (!prepared.accepted || !receipt ||
                receipt.invocation_id !=
                    item.invocation.invocation_id ||
                receipt.attempt_id !=
                    item.invocation.attempt_id ||
                receipt.module != item.invocation.module ||
                receipt.entrypoint !=
                    item.invocation.entrypoint ||
                receipt.program_compatibility_sha256 !=
                    package.definition.execution_key
                        .verified_dependency_sha256 ||
                receipt.active_budget !=
                    item.declared_active_budget ||
                !BaselinePolicyMatches(
                    package.definition.baseline.state_kind,
                    receipt.state_policy))
            {
                if (prepared.accepted && receipt.template_id)
                {
                    (void)program_runtime
                        ->ReleaseInvocationTemplate(
                            receipt.template_id);
                }
                for (const auto& staged : package.prepared)
                {
                    (void)program_runtime
                        ->ReleaseInvocationTemplate(
                            staged.template_id);
                }
                error = prepared.accepted
                    ? RuntimeError{
                          WorkerRejectionCode::
                              WorksetCatalogMismatch,
                          "Workset item does not match its exact execution key"}
                    : prepared.error;
                if (!error)
                {
                    error = {
                        WorkerRejectionCode::InvalidArgument,
                        "ProgramRuntime rejected a workset item template"};
                }
                return std::nullopt;
            }
            package.prepared.push_back(std::move(receipt));
        }
        package.state = WorkerWorksetState::Staged;
        return package;
    }

    void HandleSubmitWorkset(
        const std::shared_ptr<QueuedCommand>& queued,
        const SubmitWorksetCommand& command)
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
                "WorkerWorkset submission requires a ready or running session");
            return;
        }
        if (!HasCapability(
                capabilities_value,
                WorkerCapability::WorksetDispatch))
        {
            Reject(
                queued,
                WorkerRejectionCode::ProgramRuntimeUnavailable,
                "WorkerWorkset dispatch is unavailable");
            return;
        }
        if (staged_workset || pending_workset_staging)
        {
            Reject(
                queued,
                WorkerRejectionCode::WorksetAlreadyActive,
                "The worker already owns one staged successor workset");
            return;
        }
        if (WorksetIdentityConflicts(command.definition))
        {
            Reject(
                queued,
                WorkerRejectionCode::InvalidArgument,
                "WorkerWorkset reuses a resident, draining, or retained workset, item, or invocation identity");
            return;
        }
        if (command.definition.baseline.state_kind ==
            ProgramBaselineStateKind::CurrentSession)
        {
            const auto& guard =
                command.definition.baseline.current_session;
            const SessionSnapshot current = session->snapshot();
            if (!guard ||
                guard->session_id != current.session_id ||
                guard->state_epoch != current.state_epoch ||
                active_workset || active_invocation ||
                !SessionIsCleanIdle())
            {
                Reject(
                    queued,
                    WorkerRejectionCode::StateEpochMismatch,
                    "Current-session workset baseline is stale or the exact session is not clean and idle");
                return;
            }
        }
        if (!active_workset && !active_invocation &&
            !SessionIsCleanIdle())
        {
            Reject(
                queued,
                WorkerRejectionCode::InvalidState,
                "WorkerWorkset activation requires a clean, idle, paused session");
            return;
        }
        const std::size_t resident =
            ResidentItemCount() + command.definition.items.size();
        std::size_t candidate_terminal_bytes = 0;
        bool terminal_bytes_overflow = false;
        for (const WorksetItemTemplate& item :
             command.definition.items)
        {
            if (candidate_terminal_bytes >
                std::numeric_limits<std::size_t>::max() -
                    item.declared_terminal_bytes)
            {
                terminal_bytes_overflow = true;
                break;
            }
            candidate_terminal_bytes +=
                item.declared_terminal_bytes;
        }
        const std::size_t already_reserved =
            ResidentDeclaredTerminalBytes();
        std::size_t terminal_capacity =
            workset_limits.maximum_retained_terminal_bytes -
            std::min(
                retained_terminal_bytes,
                workset_limits.maximum_retained_terminal_bytes);
        const bool resident_terminal_capacity_exceeded =
            already_reserved > terminal_capacity;
        if (!resident_terminal_capacity_exceeded)
            terminal_capacity -= already_reserved;
        const bool terminal_capacity_exceeded =
            terminal_bytes_overflow ||
            retained_terminal_bytes >
                workset_limits.maximum_retained_terminal_bytes ||
            resident_terminal_capacity_exceeded ||
            candidate_terminal_bytes > terminal_capacity;
        if (resident >
                workset_limits.maximum_active_and_staged_items ||
            command.definition.items.size() >
                AvailableItemCredits() ||
            terminal_capacity_exceeded)
        {
            Reject(
                queued,
                WorkerRejectionCode::CapacityExceeded,
                "WorkerWorkset exceeds the worker's negotiated item credits");
            return;
        }

        if (!workset_stager)
        {
            Reject(
                queued,
                WorkerRejectionCode::ProgramRuntimeUnavailable,
                "Host-only WorkerWorkset staging is unavailable");
            return;
        }
        const WorksetStagingSubmission submitted =
            workset_stager->Submit(command.definition);
        if (!submitted.result.ok)
        {
            Reject(
                queued,
                submitted.result.code ==
                        WorksetStagerErrorCode::CapacityExceeded
                    ? WorkerRejectionCode::CapacityExceeded
                    : WorkerRejectionCode::InvalidArgument,
                submitted.result.message.empty()
                    ? "WorkerWorkset host staging was rejected"
                    : submitted.result.message);
            return;
        }
        pending_workset_staging.emplace(PendingWorksetStaging{
            submitted.staging_id,
            command.definition.workset_id,
            queued,
            false});
        RefreshSnapshot();
        PublishWorksetState(
            command.definition.workset_id,
            WorkerWorksetState::Validating);
        PublishCredits();
    }

    void DrainWorksetStager()
    {
        if (!workset_stager)
            return;
        for (WorksetStagingCompletion& completion :
             workset_stager->DrainCompletions())
        {
            if (!pending_workset_staging ||
                completion.staging_id !=
                    pending_workset_staging->staging_id ||
                completion.workset_id !=
                    pending_workset_staging->workset_id)
            {
                EnterTainted(
                    "Host-only workset staging returned an unknown completion");
                continue;
            }
            const auto pending =
                std::move(*pending_workset_staging);
            pending_workset_staging.reset();
            if (pending.cancelled)
            {
                Reject(
                    pending.command,
                    WorkerRejectionCode::RuntimeStopping,
                    "WorkerWorkset submission was cancelled during host staging");
                RefreshSnapshot();
                PublishCredits();
                continue;
            }
            if (!completion.result.ok || !completion.package)
            {
                Reject(
                    pending.command,
                    completion.result.code ==
                            WorksetStagerErrorCode::CapacityExceeded
                        ? WorkerRejectionCode::CapacityExceeded
                        : WorkerRejectionCode::InvalidArgument,
                    completion.result.message.empty()
                        ? "WorkerWorkset host staging failed"
                        : completion.result.message);
                RefreshSnapshot();
                PublishCredits();
                continue;
            }
            if (Snapshot().state == WorkerState::Stopping ||
                Snapshot().state == WorkerState::Stopped ||
                Snapshot().state == WorkerState::Tainted)
            {
                Reject(
                    pending.command,
                    WorkerRejectionCode::RuntimeStopping,
                    "Worker stopped before host-staged workset admission");
                RefreshSnapshot();
                PublishCredits();
                continue;
            }
            if (completion.package->definition.baseline.state_kind ==
                ProgramBaselineStateKind::CurrentSession)
            {
                const auto& guard = completion.package->definition
                                        .baseline.current_session;
                const SessionSnapshot current = session->snapshot();
                if (!guard ||
                    guard->session_id != current.session_id ||
                    guard->state_epoch != current.state_epoch ||
                    active_workset || active_invocation ||
                    !SessionIsCleanIdle())
                {
                    Reject(
                        pending.command,
                        WorkerRejectionCode::StateEpochMismatch,
                        "Current-session workset changed while host staging was in progress");
                    RefreshSnapshot();
                    PublishCredits();
                    continue;
                }
            }

            RuntimeError error;
            std::optional<WorksetPackage> staged =
                ValidateAndStageWorkset(
                    std::move(*completion.package),
                    error);
            if (!staged)
            {
                Reject(
                    pending.command,
                    error.code == WorkerRejectionCode::None
                        ? WorkerRejectionCode::InvalidArgument
                        : error.code,
                    error.message.empty()
                        ? "WorkerWorkset validation failed"
                        : error.message);
                RefreshSnapshot();
                PublishCredits();
                continue;
            }
            if (staged_workset)
            {
                for (const auto& receipt : staged->prepared)
                {
                    (void)program_runtime
                        ->ReleaseInvocationTemplate(
                            receipt.template_id);
                }
                Reject(
                    pending.command,
                    WorkerRejectionCode::WorksetAlreadyActive,
                    "The staged-successor slot became occupied during host staging");
                RefreshSnapshot();
                PublishCredits();
                continue;
            }

            const bool activate_now =
                !active_workset && !active_invocation &&
                SessionIsCleanIdle();
            const WorkerWorksetId accepted_id =
                staged->definition.workset_id;
            if (activate_now)
                active_workset.emplace(std::move(*staged));
            else
                staged_workset.emplace(std::move(*staged));
            RefreshSnapshot();
            Complete(
                pending.command,
                WorkerCommandOutcome::Accepted);
            PublishWorksetState(
                accepted_id,
                WorkerWorksetState::Staged);
            PublishCredits();
            if (activate_now)
                ActivateCurrentWorkset();
        }
    }

    void ActivateCurrentWorkset()
    {
        if (!active_workset || active_invocation ||
            Snapshot().state == WorkerState::Tainted ||
            Snapshot().state == WorkerState::Stopping)
        {
            return;
        }
        if (!SessionIsCleanIdle())
        {
            FailRemainingWorksetItems(
                WorkerRejectionCode::InvalidState,
                "WorkerWorkset activation found a non-idle session");
            FinishCurrentWorkset(WorkerWorksetState::Failed);
            return;
        }
        active_workset->state =
            WorkerWorksetState::PreparingBaseline;
        PublishWorksetState(
            active_workset->definition.workset_id,
            active_workset->state);
        ProgramBaselineComponentResult prepared =
            workset_state->Prepare(
                active_workset->definition.workset_id,
                active_workset->definition.baseline,
                active_workset->definition.items.size() > 1,
                active_workset->baseline);
        if (!prepared.ok)
        {
            FailRemainingWorksetItems(
                prepared.error.code == WorkerRejectionCode::None
                    ? WorkerRejectionCode::BackendFailure
                    : prepared.error.code,
                prepared.error.message.empty()
                    ? "Program baseline preparation failed"
                    : prepared.error.message);
            FinishCurrentWorkset(WorkerWorksetState::Failed);
            if (prepared.error.code ==
                WorkerRejectionCode::SessionTainted)
            {
                EnterTainted(prepared.error.message);
            }
            return;
        }
        active_workset->state = WorkerWorksetState::Running;
        PublishWorksetState(
            active_workset->definition.workset_id,
            active_workset->state);
        StartNextWorksetItem();
    }

    void StartNextWorksetItem()
    {
        if (!active_workset || active_invocation)
            return;
        if (retained_terminals.size() +
                pending_finalized_terminals.size() >=
                workset_limits.maximum_retained_terminals ||
            retained_terminal_bytes >=
                workset_limits.maximum_retained_terminal_bytes)
        {
            PublishCredits();
            return;
        }

        while (active_workset->next_item <
               active_workset->definition.items.size())
        {
            const std::uint32_t ordinal =
                active_workset->next_item;
            if (active_workset->cancelled[ordinal] &&
                !active_workset->terminalized[ordinal])
            {
                RetainUnstartedTerminal(
                    *active_workset,
                    ordinal,
                    active_workset->admission_close_reason.empty()
                        ? "Workset item was cancelled before admission"
                        : active_workset
                              ->admission_close_reason);
            }
            if (!active_workset->cancelled[ordinal] &&
                !active_workset->terminalized[ordinal])
                break;
            ++active_workset->next_item;
        }
        if (active_workset->next_item >=
            active_workset->definition.items.size())
        {
            FinishCurrentWorkset(
                active_workset->admission_closed
                    ? WorkerWorksetState::Cancelled
                    : WorkerWorksetState::Completed);
            return;
        }

        if (active_workset->next_item != 0)
        {
            ProgramBaselineComponentResult restored =
                workset_state->RestoreForNextItem(
                    active_workset->baseline);
            if (!restored.ok)
            {
                FailRemainingWorksetItems(
                    restored.error.code ==
                            WorkerRejectionCode::None
                        ? WorkerRejectionCode::BackendFailure
                        : restored.error.code,
                    restored.error.message.empty()
                        ? "Workset baseline restore failed"
                        : restored.error.message);
                FinishCurrentWorkset(WorkerWorksetState::Failed);
                if (restored.error.code ==
                    WorkerRejectionCode::SessionTainted)
                {
                    EnterTainted(restored.error.message);
                }
                return;
            }
        }

        const std::uint32_t ordinal =
            active_workset->next_item;
        const WorksetItemTemplate& item =
            active_workset->definition.items[ordinal];
        const PreparedInvocationTemplateReceipt& prepared =
            active_workset->prepared[ordinal];
        const SessionSnapshot current = session->snapshot();
        const WorkerOutboundSequence start_sequence =
            NextOutboundSequence();
        if (!start_sequence)
            return;
        if (!Publish(WorkerWorksetItemStartedEvent{
            start_sequence,
            active_workset->definition.workset_id,
            item.item_id,
            ordinal,
            item.invocation.invocation_id,
            item.invocation.attempt_id,
            current.session_id,
            current.state_epoch,
            active_workset->baseline}))
        {
            EnterTainted(
                "WorkerWorkset item-start event could not be published before execution");
            return;
        }
        active_invocation.emplace(
            item.invocation.invocation_id,
            item.invocation.attempt_id,
            current.state_epoch);
        active_invocation->workset_id =
            active_workset->definition.workset_id;
        active_invocation->workset_item_id = item.item_id;
        active_invocation->workset_item_ordinal = ordinal;

        ProgramRuntimeSubmission submission;
        bool start_threw = false;
        try
        {
            submission = program_runtime->StartPreparedInvocation(
                {
                    current_snapshot.last_command_sequence,
                    prepared.template_id,
                    current.session_id,
                    current.state_epoch,
                    active_workset->baseline.key.sha256,
                    active_workset->baseline.lineage,
                },
                active_invocation->cancellation.token(),
                program_event_ingress);
        }
        catch (const std::exception& ex)
        {
            start_threw = true;
            submission = ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InternalFailure,
                std::string(
                    "Prepared invocation submission threw: ") +
                    ex.what());
        }
        catch (...)
        {
            start_threw = true;
            submission = ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InternalFailure,
                "Prepared invocation submission threw");
        }
        if (!submission.accepted)
        {
            ProgramRuntimeSubmission released;
            try
            {
                released =
                    program_runtime->ReleaseInvocationTemplate(
                        prepared.template_id);
            }
            catch (...)
            {
                released = ProgramRuntimeSubmission::Rejected(
                    WorkerRejectionCode::InternalFailure,
                    "Prepared invocation release threw");
            }
            std::string failure =
                submission.error.message.empty()
                ? "Prepared invocation start was rejected"
                : submission.error.message;
            if (start_threw)
                failure = "Uncertain prepared invocation start: " + failure;
            if (!released.accepted)
            {
                failure += "; prepared template cleanup was not proven";
            }
            active_invocation.reset();
            session->MarkTainted(failure);
            ProgramInvocationTerminalEvent terminal;
            terminal.invocation_id =
                item.invocation.invocation_id;
            terminal.attempt_id = item.invocation.attempt_id;
            terminal.status =
                InvocationTerminalStatus::CleanupFailure;
            terminal.cleanup = CleanupStatus::Failed;
            terminal.session_disposition =
                SessionDisposition::Tainted;
            terminal.origin_state_epoch =
                session->snapshot().state_epoch;
            terminal.error = {
                submission.error.code ==
                        WorkerRejectionCode::None
                    ? WorkerRejectionCode::InternalFailure
                    : submission.error.code,
                failure};
            RetainWorksetTerminal(
                *active_workset,
                ordinal,
                std::move(terminal),
                false);
            ++active_workset->next_item;
            FailRemainingWorksetItems(
                WorkerRejectionCode::SessionTainted,
                failure);
            WorksetPackage failed =
                std::move(*active_workset);
            active_workset.reset();
            const ProgramBaselineComponentResult
                baseline_release = workset_state->Release();
            if (!baseline_release.ok &&
                !baseline_release.error.message.empty())
            {
                failure += "; " +
                    baseline_release.error.message;
            }
            MoveToDraining(
                failed,
                WorkerWorksetState::Failed);
            EnterTainted(failure, false);
            return;
        }
        RefreshSnapshot();
        ChangeState(WorkerState::Running);
        QueueProgramPump();
    }

    void HandleInvoke(
        const std::shared_ptr<QueuedCommand>& queued,
        const InvokeProgramCommand& command)
    {
        if (program_runtime &&
            HasCapability(
                program_runtime->capabilities(),
                WorkerCapability::WorksetDispatch))
        {
            Reject(
                queued,
                WorkerRejectionCode::Unsupported,
                "Scalar invocation submission is retired; submit a one-item WorkerWorkset");
            return;
        }
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
        if (active_invocation->artifact_publication_promoted)
        {
            CloseActiveWorksetAdmissionAfterPublication(
                "Cancellation after state capture closed later workset admission");
        }

        Complete(
            queued,
            WorkerCommandOutcome::Accepted,
            command.invocation_id);
    }

    void HandleCancelWorksetItem(
        const std::shared_ptr<QueuedCommand>& queued,
        const CancelWorksetItemCommand& command)
    {
        WorksetPackage* package = nullptr;
        bool staged = false;
        if (active_workset &&
            active_workset->definition.workset_id ==
                command.workset_id)
        {
            package = &*active_workset;
        }
        else if (staged_workset &&
                 staged_workset->definition.workset_id ==
                     command.workset_id)
        {
            package = &*staged_workset;
            staged = true;
        }
        if (!package)
        {
            Reject(
                queued,
                WorkerRejectionCode::WorksetNotFound,
                "Cancellation does not identify a resident workset");
            return;
        }
        const auto found = std::ranges::find(
            package->definition.items,
            command.item_id,
            &WorksetItemTemplate::item_id);
        if (found == package->definition.items.end())
        {
            Reject(
                queued,
                WorkerRejectionCode::WorksetItemNotFound,
                "Cancellation does not identify a workset item");
            return;
        }
        const std::uint32_t ordinal = static_cast<std::uint32_t>(
            std::distance(
                package->definition.items.begin(),
                found));
        if (package->cancelled[ordinal] ||
            (ordinal < package->terminalized.size() &&
             package->terminalized[ordinal]) ||
            (!staged && ordinal < package->next_item))
        {
            Reject(
                queued,
                WorkerRejectionCode::DuplicateCancellation,
                "Workset item is already terminal");
            return;
        }
        if (!staged && active_invocation &&
            active_invocation->workset_item_id ==
                command.item_id)
        {
            if (active_invocation->cancellation
                    .is_cancellation_requested())
            {
                Reject(
                    queued,
                    WorkerRejectionCode::DuplicateCancellation,
                    "Active workset item cancellation has already been requested");
                return;
            }
            if (!RequestActiveWorksetCancellation())
            {
                Reject(
                    queued,
                    WorkerRejectionCode::InternalFailure,
                    "Active workset item cancellation could not be delivered");
                return;
            }
            if (active_invocation &&
                active_invocation
                    ->artifact_publication_promoted)
            {
                CloseActiveWorksetAdmissionAfterPublication(
                    "Cancellation after state capture closed later workset admission");
            }
        }
        else
        {
            package->cancelled[ordinal] = true;
            RetainUnstartedTerminal(
                *package,
                ordinal,
                "Workset item was cancelled before admission");
            (void)program_runtime->ReleaseInvocationTemplate(
                package->prepared[ordinal].template_id);
            if (!staged &&
                ordinal == package->next_item &&
                !active_invocation)
            {
                StartNextWorksetItem();
            }
        }
        RefreshSnapshot();
        Complete(queued, WorkerCommandOutcome::Accepted);
        PublishCredits();
    }

    void HandleCancelWorkset(
        const std::shared_ptr<QueuedCommand>& queued,
        const CancelWorksetCommand& command)
    {
        if (pending_workset_staging &&
            pending_workset_staging->workset_id ==
                command.workset_id)
        {
            if (pending_workset_staging->cancelled)
            {
                Reject(
                    queued,
                    WorkerRejectionCode::DuplicateCancellation,
                    "Host-staged workset cancellation has already been requested");
                return;
            }
            pending_workset_staging->cancelled = true;
            RefreshSnapshot();
            Complete(
                queued,
                WorkerCommandOutcome::Accepted);
            PublishCredits();
            return;
        }
        if (staged_workset &&
            staged_workset->definition.workset_id ==
                command.workset_id)
        {
            WorksetPackage cancelled =
                std::move(*staged_workset);
            staged_workset.reset();
            cancelled.admission_closed = true;
            for (std::uint32_t ordinal = 0;
                 ordinal < cancelled.definition.items.size();
                 ++ordinal)
            {
                if (!cancelled.cancelled[ordinal] &&
                    !cancelled.terminalized[ordinal])
                {
                    cancelled.cancelled[ordinal] = true;
                    RetainUnstartedTerminal(
                        cancelled,
                        ordinal,
                        "Staged workset was cancelled");
                    (void)program_runtime
                        ->ReleaseInvocationTemplate(
                            cancelled.prepared[ordinal]
                                .template_id);
                }
            }
            MoveToDraining(
                cancelled,
                WorkerWorksetState::Cancelled);
            RefreshSnapshot();
            Complete(queued, WorkerCommandOutcome::Accepted);
            PublishCredits();
            return;
        }
        if (!active_workset ||
            active_workset->definition.workset_id !=
                command.workset_id)
        {
            Reject(
                queued,
                WorkerRejectionCode::WorksetNotFound,
                "Cancellation does not identify the active workset");
            return;
        }
        if (active_workset->admission_closed)
        {
            Reject(
                queued,
                WorkerRejectionCode::DuplicateCancellation,
                "Workset cancellation has already closed admission");
            return;
        }
        active_workset->admission_closed = true;
        for (std::uint32_t ordinal =
                 active_workset->next_item;
             ordinal < active_workset->definition.items.size();
             ++ordinal)
        {
            if (active_invocation &&
                active_invocation->workset_item_ordinal == ordinal)
            {
                continue;
            }
            if (!active_workset->cancelled[ordinal] &&
                !active_workset->terminalized[ordinal])
            {
                active_workset->cancelled[ordinal] = true;
                RetainUnstartedTerminal(
                    *active_workset,
                    ordinal,
                    "Workset was cancelled before item admission");
                (void)program_runtime
                    ->ReleaseInvocationTemplate(
                        active_workset->prepared[ordinal]
                            .template_id);
            }
        }
        if (active_invocation &&
            !RequestActiveWorksetCancellation())
        {
            Reject(
                queued,
                WorkerRejectionCode::InternalFailure,
                "Active workset cancellation could not be delivered");
            return;
        }
        if (!active_invocation)
            StartNextWorksetItem();
        RefreshSnapshot();
        Complete(queued, WorkerCommandOutcome::Accepted);
        PublishCredits();
    }

    void HandleAcknowledgeTerminal(
        const std::shared_ptr<QueuedCommand>& queued,
        const AcknowledgeTerminalCommand& command)
    {
        const auto found =
            retained_terminals.find(
                command.correlation.terminal_id.value());
        if (found == retained_terminals.end())
        {
            const CompletionLedgerResult retry =
                completion_ledger.AcknowledgeTerminal(
                    command.correlation);
            if (retry.ok)
            {
                Complete(
                    queued,
                    WorkerCommandOutcome::Completed);
            }
            else
            {
                Reject(
                    queued,
                    retry.code ==
                            CompletionLedgerErrorCode::
                                TerminalMismatch
                        ? WorkerRejectionCode::TerminalMismatch
                        : WorkerRejectionCode::TerminalNotFound,
                    retry.message.empty()
                        ? "Terminal acknowledgement is stale or unknown"
                        : retry.message);
            }
            return;
        }
        const WorkerItemTerminalCorrelation& correlation =
            found->second.event.correlation;
        if (correlation != command.correlation)
        {
            Reject(
                queued,
                WorkerRejectionCode::TerminalMismatch,
                "Terminal acknowledgement correlation does not match");
            return;
        }
        const CompletionLedgerResult acknowledged =
            completion_ledger.AcknowledgeTerminal(correlation);
        if (!acknowledged.ok)
        {
            Reject(
                queued,
                WorkerRejectionCode::TerminalMismatch,
                acknowledged.message.empty()
                    ? "Completion ledger rejected the acknowledgement"
                    : acknowledged.message);
            return;
        }
        retained_terminal_bytes -= std::min(
            retained_terminal_bytes,
            found->second.retained_bytes);
        retained_terminals.erase(found);
        auto draining =
            draining_worksets.find(
                command.correlation.workset_id.value());
        if (draining != draining_worksets.end() &&
            draining->second.unacknowledged != 0)
        {
            --draining->second.unacknowledged;
            if (draining->second.unacknowledged == 0)
            {
                PublishWorksetSummary(
                    command.correlation.workset_id,
                    draining->second);
                draining_worksets.erase(draining);
            }
        }
        RefreshSnapshot();
        Complete(queued, WorkerCommandOutcome::Completed);
        PublishCredits();
        if (active_workset && !active_invocation)
            StartNextWorksetItem();
        else if (!active_workset && staged_workset)
            PromoteStagedWorkset();
    }

    [[nodiscard]] bool RequestActiveWorksetCancellation()
    {
        if (!active_invocation ||
            active_invocation->cancellation
                .is_cancellation_requested())
        {
            return false;
        }
        ProgramRuntimeSubmission submission =
            RequestProgramCancellation(
                active_invocation->invocation_id);
        if (!submission.accepted)
            return false;
        if (submission.terminal_already_published)
            return true;
        if (!active_invocation->cancellation.request_cancellation(
                CancellationReason::ExternalRequest))
        {
            return false;
        }
        if (program_action_host)
        {
            program_action_host->RequestCancellation(
                active_invocation->invocation_id,
                CancellationReason::ExternalRequest);
        }
        ChangeState(WorkerState::Cancelling);
        QueueProgramPump();
        return true;
    }

    [[nodiscard]] WorkerOutboundSequence NextOutboundSequence()
    {
        if (completion_ledger.snapshot().open_outbound_sequence)
        {
            if (!PublishReadyWorksetTerminals() ||
                completion_ledger.snapshot()
                    .open_outbound_sequence)
            {
                return {};
            }
        }
        const WorkerOutboundSequenceReceipt reserved =
            completion_ledger.ReserveOutboundSequence();
        if (!reserved.result.ok)
            return {};
        if (!completion_ledger
                 .ConfirmOutboundSequence(reserved.sequence)
                 .ok)
        {
            return {};
        }
        return reserved.sequence;
    }

    [[nodiscard]] std::size_t ResidentItemCount() const noexcept
    {
        const auto count = [](const std::optional<WorksetPackage>& package)
        {
            return package
                ? package->definition.items.size() -
                    std::min<std::size_t>(
                        package->terminal_count,
                        package->definition.items.size())
                : std::size_t{0};
        };
        std::size_t result =
            count(active_workset) + count(staged_workset);
        if (pending_workset_staging)
        {
            if (const auto* pending =
                    std::get_if<SubmitWorksetCommand>(
                        &pending_workset_staging->command->command))
            {
                result += pending->definition.items.size();
            }
        }
        return result;
    }

    [[nodiscard]] std::size_t CreditedItemCount() const
    {
        std::set<WorkerItemExecutionCorrelation> items;
        const auto collect_package =
            [&](const WorksetPackage& package)
        {
            for (std::size_t ordinal = 0;
                 ordinal < package.definition.items.size();
                 ++ordinal)
            {
                if (ordinal < package.terminalized.size() &&
                    package.terminalized[ordinal])
                {
                    continue;
                }
                const WorksetItemTemplate& item =
                    package.definition.items[ordinal];
                items.emplace(WorkerItemExecutionCorrelation{
                    package.definition.workset_id,
                    item.item_id,
                    static_cast<std::uint32_t>(ordinal),
                    item.invocation.invocation_id,
                    item.invocation.attempt_id});
            }
        };
        if (active_workset)
            collect_package(*active_workset);
        if (staged_workset)
            collect_package(*staged_workset);
        if (pending_workset_staging)
        {
            if (const auto* pending =
                    std::get_if<SubmitWorksetCommand>(
                        &pending_workset_staging->command->command))
            {
                for (std::size_t ordinal = 0;
                     ordinal < pending->definition.items.size();
                     ++ordinal)
                {
                    const WorksetItemTemplate& item =
                        pending->definition.items[ordinal];
                    items.emplace(WorkerItemExecutionCorrelation{
                        pending->definition.workset_id,
                        item.item_id,
                        static_cast<std::uint32_t>(ordinal),
                        item.invocation.invocation_id,
                        item.invocation.attempt_id});
                }
            }
        }
        for (const auto& [_, terminal] : retained_terminals)
            items.emplace(ExecutionCorrelation(
                terminal.event.correlation));
        for (const auto& [_, terminal] :
             pending_finalized_terminals)
        {
            items.emplace(ExecutionCorrelation(
                terminal.event.correlation));
        }
        for (const auto& [_, publication] :
             artifact_publications)
        {
            items.emplace(publication.item);
        }
        return items.size();
    }

    [[nodiscard]] std::uint32_t AvailableItemCredits() const
    {
        const std::size_t resident = ResidentItemCount();
        const std::size_t retained =
            retained_terminals.size() +
            pending_finalized_terminals.size();
        const std::size_t global_used = CreditedItemCount();
        const std::size_t global =
            global_used >= workset_limits.maximum_item_credits
            ? 0
            : workset_limits.maximum_item_credits - global_used;
        const std::size_t terminal_reserved = retained + resident;
        const std::size_t terminal =
            terminal_reserved >=
                    workset_limits.maximum_retained_terminals
            ? 0
            : workset_limits.maximum_retained_terminals -
                terminal_reserved;
        return static_cast<std::uint32_t>(
            std::min(global, terminal));
    }

    void PublishWorksetState(
        WorkerWorksetId workset_id,
        WorkerWorksetState state,
        RuntimeError error = {})
    {
        std::uint32_t next = 0;
        if (active_workset &&
            active_workset->definition.workset_id == workset_id)
        {
            next = active_workset->next_item;
        }
        else if (staged_workset &&
                 staged_workset->definition.workset_id == workset_id)
        {
            next = staged_workset->next_item;
        }
        const WorkerOutboundSequence sequence =
            NextOutboundSequence();
        if (!sequence)
            return;
        Publish(WorkerWorksetStateEvent{
            sequence,
            workset_id,
            state,
            next,
            std::move(error)});
    }

    void PublishCredits()
    {
        const WorkerOutboundSequence sequence =
            NextOutboundSequence();
        if (!sequence)
            return;
        Publish(WorkerWorksetCreditEvent{
            sequence,
            AvailableItemCredits(),
            static_cast<std::uint32_t>(ResidentItemCount()),
            static_cast<std::uint32_t>(
                retained_terminals.size() +
                pending_finalized_terminals.size())});
    }

    [[nodiscard]] bool PublishReadyWorksetTerminals()
    {
        for (;;)
        {
            const WorkerTerminalPublicationReceipt publication =
                completion_ledger.BeginNextPublication();
            if (!publication.result.ok)
            {
                EnterTainted(
                    publication.result.message.empty()
                        ? "Completion ledger could not publish a ready terminal"
                        : publication.result.message);
                return false;
            }
            if (!publication.publication)
                return true;

            const auto found = retained_terminals.find(
                publication.publication->correlation
                    .terminal_id.value());
            if (found == retained_terminals.end() ||
                found->second.event.correlation !=
                    publication.publication->correlation)
            {
                EnterTainted(
                    "Completion ledger publication lost its retained terminal");
                return false;
            }

            found->second.event.outbound_sequence =
                publication.publication->outbound_sequence;
            if (!Publish(found->second.event))
            {
                const CompletionLedgerResult aborted =
                    completion_ledger.AbortPublication(
                        publication.publication->correlation);
                if (!aborted.ok)
                {
                    EnterTainted(
                        aborted.message.empty()
                            ? "Failed terminal publication could not be retried"
                            : aborted.message);
                    return false;
                }
                // The exact retained event and its assigned outbound sequence
                // remain ready for a later actor turn.
                SignalMailbox(*mailbox);
                return false;
            }
            const CompletionLedgerResult published =
                completion_ledger.ConfirmPublished(
                    publication.publication->correlation);
            if (!published.ok)
            {
                EnterTainted(
                    published.message.empty()
                        ? "Completion ledger publication confirmation failed"
                        : published.message);
                return false;
            }
        }
    }

    void RetainWorksetTerminal(
        WorksetPackage& package,
        std::uint32_t ordinal,
        ProgramInvocationTerminalEvent terminal,
        bool unstarted,
        std::vector<StateArtifactFinalizationId>
            finalizations = {})
    {
        if (ordinal >= package.definition.items.size())
            return;
        if (ordinal < package.terminalized.size() &&
            package.terminalized[ordinal])
        {
            EnterTainted(
                "A workset item attempted to retain more than one terminal");
            return;
        }
        const std::size_t declared_bytes =
            package.definition.items[ordinal]
                .declared_terminal_bytes;
        const auto encoded_size =
            [](const ProgramInvocationTerminalEvent& value)
            {
                return sizeof(WorkerWorksetItemTerminalEvent) +
                    value.output_payload.size() +
                    value.error.message.size();
            };
        std::size_t encoded_bytes = encoded_size(terminal);
        if (encoded_bytes > declared_bytes)
        {
            terminal.status =
                InvocationTerminalStatus::InfrastructureFailure;
            terminal.output_payload.clear();
            terminal.error = {
                WorkerRejectionCode::CapacityExceeded,
                "Program result exceeded its declared terminal-byte reservation"};
            if (terminal.cleanup != CleanupStatus::Failed)
                terminal.cleanup = CleanupStatus::Clean;
            encoded_bytes = encoded_size(terminal);
        }
        if (encoded_bytes > declared_bytes ||
            declared_bytes <
                kMinimumWorksetTerminalReservationBytes)
        {
            EnterTainted(
                "A typed terminal cannot fit its validated byte reservation");
            return;
        }
        if (retained_terminals.size() +
                pending_finalized_terminals.size() >=
                workset_limits.maximum_retained_terminals ||
            declared_bytes >
                workset_limits.maximum_retained_terminal_bytes -
                    std::min(
                        retained_terminal_bytes,
                        workset_limits
                            .maximum_retained_terminal_bytes))
        {
            EnterTainted(
                "Authoritative workset terminal retention capacity was exhausted");
            return;
        }
        const WorksetItemTemplate& item =
            package.definition.items[ordinal];
        WorkerItemTerminalCorrelation proposed{
            package.definition.workset_id,
            item.item_id,
            ordinal,
            item.invocation.invocation_id,
            item.invocation.attempt_id,
            {},
            {}};
        const WorkerTerminalReservation reserved =
            completion_ledger.ReserveTerminal(
                proposed,
                declared_bytes);
        if (!reserved.result.ok)
        {
            EnterTainted(
                reserved.result.message.empty()
                    ? "Completion ledger could not reserve a terminal"
                    : reserved.result.message);
            return;
        }
        WorkerWorksetItemTerminalEvent event;
        event.correlation = reserved.correlation;
        event.terminal = std::move(terminal);
        event.unstarted = unstarted;
        retained_terminal_bytes += declared_bytes;
        package.terminalized[ordinal] = true;
        ++package.terminal_count;
        if (unstarted)
            ++package.unstarted_count;
        for (const StateArtifactFinalizationId id :
             finalizations)
        {
            const auto publication =
                artifact_publications.find(id.value());
            if (publication ==
                    artifact_publications.end() ||
                publication->second.item !=
                    ExecutionCorrelation(
                        reserved.correlation) ||
                publication->second.terminal_id)
            {
                EnterTainted(
                    "State artifact finalization lost its exact workset item correlation");
                return;
            }
            publication->second.terminal_id =
                reserved.correlation.terminal_id;
        }
        pending_finalized_terminals.emplace(
            reserved.correlation.terminal_id.value(),
            PendingFinalizedTerminal{
                std::move(event),
                declared_bytes,
                std::move(finalizations)});
        TryFinalizePendingTerminal(
            reserved.correlation.terminal_id);
    }

    void TryFinalizePendingTerminal(
        WorkerTerminalId terminal_id)
    {
        const auto pending =
            pending_finalized_terminals.find(
                terminal_id.value());
        if (pending == pending_finalized_terminals.end())
            return;
        for (const StateArtifactFinalizationId id :
             pending->second.finalizations)
        {
            const auto publication =
                artifact_publications.find(id.value());
            if (publication ==
                    artifact_publications.end())
            {
                EnterTainted(
                    "Pending terminal lost an artifact publication");
                return;
            }
            if (!publication->second.completed)
                return;
        }

        WorkerWorksetItemTerminalEvent event =
            std::move(pending->second.event);
        const std::size_t reserved_bytes =
            pending->second.reserved_bytes;
        bool artifact_failed = false;
        std::string artifact_failure;
        if (!pending->second.finalizations.empty())
        {
            const auto decoded =
                program::DecodeProgramResultV1(
                    event.terminal.output_payload);
            program::ProgramResult result;
            if (!decoded)
            {
                artifact_failed = true;
                artifact_failure =
                    decoded.status.message.empty()
                    ? "ProgramResult could not be decoded for artifact finalization"
                    : decoded.status.message;
            }
            else
            {
                result = std::move(*decoded.value);
                std::uint64_t next_sequence = 1;
                for (const program::ProgramArtifact& artifact :
                     result.artifacts)
                {
                    next_sequence = std::max(
                        next_sequence,
                        artifact.sequence.value() + 1);
                }
                for (const StateArtifactFinalizationId id :
                     pending->second.finalizations)
                {
                    const ArtifactPublication& publication =
                        artifact_publications.at(id.value());
                    if (!publication.failure.empty() ||
                        !publication.artifact)
                    {
                        artifact_failed = true;
                        artifact_failure =
                            publication.failure.empty()
                            ? "State artifact publication did not produce authoritative evidence"
                            : publication.failure;
                        break;
                    }
                    result.artifacts.push_back(
                        program::ProgramArtifact{
                            program::ProgramArtifactSequence(
                                next_sequence++),
                            *publication.artifact});
                }
                if (!artifact_failed)
                {
                    const program::EncodeResult encoded =
                        program::EncodeProgramResultV1(result);
                    if (!encoded)
                    {
                        artifact_failed = true;
                        artifact_failure =
                            encoded.status.message.empty()
                            ? "Final ProgramResult artifact encoding failed"
                            : encoded.status.message;
                    }
                    else
                    {
                        event.terminal.output_payload =
                            encoded.bytes;
                    }
                }
            }
        }
        if (artifact_failed)
        {
            event.terminal.status =
                InvocationTerminalStatus::InfrastructureFailure;
            event.terminal.output_payload.clear();
            event.terminal.error = {
                WorkerRejectionCode::BackendFailure,
                artifact_failure};
        }

        const auto encoded_size =
            [](const ProgramInvocationTerminalEvent& value)
            {
                return sizeof(WorkerWorksetItemTerminalEvent) +
                    value.output_payload.size() +
                    value.error.message.size();
            };
        std::size_t actual_bytes =
            encoded_size(event.terminal);
        if (actual_bytes > reserved_bytes)
        {
            event.terminal.status =
                InvocationTerminalStatus::InfrastructureFailure;
            event.terminal.output_payload.clear();
            event.terminal.error = {
                WorkerRejectionCode::CapacityExceeded,
                "Finalized ProgramResult exceeded its declared terminal-byte reservation"};
            event.terminal.cleanup = CleanupStatus::Clean;
            actual_bytes = encoded_size(event.terminal);
        }
        if (actual_bytes > reserved_bytes)
        {
            EnterTainted(
                "A finalized terminal cannot fit its validated byte reservation");
            return;
        }
        // The ledger's byte bound covers the complete retained event, not
        // only ProgramResult bytes. Its immutable payload is internal
        // retention evidence; the typed event remains authoritative.
        std::vector<std::uint8_t> ledger_payload(
            actual_bytes,
            0);
        std::copy(
            event.terminal.output_payload.begin(),
            event.terminal.output_payload.end(),
            ledger_payload.begin());
        const CompletionLedgerResult completed =
            completion_ledger.CompleteTerminal(
                event.correlation,
                std::move(ledger_payload));
        if (!completed.ok)
        {
            EnterTainted(
                completed.message.empty()
                    ? "Completion ledger could not complete the finalized terminal"
                    : completed.message);
            return;
        }
        retained_terminal_bytes -=
            std::min(
                retained_terminal_bytes,
                reserved_bytes);
        retained_terminal_bytes += actual_bytes;
        for (const StateArtifactFinalizationId id :
             pending->second.finalizations)
        {
            artifact_publications.erase(id.value());
        }
        retained_terminals.emplace(
            event.correlation.terminal_id.value(),
            RetainedTerminal{
                std::move(event),
                actual_bytes});
        pending_finalized_terminals.erase(pending);
        RefreshSnapshot();
        if (!terminal_publication_deferred)
        {
            if (!PublishReadyWorksetTerminals())
                return;
            PublishCredits();
        }
    }

    void RetainUnstartedTerminal(
        WorksetPackage& package,
        std::uint32_t ordinal,
        std::string message)
    {
        const WorksetItemTemplate& item =
            package.definition.items[ordinal];
        ProgramInvocationTerminalEvent terminal;
        terminal.invocation_id =
            item.invocation.invocation_id;
        terminal.attempt_id = item.invocation.attempt_id;
        terminal.status = InvocationTerminalStatus::Cancelled;
        terminal.cleanup = CleanupStatus::Clean;
        terminal.session_disposition = session
            ? session->snapshot().disposition
            : SessionDisposition::Closed;
        terminal.origin_state_epoch = session
            ? session->snapshot().state_epoch
            : StateEpoch{};
        terminal.error = {
            WorkerRejectionCode::InvalidState,
            std::move(message)};
        RetainWorksetTerminal(
            package,
            ordinal,
            std::move(terminal),
            true);
    }

    void FailRemainingWorksetItems(
        WorkerRejectionCode code,
        const std::string& message)
    {
        if (!active_workset)
            return;
        for (std::uint32_t ordinal =
                 active_workset->next_item;
             ordinal < active_workset->definition.items.size();
             ++ordinal)
        {
            if (active_workset->cancelled[ordinal] ||
                active_workset->terminalized[ordinal])
                continue;
            const WorksetItemTemplate& item =
                active_workset->definition.items[ordinal];
            ProgramInvocationTerminalEvent terminal;
            terminal.invocation_id =
                item.invocation.invocation_id;
            terminal.attempt_id = item.invocation.attempt_id;
            terminal.status =
                InvocationTerminalStatus::InfrastructureFailure;
            terminal.cleanup = CleanupStatus::Clean;
            terminal.session_disposition =
                session->snapshot().disposition;
            terminal.origin_state_epoch =
                session->snapshot().state_epoch;
            terminal.error = {code, message};
            RetainWorksetTerminal(
                *active_workset,
                ordinal,
                std::move(terminal),
                true);
            (void)program_runtime->ReleaseInvocationTemplate(
                active_workset->prepared[ordinal].template_id);
            active_workset->cancelled[ordinal] = true;
        }
        active_workset->next_item =
            static_cast<std::uint32_t>(
                active_workset->definition.items.size());
    }

    void MoveToDraining(
        const WorksetPackage& package,
        WorkerWorksetState terminal_state)
    {
        DrainingWorkset draining;
        draining.item_count =
            static_cast<std::uint32_t>(
                package.definition.items.size());
        draining.terminal_count = package.terminal_count;
        draining.unstarted_count = package.unstarted_count;
        draining.unacknowledged =
            static_cast<std::uint32_t>(
                std::ranges::count_if(
                    retained_terminals,
                    [&](const auto& entry)
                    {
                        return entry.second.event.correlation
                                   .workset_id ==
                            package.definition.workset_id;
                    }) +
                std::ranges::count_if(
                    pending_finalized_terminals,
                    [&](const auto& entry)
                    {
                        return entry.second.event.correlation
                                   .workset_id ==
                            package.definition.workset_id;
                    }));
        PublishWorksetState(
            package.definition.workset_id,
            terminal_state);
        if (draining.unacknowledged == 0)
        {
            PublishWorksetSummary(
                package.definition.workset_id,
                draining);
        }
        else
        {
            draining_worksets.insert_or_assign(
                package.definition.workset_id.value(),
                draining);
        }
    }

    void PublishWorksetSummary(
        WorkerWorksetId workset_id,
        const DrainingWorkset& draining)
    {
        const WorkerOutboundSequence sequence =
            NextOutboundSequence();
        if (!sequence)
            return;
        Publish(WorkerWorksetTerminalSummaryEvent{
            sequence,
            workset_id,
            draining.item_count,
            draining.terminal_count -
                std::min(
                    draining.terminal_count,
                    draining.unstarted_count),
            draining.unstarted_count});
    }

    void FinishCurrentWorkset(WorkerWorksetState terminal_state)
    {
        if (!active_workset)
            return;
        WorksetPackage finished =
            std::move(*active_workset);
        active_workset.reset();
        const ProgramBaselineComponentResult released =
            workset_state->Release();
        if (!released.ok)
        {
            terminal_state = WorkerWorksetState::Failed;
            if (released.error.code ==
                WorkerRejectionCode::SessionTainted)
            {
                EnterTainted(released.error.message);
            }
        }
        MoveToDraining(finished, terminal_state);
        RefreshSnapshot();
        if (session->snapshot().disposition ==
                SessionDisposition::Tainted ||
            Snapshot().state == WorkerState::Stopping)
        {
            return;
        }
        if (staged_workset)
            PromoteStagedWorkset();
        else
            ChangeState(WorkerState::Ready);
        PublishCredits();
    }

    void PromoteStagedWorkset()
    {
        if (active_workset || !staged_workset ||
            active_invocation)
        {
            return;
        }
        active_workset.emplace(
            std::move(*staged_workset));
        staged_workset.reset();
        RefreshSnapshot();
        ActivateCurrentWorkset();
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
        if (active_invocation || active_workset ||
            staged_workset || pending_workset_staging)
        {
            Reject(
                queued,
                WorkerRejectionCode::InvocationAlreadyActive,
                "Ready-session execution control is unavailable while work is resident or staging");
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
            command.control == WorkerExecutionControlKind::StepFrame;
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
            (!HasCapability(
                 capabilities_value,
                 WorkerCapability::WorksetDispatch) &&
             !HasCapability(
                 capabilities_value,
                 WorkerCapability::ProgramInvocation)))
        {
            Reject(
                queued,
                WorkerRejectionCode::ProgramRuntimeUnavailable,
                "Canonical ProgramRuntime workset dispatch is not available");
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

    void CloseActiveWorksetAdmissionAfterPublication(
        std::string_view reason)
    {
        if (!active_workset || !active_invocation ||
            active_workset->admission_closed)
        {
            return;
        }
        active_workset->admission_closed = true;
        for (std::uint32_t ordinal =
                 active_workset->next_item;
             ordinal < active_workset->definition.items.size();
             ++ordinal)
        {
            if (ordinal ==
                    active_invocation->workset_item_ordinal ||
                active_workset->cancelled[ordinal] ||
                active_workset->terminalized[ordinal])
            {
                continue;
            }
            active_workset->cancelled[ordinal] = true;
            (void)program_runtime->ReleaseInvocationTemplate(
                active_workset->prepared[ordinal].template_id);
        }
        active_workset->admission_close_reason =
            std::string(reason);
    }

    void PromotePendingArtifactPublications()
    {
        if (!program_runtime || !active_invocation)
            return;
        std::vector<program::PendingStateArtifactPublication>
            pending;
        try
        {
            pending = program_runtime
                ->DrainPendingStateArtifactPublications();
        }
        catch (const std::exception& exception)
        {
            EnterTainted(
                std::string(
                    "ProgramRuntime pending publication drain threw: ") +
                exception.what());
            return;
        }
        catch (...)
        {
            EnterTainted(
                "ProgramRuntime pending publication drain threw");
            return;
        }
        if (pending.empty())
            return;
        if (!active_invocation->workset_id ||
            !active_invocation->workset_item_id ||
            !artifact_finalizer)
        {
            for (auto& publication : pending)
            {
                (void)session->AbandonImmutableStateArtifact(
                    publication.capture.artifact);
            }
            EnterTainted(
                "Pending state-artifact publication has no exact workset item owner");
            return;
        }

        const WorkerItemExecutionCorrelation correlation{
            *active_invocation->workset_id,
            *active_invocation->workset_item_id,
            active_invocation->workset_item_ordinal,
            active_invocation->invocation_id,
            active_invocation->attempt_id};
        for (auto& publication : pending)
        {
            StateArtifactFinalizationRequest request;
            request.item = correlation;
            request.state_artifact_id =
                publication.capture.artifact;
            request.logical_artifact_id =
                publication.artifact_id;
            request.state.final_path =
                publication.capture.final_path;
            request.state.bytes = std::move(
                publication.capture.state_bytes);
            if (publication.capture.movie_bytes)
            {
                if (!publication.capture.movie ||
                    publication.capture.movie->dtm_path.empty())
                {
                    (void)session
                        ->AbandonImmutableStateArtifact(
                            publication.capture.artifact);
                    active_invocation
                        ->artifact_finalization_failure =
                        "Captured movie state has no exact sidecar path";
                    CloseActiveWorksetAdmissionAfterPublication(
                        "Artifact publication failure closed later workset admission");
                    (void)RequestActiveWorksetCancellation();
                    continue;
                }
                ImmutableArtifactFile sidecar;
                sidecar.final_path =
                    publication.capture.movie->dtm_path;
                sidecar.bytes = std::move(
                    *publication.capture.movie_bytes);
                sidecar.expected_sha256 =
                    publication.capture.movie->dtm_sha256;
                request.sidecars.push_back(
                    std::move(sidecar));
            }
            const StateArtifactId state_artifact_id =
                request.state_artifact_id;
            const std::string logical_artifact_id =
                request.logical_artifact_id;
            const StateArtifactFinalizerSubmission submitted =
                artifact_finalizer->Submit(std::move(request));
            if (!submitted.result.ok)
            {
                (void)session->AbandonImmutableStateArtifact(
                    state_artifact_id);
                active_invocation
                    ->artifact_finalization_failure =
                    submitted.result.message.empty()
                    ? "State artifact finalizer rejected captured bytes"
                    : submitted.result.message;
                CloseActiveWorksetAdmissionAfterPublication(
                    "Artifact publication failure closed later workset admission");
                (void)RequestActiveWorksetCancellation();
                continue;
            }
            active_invocation->artifact_publication_promoted =
                true;
            active_invocation->artifact_finalizations.push_back(
                submitted.finalization_id);
            artifact_publications.emplace(
                submitted.finalization_id.value(),
                ArtifactPublication{
                    correlation,
                    state_artifact_id,
                    logical_artifact_id});
        }
        RefreshSnapshot();
        PublishCredits();
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
        PromotePendingArtifactPublications();
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
                    if (module.prepared)
                        BuildRuntimeManifest();
                    const auto pending =
                        pending_module_commands.find(
                            module.command_sequence.value());
                    // Publish the refreshed module/catalog observation before
                    // resolving the command. Wire clients can therefore treat
                    // a successful PrepareModule completion as a barrier for
                    // the corresponding RuntimeManifest update.
                    Publish(module);
                    if (pending !=
                        pending_module_commands.end())
                    {
                        if (module.prepared)
                        {
                            Complete(
                                pending->second,
                                WorkerCommandOutcome::Completed);
                        }
                        else
                        {
                            Reject(
                                pending->second,
                                module.error.code ==
                                        WorkerRejectionCode::None
                                    ? WorkerRejectionCode::
                                          InvalidArgument
                                    : module.error.code,
                                module.error.message.empty()
                                    ? "Program module preparation failed"
                                    : module.error.message);
                        }
                        pending_module_commands.erase(pending);
                    }
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
        const std::optional<WorkerWorksetId> terminal_workset =
            active_invocation->workset_id;
        const std::optional<WorkerWorksetItemId>
            terminal_workset_item =
                active_invocation->workset_item_id;
        const std::uint32_t terminal_workset_ordinal =
            active_invocation->workset_item_ordinal;
        std::vector<StateArtifactFinalizationId>
            terminal_finalizations =
                active_invocation->artifact_finalizations;
        const bool artifact_publication_promoted =
            active_invocation->artifact_publication_promoted;
        const std::string artifact_finalization_failure =
            active_invocation->artifact_finalization_failure;

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
            if (!artifact_publication_promoted)
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
        else if (!artifact_finalization_failure.empty())
        {
            terminal.status =
                InvocationTerminalStatus::InfrastructureFailure;
            terminal.error = {
                WorkerRejectionCode::BackendFailure,
                artifact_finalization_failure};
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
            // Publish the tainted session/worker state before exposing the
            // authoritative terminal. Consumers that observe the terminal
            // must never race a still-Ready snapshot and admit more
            // mutating work.
            session->MarkTainted(taint_reason);
            RefreshSnapshot();
            ChangeState(WorkerState::Tainted);
            terminal.session_disposition = SessionDisposition::Tainted;
        }

        if (taint_reason.empty() &&
            (terminal.cleanup == CleanupStatus::CleanWithDiagnostics ||
                 terminal.session_disposition ==
                     SessionDisposition::CleanWithDiagnostics))
        {
            session->MarkCleanWithDiagnostics(
                terminal.error.message.empty()
                    ? "Invocation cleanup completed with diagnostics"
                    : terminal.error.message);
        }
        const bool tainted_terminal = !taint_reason.empty();
        const bool publication_was_deferred =
            terminal_publication_deferred;
        if (tainted_terminal)
            terminal_publication_deferred = true;
        std::optional<ProgramInvocationTerminalEvent>
            deferred_scalar_terminal;

        active_invocation.reset();
        RefreshSnapshot();
        terminal.session_disposition = session->snapshot().disposition;
        if (terminal_workset && terminal_workset_item &&
            active_workset &&
            active_workset->definition.workset_id ==
                *terminal_workset &&
            terminal_workset_ordinal <
                active_workset->definition.items.size())
        {
            RetainWorksetTerminal(
                *active_workset,
                terminal_workset_ordinal,
                std::move(terminal),
                false,
                std::move(terminal_finalizations));
            if (active_workset->next_item ==
                terminal_workset_ordinal)
            {
                ++active_workset->next_item;
            }
        }
        else if (tainted_terminal)
        {
            deferred_scalar_terminal.emplace(std::move(terminal));
        }
        else
        {
            Publish(terminal);
        }

        if (tainted_terminal)
        {
            if (active_workset)
            {
                FailRemainingWorksetItems(
                    WorkerRejectionCode::SessionTainted,
                    taint_reason);
                WorksetPackage failed =
                    std::move(*active_workset);
                active_workset.reset();
                (void)workset_state->Release();
                MoveToDraining(
                    failed,
                    WorkerWorksetState::Failed);
            }
            EnterTainted(taint_reason, false);
            terminal_publication_deferred =
                publication_was_deferred;
            if (!publication_was_deferred)
                (void)PublishReadyWorksetTerminals();
            if (deferred_scalar_terminal)
                Publish(std::move(*deferred_scalar_terminal));
            return;
        }

        if (!pending_shutdown_commands.empty() ||
            Snapshot().state == WorkerState::Stopping)
        {
            FinishShutdown(false);
            return;
        }

        if (active_workset)
        {
            StartNextWorksetItem();
        }
        else if (session->snapshot().disposition !=
                 SessionDisposition::Tainted)
        {
            ChangeState(WorkerState::Ready);
        }
    }

    void EnterTainted(
        std::string diagnostic,
        bool synthesize_terminal = true,
        bool notify_program_runtime = true)
    {
        if (diagnostic.empty())
            diagnostic = "Session integrity could not be proven";
        const bool publication_was_deferred =
            terminal_publication_deferred;
        terminal_publication_deferred = true;

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

        if (synthetic_terminal && active_workset &&
            active_invocation &&
            active_invocation->workset_id ==
                active_workset->definition.workset_id &&
            active_invocation->workset_item_id &&
            active_invocation->workset_item_ordinal <
                active_workset->definition.items.size())
        {
            const std::uint32_t ordinal =
                active_invocation->workset_item_ordinal;
            RetainWorksetTerminal(
                *active_workset,
                ordinal,
                std::move(*synthetic_terminal),
                false,
                active_invocation
                    ->artifact_finalizations);
            synthetic_terminal.reset();
            if (active_workset->next_item == ordinal)
                ++active_workset->next_item;
        }
        if (active_workset)
        {
            FailRemainingWorksetItems(
                WorkerRejectionCode::SessionTainted,
                diagnostic);
            WorksetPackage failed =
                std::move(*active_workset);
            active_workset.reset();
            const ProgramBaselineComponentResult released =
                workset_state->Release();
            if (!released.ok &&
                !released.error.message.empty())
            {
                diagnostic += "; " +
                    released.error.message;
            }
            MoveToDraining(
                failed,
                WorkerWorksetState::Failed);
        }
        if (staged_workset)
        {
            WorksetPackage failed =
                std::move(*staged_workset);
            staged_workset.reset();
            failed.admission_closed = true;
            for (std::uint32_t ordinal = 0;
                 ordinal < failed.definition.items.size();
                 ++ordinal)
            {
                if (failed.terminalized[ordinal])
                    continue;
                failed.cancelled[ordinal] = true;
                RetainUnstartedTerminal(
                    failed,
                    ordinal,
                    "Staged workset was not admitted because the session became tainted");
                try
                {
                    (void)program_runtime
                        ->ReleaseInvocationTemplate(
                            failed.prepared[ordinal]
                                .template_id);
                }
                catch (...)
                {
                    diagnostic +=
                        "; staged prepared-template cleanup threw";
                }
            }
            MoveToDraining(
                failed,
                WorkerWorksetState::Failed);
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
        terminal_publication_deferred = publication_was_deferred;
        if (!publication_was_deferred)
            (void)PublishReadyWorksetTerminals();
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
        if (workset_stager)
        {
            workset_stager->Shutdown();
            DrainWorksetStager();
        }
        if (active_workset)
        {
            FailRemainingWorksetItems(
                WorkerRejectionCode::RuntimeStopping,
                "Worker shutdown closed workset admission");
            WorksetPackage stopped =
                std::move(*active_workset);
            active_workset.reset();
            if (workset_state)
                (void)workset_state->Release();
            MoveToDraining(
                stopped,
                WorkerWorksetState::Cancelled);
        }
        if (staged_workset)
        {
            WorksetPackage stopped =
                std::move(*staged_workset);
            staged_workset.reset();
            for (std::uint32_t ordinal = 0;
                 ordinal < stopped.definition.items.size();
                 ++ordinal)
            {
                if (stopped.cancelled[ordinal] ||
                    stopped.terminalized[ordinal])
                    continue;
                stopped.cancelled[ordinal] = true;
                RetainUnstartedTerminal(
                    stopped,
                    ordinal,
                    "Worker shutdown cancelled the staged workset");
                (void)program_runtime
                    ->ReleaseInvocationTemplate(
                        stopped.prepared[ordinal].template_id);
            }
            MoveToDraining(
                stopped,
                WorkerWorksetState::Cancelled);
        }
        if (workset_state)
            (void)workset_state->Shutdown();
        if (artifact_finalizer)
            artifact_finalizer->Shutdown();
        DrainArtifactFinalizers();
        (void)PublishReadyWorksetTerminals();
        const std::size_t unacknowledged_terminals =
            std::max(
                retained_terminals.size() +
                    pending_finalized_terminals.size(),
                completion_ledger.snapshot().retained_terminals);
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
        else if (unacknowledged_terminals != 0 &&
                 shutdown_receipt.ok)
        {
            shutdown_receipt.ok = false;
            shutdown_receipt.backend = BackendResult::Failure(
                BackendErrorCode::OperationFailed,
                "Worker shutdown retained " +
                    std::to_string(unacknowledged_terminals) +
                    " unacknowledged authoritative terminal(s); "
                    "durable per-item recovery is required",
                BackendIntegrity::Preserved);
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
        for (auto& [_, pending] : pending_module_commands)
        {
            Reject(
                pending,
                WorkerRejectionCode::RuntimeStopping,
                "WorkerRuntime stopped before module preparation completed");
        }
        pending_module_commands.clear();

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
            current_snapshot.active_workset = active_workset
                ? std::optional<WorkerWorksetId>(
                      active_workset->definition.workset_id)
                : std::nullopt;
            current_snapshot.staged_workset = staged_workset
                ? std::optional<WorkerWorksetId>(
                      staged_workset->definition.workset_id)
                : pending_workset_staging
                ? std::optional<WorkerWorksetId>(
                      pending_workset_staging->workset_id)
                : std::nullopt;
            current_snapshot.active_workset_item =
                active_invocation &&
                    active_invocation->workset_item_id
                ? active_invocation->workset_item_id
                : std::nullopt;
            current_snapshot.available_item_credits =
                AvailableItemCredits();
            current_snapshot.retained_terminal_count =
                static_cast<std::uint32_t>(
                    retained_terminals.size() +
                    pending_finalized_terminals.size());
            current_snapshot.retained_terminal_bytes =
                retained_terminal_bytes;
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
        current_snapshot.active_workset = active_workset
            ? std::optional<WorkerWorksetId>(
                  active_workset->definition.workset_id)
            : std::nullopt;
        current_snapshot.staged_workset = staged_workset
            ? std::optional<WorkerWorksetId>(
                  staged_workset->definition.workset_id)
            : pending_workset_staging
            ? std::optional<WorkerWorksetId>(
                  pending_workset_staging->workset_id)
            : std::nullopt;
        current_snapshot.active_workset_item =
            active_invocation &&
                active_invocation->workset_item_id
            ? active_invocation->workset_item_id
            : std::nullopt;
        current_snapshot.available_item_credits =
            AvailableItemCredits();
        current_snapshot.retained_terminal_count =
            static_cast<std::uint32_t>(
                retained_terminals.size() +
                pending_finalized_terminals.size());
        current_snapshot.retained_terminal_bytes =
            retained_terminal_bytes;
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
    bool Publish(Event event)
    {
        if (!event_sink)
            return true;
        try
        {
            event_sink(WorkerEvent(std::move(event)));
            return true;
        }
        catch (...)
        {
            // Event publication cannot break command serialization or session cleanup.
            return false;
        }
    }

    void DrainArtifactFinalizers()
    {
        if (!artifact_finalizer)
            return;
        std::vector<StateArtifactFinalizationCompletion>
            completions =
                artifact_finalizer->DrainCompletions();
        if (completions.empty())
            return;
        for (StateArtifactFinalizationCompletion& completion :
             completions)
        {
            const auto found = artifact_publications.find(
                completion.finalization_id.value());
            if (found == artifact_publications.end() ||
                found->second.item != completion.item ||
                found->second.state_artifact_id !=
                    completion.state_artifact_id ||
                found->second.logical_artifact_id !=
                    completion.logical_artifact_id ||
                found->second.completed)
            {
                EnterTainted(
                    "State artifact finalizer returned a stale or mismatched completion");
                continue;
            }
            ArtifactPublication& publication = found->second;
            publication.completed = true;
            if (!completion.result.ok)
            {
                publication.failure =
                    completion.result.message.empty()
                    ? "State artifact finalization failed"
                    : completion.result.message;
                (void)session->AbandonImmutableStateArtifact(
                    publication.state_artifact_id);
            }
            else
            {
                ImmutableStateArtifactPublicationReceipt evidence;
                evidence.artifact =
                    publication.state_artifact_id;
                evidence.state_path = completion.state.path;
                evidence.state_size_bytes =
                    completion.state.size_bytes;
                evidence.state_sha256 =
                    completion.state.sha256;
                if (!completion.sidecars.empty())
                {
                    if (completion.sidecars.size() != 1)
                    {
                        publication.failure =
                            "State artifact finalization returned an unexpected sidecar set";
                    }
                    else
                    {
                        evidence.movie_path =
                            completion.sidecars.front().path;
                        evidence.movie_size_bytes =
                            completion.sidecars.front()
                                .size_bytes;
                        evidence.movie_sha256 =
                            completion.sidecars.front().sha256;
                    }
                }
                if (!publication.failure.empty())
                {
                    (void)session->AbandonImmutableStateArtifact(
                        publication.state_artifact_id);
                }
                if (publication.failure.empty())
                {
                    const StateFileArtifactReceipt committed =
                        session->CommitImmutableStateArtifact(
                            evidence);
                    if (!committed.result.ok)
                    {
                        publication.failure =
                            committed.result.message.empty()
                            ? "StateService rejected finalized artifact evidence"
                            : committed.result.message;
                        (void)session
                            ->AbandonImmutableStateArtifact(
                                publication.state_artifact_id);
                    }
                    else
                    {
                        const auto hash =
                            program::ContentHash256::FromHex(
                                committed.sha256);
                        const auto schema =
                            program::
                                CanonicalActionArtifactPayloadSchemaIdentity(
                                    program::CanonicalAction::
                                        StateSaveImmutableArtifact);
                        if (!hash || !schema)
                        {
                            publication.failure =
                                "Finalized state artifact has invalid canonical identity";
                        }
                        else
                        {
                            publication.artifact =
                                program::ArtifactReferenceValue{
                                    publication
                                        .logical_artifact_id,
                                    *schema,
                                    *hash,
                                    committed.path.string(),
                                    true};
                        }
                        const StateServiceResult released =
                            session->ReleaseStateArtifact(
                                publication.state_artifact_id);
                        if (!released.ok)
                        {
                            publication.artifact.reset();
                            publication.failure =
                                released.message.empty()
                                ? "Finalized state artifact service ownership could not be released"
                                : released.message;
                        }
                    }
                }
            }
            if (!publication.failure.empty())
            {
                Publish(WorkerRuntimeDiagnosticEvent{
                    WorkerRejectionCode::BackendFailure,
                    publication.failure,
                    publication.item.invocation_id});
                if (active_invocation &&
                    active_invocation->invocation_id ==
                        publication.item.invocation_id &&
                    active_invocation->attempt_id ==
                        publication.item.attempt_id)
                {
                    active_invocation
                        ->artifact_finalization_failure =
                        publication.failure;
                    CloseActiveWorksetAdmissionAfterPublication(
                        "Artifact publication failure closed later workset admission");
                    if (!active_invocation->cancellation
                             .is_cancellation_requested())
                    {
                        (void)RequestActiveWorksetCancellation();
                    }
                }
            }
            const WorkerTerminalId terminal_id =
                publication.terminal_id;
            if (terminal_id)
                TryFinalizePendingTerminal(terminal_id);
        }
        RefreshSnapshot();
        PublishCredits();
    }

    void BuildRuntimeManifest()
    {
        WorkerRuntimeManifest manifest;
        manifest.catalog_status = RuntimeCatalogStatus::Partial;
        manifest.limits = workset_limits;
        if (program_runtime)
        {
            const ProgramRuntimeCatalogSnapshot catalog =
                program_runtime->catalog();
            manifest.catalog_status = catalog.complete_exact
                ? RuntimeCatalogStatus::CompleteExact
                : RuntimeCatalogStatus::Partial;
            manifest.catalog_generation = catalog.generation;
            manifest.runtime_profile_sha256 =
                catalog.runtime_profile_sha256;
            manifest.dependency_manifest_sha256 =
                catalog.dependency_manifest_sha256;
            for (const ProgramRuntimeCatalogModule& module :
                 catalog.modules)
            {
                manifest.modules.push_back(
                    RuntimeModuleManifestEntry{
                        module.identity,
                        module.entrypoints,
                        catalog.dependency_manifest_sha256,
                        module.development_only});
            }
        }
        manifest.catalog_sha256 =
            ComputeRuntimeCatalogHash(
                manifest.modules,
                manifest.catalog_status);
        if (HasCapability(
                capabilities_value,
                WorkerCapability::WorksetDispatch) &&
            !ValidateWorkerRuntimeManifest(manifest).ok)
        {
            capabilities_value = RemoveCapability(
                capabilities_value,
                WorkerCapability::WorksetDispatch);
        }
        std::lock_guard lock(manifest_mutex);
        runtime_manifest_value = std::move(manifest);
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
    WorkerWorksetLimits workset_limits;
    WorkerCompletionLedger completion_ledger{workset_limits};
    std::shared_ptr<ProgramBaselineComponentRegistry>
        baseline_components;
    std::unique_ptr<WorksetStager> workset_stager;
    std::unique_ptr<StateArtifactFinalizer>
        artifact_finalizer;
    std::unique_ptr<WorksetStateCoordinator> workset_state;
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
    std::optional<WorksetPackage> active_workset;
    std::optional<WorksetPackage> staged_workset;
    std::optional<PendingWorksetStaging>
        pending_workset_staging;
    std::unordered_map<std::uint64_t, DrainingWorkset>
        draining_worksets;
    std::unordered_map<std::uint64_t, RetainedTerminal>
        retained_terminals;
    std::unordered_map<std::uint64_t, PendingFinalizedTerminal>
        pending_finalized_terminals;
    std::unordered_map<std::uint64_t, ArtifactPublication>
        artifact_publications;
    std::size_t retained_terminal_bytes = 0;
    bool terminal_publication_deferred = false;
    mutable std::mutex manifest_mutex;
    WorkerRuntimeManifest runtime_manifest_value;
    std::unordered_map<std::uint64_t, PendingExecutionCommand>
        pending_execution_commands;
    std::unordered_map<
        std::uint64_t,
        std::shared_ptr<QueuedCommand>>
        pending_module_commands;
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
    std::unique_ptr<program::IProgramActionHost> action_host,
    std::shared_ptr<ProgramBaselineComponentRegistry>
        baseline_components)
    : impl_(std::make_unique<Impl>(
          std::move(session),
          std::move(program_runtime),
          std::move(event_sink),
          std::move(test_hooks),
          std::move(action_host),
          std::move(baseline_components)))
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

WorkerRuntimeManifest WorkerRuntime::runtime_manifest() const
{
    return impl_->RuntimeManifest();
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

    auto session = std::make_unique<EmulationSession>(
        session_id,
        MakeDolphinWrapperBackend());
    auto runtime =
        program::MakeSupportedSoaUsaProgramRuntime();
    auto action_host =
        std::make_unique<program::SessionProgramActionHost>(
            *session);
    return std::make_unique<WorkerRuntime>(
        std::move(session),
        std::move(runtime),
        std::move(event_sink),
        nullptr,
        std::move(action_host));
}

} // namespace savor::runtime
