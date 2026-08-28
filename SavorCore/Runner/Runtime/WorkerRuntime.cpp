#include "WorkerRuntime.h"
#include "ProgramKind.h"

#include "DolphinWrapperBackend.h"
#include "ProgramRuntime/Actions/SessionProgramActionHost.h"
#include "ProgramRuntime/Codec/ProgramCodecV1.h"
#include "ProgramRuntime/ProgramRuntime.h"
#include "ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "Worksets/ProgramBaseline.h"
#include "Worksets/SavestateArtifactFinalizer.h"
#include "Worksets/WorkerCompletionLedger.h"
#include "Worksets/WorksetStager.h"
#include "Worksets/WorksetWireCodec.h"
#include "Core/Memory/Soa/SoaAddrRegistry.h"
#include "Utils/Hash.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
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
    case ExecutionErrorCode::WorksetEpochMismatch:
        return WorkerRejectionCode::WorksetEpochMismatch;
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

    struct CaptureProgressBuffer
    {
        std::mutex mutex;
        std::deque<savor::capture_format::Event> events;
        bool notification_queued = false;
    };

    enum class MailboxItemKind : std::uint8_t
    {
        Command,
        ProgramEvent,
        ProgramActionRequest,
        ProgramActionResolution,
        ProgramPump,
        CaptureProgress,
        BeginWorksetExecution,
        HostEvent,
        ForceStop,
    };

    struct PendingHostEvent
    {
        HostEventSequence sequence;
        SessionId observed_session_id;
        WorksetEpoch observed_workset_epoch;
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
        std::optional<program::ProgramActionResolution>
            program_action_resolution;
        std::optional<PendingHostEvent> host_event;
        InvocationId capture_invocation_id;
        AttemptId capture_attempt_id;
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
        bool workset_begin_queued = false;
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
        : public ISavestateArtifactFinalizerNotifier
    {
    public:
        explicit ArtifactFinalizerNotifier(
            std::weak_ptr<Mailbox> mailbox)
            : mailbox_(std::move(mailbox))
        {
        }

        void NotifySavestateArtifactFinalizerCompletion() noexcept override
        {
            if (const auto mailbox = mailbox_.lock())
                SignalMailbox(*mailbox);
        }

    private:
        std::weak_ptr<Mailbox> mailbox_;
    };

    struct ActiveInvocation
    {
        enum class OutputTransactionState : std::uint8_t
        {
            Open,
            SealedForCommit,
            Finalizing,
            Committed,
            SealedForAbandon,
            Abandoned,
        };

        enum class OutputState : std::uint8_t
        {
            Adopted,
            Finalizing,
            Published,
            Abandoned,
        };

        struct Output
        {
            program::ProgramActionRequestId action_request_id;
            program::StagedSavestateOutput savestate;
            OutputState state = OutputState::Adopted;
            SavestateArtifactFinalizationId finalization_id;
            std::optional<program::ArtifactReferenceValue> artifact;
            std::string failure;
        };

        struct OutputTransaction
        {
            OutputTransactionState state = OutputTransactionState::Open;
            std::vector<Output> outputs;
            std::uint64_t maximum_artifacts = 0;
            std::size_t resident_bytes = 0;
        };

        InvocationId invocation_id;
        AttemptId attempt_id;
        WorksetEpoch workset_epoch;
        CancellationSource cancellation;
        std::optional<WorkerWorksetId> workset_id;
        std::optional<WorkerWorksetItemId> workset_item_id;
        std::uint32_t workset_item_ordinal = 0;
        std::vector<SavestateArtifactFinalizationId>
            artifact_finalizations;
        bool artifact_publication_promoted = false;
        std::string artifact_finalization_failure;
        OutputTransaction outputs;
        std::optional<ProgramExecutionFinished> execution_finished;
        std::optional<ProgramInvocationTerminalEvent> terminal_draft;
        std::optional<CaptureAttachmentId> capture_attachment;
        std::optional<std::filesystem::path> capture_path;
        std::optional<program::ArtifactReferenceValue> capture_artifact;
        std::vector<std::string> capture_diagnostics;
        std::shared_ptr<CaptureProgressBuffer> capture_progress;
        std::uint64_t next_progress_ordinal = 1;

        ActiveInvocation(
            InvocationId invocation,
            AttemptId attempt,
            WorksetEpoch epoch,
            std::uint64_t maximum_artifacts)
            : invocation_id(invocation),
              attempt_id(attempt),
              workset_epoch(epoch),
              cancellation(invocation)
        {
            outputs.maximum_artifacts = maximum_artifacts;
        }
    };

    static void EnqueueCaptureProgress(
        const std::weak_ptr<Mailbox>& weak_mailbox,
        const std::shared_ptr<CaptureProgressBuffer>& buffer,
        InvocationId invocation_id,
        AttemptId attempt_id,
        savor::capture_format::Event event) noexcept
    {
        bool notify = false;
        try
        {
            std::lock_guard lock(buffer->mutex);
            buffer->events.push_back(std::move(event));
            if (!buffer->notification_queued)
            {
                buffer->notification_queued = true;
                notify = true;
            }
        }
        catch (...)
        {
            return;
        }
        if (!notify)
            return;
        const std::shared_ptr<Mailbox> mailbox = weak_mailbox.lock();
        if (!mailbox)
            return;
        {
            std::lock_guard lock(mailbox->mutex);
            if (!mailbox->accept_program_events)
                return;
            MailboxItem item;
            item.kind = MailboxItemKind::CaptureProgress;
            item.capture_invocation_id = invocation_id;
            item.capture_attempt_id = attempt_id;
            mailbox->items.push_back(std::move(item));
        }
        SignalMailbox(*mailbox);
    }

    struct WorksetPackage
    {
        WorkerWorksetDefinition definition;
        std::optional<HostStagedCaptureProfile> capture;
        std::vector<PreparedInvocationTemplateReceipt> prepared;
        std::vector<bool> cancelled;
        std::vector<bool> initially_suppressed;
        std::vector<bool> terminalized;
        std::uint32_t next_item = 0;
        WorkerWorksetState state = WorkerWorksetState::Validating;
        PreparedProgramBaselineReceipt baseline;
        std::uint32_t terminal_count = 0;
        std::uint32_t unstarted_count = 0;
        std::uint32_t initially_suppressed_count = 0;
        std::string definition_sha256;
        std::string cancellation_sidecar_sha256;
        bool admission_closed = false;
        std::string admission_close_reason;
    };

    struct DrainingWorkset
    {
        std::uint32_t item_count = 0;
        std::uint32_t terminal_count = 0;
        std::uint32_t unstarted_count = 0;
        std::uint32_t initially_suppressed_count = 0;
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
        InitialWorksetCancellationSidecarV1 initial_cancellations;
        std::string definition_sha256;
        std::string cancellation_sidecar_sha256;
        bool cancelled = false;
    };

    struct AcceptedWorksetSubmission
    {
        std::string definition_sha256;
        SubmitWorksetResultV1 receipt;
    };

    struct ArtifactPublication
    {
        WorkerItemExecutionCorrelation item;
        SavestateArtifactId state_artifact_id;
        std::string logical_artifact_id;
        bool completed = false;
        bool abandon_requested = false;
        std::optional<program::ArtifactReferenceValue> artifact;
        std::string failure;
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
        const bool using_injected_baseline_components =
            static_cast<bool>(injected_baseline_components);
        baseline_components = injected_baseline_components
            ? std::move(injected_baseline_components)
            : std::make_shared<ProgramBaselineComponentRegistry>();
        if (!using_injected_baseline_components)
        {
            (void)baseline_components->Register(
                std::make_shared<
                    TasMovieCheckpointSterilizationBaselineComponentProvider>());
        }
        baseline_components->Freeze();
        workset_stager = std::make_unique<WorksetStager>(
            workset_limits,
            baseline_components,
            std::make_shared<WorksetStagerNotifier>(mailbox));
        artifact_finalizer =
            std::make_unique<SavestateArtifactFinalizer>(
                workset_limits,
                std::make_shared<ArtifactFinalizerNotifier>(
                    mailbox),
                this->test_hooks
                    ? this->test_hooks->before_output_finalization
                    : std::function<void()>{});
        if (this->session)
        {
            workset_state = std::make_unique<WorksetStateCoordinator>(
                *this->session,
                workset_limits,
                baseline_components);
        }
        runtime_contract_value = BuildProductionWorkerRuntimeContractV1();
        if (this->program_runtime)
        {
            this->program_runtime->BindActionSink(
                program_action_ingress);
        }

        current_snapshot.state = WorkerState::Starting;
        current_snapshot.runtime_contract_sha256 =
            runtime_contract_value.canonical_sha256;
        current_snapshot.available_item_credits =
            workset_limits.maximum_item_credits;
        if (this->session)
        {
            (void)this->session->ConfigureStopPointIngressNotification(
                &mailbox->ingress_generation,
                mailbox.get(),
                &NotifyStopPointIngress);
            current_snapshot.session = this->session->snapshot();
            current_snapshot.execution.reset();
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

    WorkerRuntimeContractV1 RuntimeContract() const
    {
        return runtime_contract_value;
    }

    bool EnqueueHostEvent(
        std::string name,
        std::vector<std::uint8_t> encoded_payload)
    {
        const WorkerSnapshot observed = Snapshot();
        return EnqueueHostEvent(
            observed.session.session_id,
            observed.session.workset_epoch,
            std::move(name),
            std::move(encoded_payload));
    }

    bool EnqueueHostEvent(
        SessionId observed_session_id,
        WorksetEpoch observed_workset_epoch,
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
                observed_workset_epoch,
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
                    if (item.kind ==
                        MailboxItemKind::BeginWorksetExecution)
                    {
                        mailbox->workset_begin_queued = false;
                    }
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
                     MailboxItemKind::ProgramActionResolution)
            {
                if (item.program_action_resolution)
                {
                    HandleProgramActionResolution(
                        std::move(*item.program_action_resolution));
                }
            }
            else if (item.kind == MailboxItemKind::ProgramPump)
            {
                PumpProgramRuntime();
            }
            else if (item.kind == MailboxItemKind::CaptureProgress)
            {
                DrainActiveCaptureProgress(
                    item.capture_invocation_id,
                    item.capture_attempt_id);
            }
            else if (item.kind == MailboxItemKind::BeginWorksetExecution)
            {
                BeginReadyWorksetExecution();
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
        if (command.options.worker_mode == WorkerMode::VisualDebug)
        {
            const auto execution_capabilities =
                session->execution_capabilities();
            if (!HasExecutionCapability(
                    execution_capabilities,
                    BackendExecutionCapability::Pause) ||
                !HasExecutionCapability(
                    execution_capabilities,
                    BackendExecutionCapability::Resume) ||
                !HasExecutionCapability(
                    execution_capabilities,
                    BackendExecutionCapability::FrameStep))
            {
                (void)session->Shutdown();
                RefreshSnapshot();
                Reject(
                    queued,
                    WorkerRejectionCode::BackendFailure,
                    "VisualDebug mode requires pause, resume, and frame-step backend services");
                return;
            }
        }
        if (test_hooks && test_hooks->session_opened)
            test_hooks->session_opened(*session);
        RefreshSnapshot();

        ChangeState(WorkerState::Ready);
        worker_mode = command.options.worker_mode;
        {
            std::lock_guard lock(snapshot_mutex);
            current_snapshot.mode = worker_mode;
        }
        Complete(
            queued,
            WorkerCommandOutcome::Completed,
            {},
            {},
            std::move(receipt));
    }

    [[nodiscard]] static bool BaselinePolicyMatches(
        ProgramBaselineArtifactKind baseline,
        program::InvocationStatePolicy policy) noexcept
    {
        switch (baseline)
        {
        case ProgramBaselineArtifactKind::Savestate:
            return policy ==
                program::InvocationStatePolicy::RestoreBaseline;
        case ProgramBaselineArtifactKind::ReadOnlyMovie:
            return policy ==
                program::InvocationStatePolicy::EstablishBaseline;
        }
        return false;
    }

    [[nodiscard]] bool SessionIsCleanIdle() const
    {
        if (!session)
            return false;
        const SessionSnapshot current = session->snapshot();
        const bool clean =
            current.disposition == SessionDisposition::Clean ||
            current.disposition ==
                SessionDisposition::CleanWithDiagnostics;
        if (!current.open || !clean ||
            current.core_state != BackendCoreState::Paused)
        {
            return false;
        }

        // Between worksets the guest-dependent runtime is intentionally absent.
        // The paused infrastructure session is the clean idle boundary from
        // which the next workset constructs its own services and epoch.
        if (!current.workset_epoch)
            return true;

        const std::optional<ExecutionSnapshot> execution =
            session->execution_snapshot();
        return
            execution &&
            execution->activity == ExecutionActivity::IdlePaused &&
            !execution->active_operation &&
            execution->interruption_depth == 0;
    }

    [[nodiscard]] std::optional<ExecutionSnapshot>
    ProjectExecutionSnapshot() const
    {
        // The committed engine is internally usable at Ready, but ordinary
        // worker telemetry becomes authoritative only once the workset has
        // crossed into Running. Initialization and item reset deliberately
        // project no execution state.
        if (!session || !active_workset ||
            active_workset->state != WorkerWorksetState::Running)
        {
            return std::nullopt;
        }
        return session->execution_snapshot();
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
            if (ordinal < package.initially_suppressed.size() &&
                package.initially_suppressed[ordinal])
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
                    if (std::ranges::find(
                            submit->initial_cancellations.item_ids,
                            item.item_id) !=
                        submit->initial_cancellations.item_ids.end())
                    {
                        continue;
                    }
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
                    item.execution.execution_id.value());
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
                        item.execution.execution_id.value());
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
                        item.execution.execution_id.value());
            });
    }

    [[nodiscard]] std::optional<WorksetPackage> ValidateAndStageWorkset(
        HostStagedWorksetPackage source,
        const InitialWorksetCancellationSidecarV1& initial_cancellations,
        std::string definition_sha256,
        RuntimeError& error)
    {
        WorksetPackage package;
        package.definition = std::move(source.definition);
        package.capture = std::move(source.capture);
        const WorksetValidationResult validated =
            ValidateWorkerWorksetDefinition(
                package.definition,
                workset_limits);
        if (!validated.ok)
        {
            error = validated.error;
            return std::nullopt;
        }
        const WorksetValidationResult sidecar_validated =
            ValidateInitialWorksetCancellationSidecar(
                package.definition, initial_cancellations);
        if (!sidecar_validated.ok)
        {
            error = sidecar_validated.error;
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
        package.cancelled.resize(
            package.definition.items.size(),
            false);
        package.initially_suppressed.resize(
            package.definition.items.size(),
            false);
        package.terminalized.resize(
            package.definition.items.size(),
            false);
        package.prepared.resize(package.definition.items.size());
        package.definition_sha256 = std::move(definition_sha256);
        package.cancellation_sidecar_sha256 =
            ComputeInitialWorksetCancellationSidecarSha256(
                initial_cancellations);
        for (std::size_t ordinal = 0;
             ordinal < package.definition.items.size();
             ++ordinal)
        {
            if (std::ranges::find(
                    initial_cancellations.item_ids,
                    package.definition.items[ordinal].item_id) !=
                initial_cancellations.item_ids.end())
            {
                package.cancelled[ordinal] = true;
                package.initially_suppressed[ordinal] = true;
                ++package.initially_suppressed_count;
            }
        }
        const auto& prepared_program =
            package.definition.phase_invocation.program_package;
        const auto& runtime_contract =
            prepared_program.runtime_contract;
        const auto* phase = fullphase::ProductionRegistry().Find(
            prepared_program.identity.program_kind);
        if (phase == nullptr)
        {
            error = {
                WorkerRejectionCode::ProgramPackageRejected,
                "WorkerWorkset Full Phase definition is unavailable"};
            return std::nullopt;
        }
        ModuleClosureAdmissionReceipt module_receipt;
        const ProgramRuntimeSubmission admitted =
            program_runtime->AdmitModuleClosure(
                {
                    .root = runtime_contract.module,
                    .expected_dependency_lock_sha256 =
                        runtime_contract.dependency_lock_sha256,
                    .modules = prepared_program.module_closure,
                },
                module_receipt);
        if (!admitted.accepted || !module_receipt ||
            module_receipt.root != runtime_contract.module ||
            module_receipt.dependency_lock_sha256 !=
                runtime_contract.dependency_lock_sha256 ||
            module_receipt.admitted_module_count !=
                prepared_program.module_closure.size())
        {
            error = admitted.accepted
                ? RuntimeError{
                      WorkerRejectionCode::ProgramPackageRejected,
                      "Workset Full Phase module closure admission receipt is invalid"}
                : admitted.error;
            return std::nullopt;
        }
        for (std::size_t ordinal = 0;
             ordinal < package.definition.items.size();
             ++ordinal)
        {
            if (package.initially_suppressed[ordinal])
                continue;
            const WorksetItemTemplate& item =
                package.definition.items[ordinal];
            std::string build_diagnostic;
            const auto execution = phase->BuildResolvedExecution(
                prepared_program,
                package.definition.phase_invocation.common_input.payload,
                item.execution.input_payload,
                item.execution.execution_id,
                item.execution.attempt_id,
                &build_diagnostic);
            if (!execution)
            {
                error = {
                    WorkerRejectionCode::InvalidArgument,
                    build_diagnostic.empty()
                        ? "Full Phase scalar input could not be resolved"
                        : std::move(build_diagnostic)};
                return std::nullopt;
            }
            const auto encoded =
                program::EncodeProgramInvocationV1(*execution);
            if (!encoded)
            {
                error = {
                    WorkerRejectionCode::InvalidArgument,
                    "Resolved Full Phase execution could not be encoded: " +
                        encoded.status.message};
                return std::nullopt;
            }
            EncodedInvocationEnvelope envelope;
            envelope.invocation_id =
                item.execution.execution_id;
            envelope.attempt_id = item.execution.attempt_id;
            envelope.module = runtime_contract.module;
            envelope.entrypoint = runtime_contract.entrypoint;
            envelope.expected_workset_epoch = {};
            envelope.input_payload = encoded.bytes;

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
                    item.execution.execution_id ||
                receipt.attempt_id !=
                    item.execution.attempt_id ||
                receipt.module != runtime_contract.module ||
                receipt.entrypoint != runtime_contract.entrypoint ||
                receipt.program_compatibility_sha256 !=
                    package.definition.execution_key
                        .verified_dependency_sha256 ||
                !BaselinePolicyMatches(
                    package.definition.baseline.artifact.kind,
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
                    if (staged.template_id)
                    {
                        (void)program_runtime
                            ->ReleaseInvocationTemplate(
                                staged.template_id);
                    }
                }
                error = prepared.accepted
                    ? RuntimeError{
                          WorkerRejectionCode::
                              ProgramPackageRejected,
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
            package.prepared[ordinal] = std::move(receipt);
        }
        package.state = WorkerWorksetState::Admitted;
        return package;
    }

    void HandleSubmitWorkset(
        const std::shared_ptr<QueuedCommand>& queued,
        const SubmitWorksetCommand& command)
    {
        const auto sidecar_validation =
            ValidateInitialWorksetCancellationSidecar(
                command.definition, command.initial_cancellations);
        const auto sidecar_sha256 =
            ComputeInitialWorksetCancellationSidecarSha256(
                command.initial_cancellations);
        std::vector<std::uint8_t> encoded_definition;
        const auto encoded = EncodeWorkerWorksetV5(
            command.definition,
            encoded_definition);
        const auto definition_sha256 = encoded.ok
            ? ::hash::sha256(
                encoded_definition.data(),
                encoded_definition.size())
            : std::string{};
        if (!sidecar_validation.ok || !encoded.ok
            || (!command.definition_sha256.empty()
                && command.definition_sha256 != definition_sha256))
        {
            Reject(
                queued,
                WorkerRejectionCode::InvalidArgument,
                !sidecar_validation.ok
                    ? sidecar_validation.error.message
                    : !encoded.ok
                        ? encoded.message
                        : "WorkerWorkset definition digest is invalid");
            return;
        }
        const auto accepted = accepted_workset_submissions.find(
            command.definition.workset_id.value());
        if (accepted != accepted_workset_submissions.end())
        {
            if (accepted->second.definition_sha256
                    != definition_sha256 ||
                accepted->second.receipt.applied_sidecar_sha256
                    != sidecar_sha256)
            {
                Reject(
                    queued,
                    WorkerRejectionCode::InvalidArgument,
                    "WorkerWorkset identity was repeated with a different definition or cancellation sidecar");
                return;
            }
            auto receipt = accepted->second.receipt;
            receipt.disposition =
                WorksetSubmissionDispositionV1::AlreadyAdmitted;
            Complete(
                queued,
                WorkerCommandOutcome::Accepted,
                {}, {}, {}, {}, {}, receipt);
            return;
        }
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
        if (!program_runtime || !program_action_host)
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
        if (!active_workset && !active_invocation &&
            !SessionIsCleanIdle())
        {
            Reject(
                queued,
                WorkerRejectionCode::InvalidState,
                "WorkerWorkset activation requires a clean, idle, paused session");
            return;
        }
        const std::size_t live_item_count =
            command.definition.items.size()
            - command.initial_cancellations.item_ids.size();
        const std::size_t resident =
            ResidentItemCount() + live_item_count;
        std::size_t candidate_terminal_bytes = 0;
        bool terminal_bytes_overflow = false;
        for (const WorksetItemTemplate& item :
             command.definition.items)
        {
            if (std::ranges::find(
                    command.initial_cancellations.item_ids,
                    item.item_id) !=
                command.initial_cancellations.item_ids.end())
            {
                continue;
            }
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
            live_item_count >
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
            command.initial_cancellations,
            definition_sha256,
            sidecar_sha256,
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
             workset_stager->DrainResults())
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
            RuntimeError error;
            std::optional<WorksetPackage> staged =
                ValidateAndStageWorkset(
                    std::move(*completion.package),
                    pending.initial_cancellations,
                    pending.definition_sha256,
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
                    if (receipt.template_id)
                    {
                        (void)program_runtime
                            ->ReleaseInvocationTemplate(
                                receipt.template_id);
                    }
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
            SubmitWorksetResultV1 submission_receipt{
                .workset_id = accepted_id,
                .applied_item_count = static_cast<std::uint32_t>(
                    pending.initial_cancellations.item_ids.size()),
                .applied_sidecar_sha256 =
                    pending.cancellation_sidecar_sha256,
                .disposition =
                    WorksetSubmissionDispositionV1::Admitted,
            };
            accepted_workset_submissions.insert_or_assign(
                accepted_id.value(),
                AcceptedWorksetSubmission{
                    .definition_sha256 = pending.definition_sha256,
                    .receipt = submission_receipt,
                });
            RefreshSnapshot();
            Complete(
                pending.command,
                WorkerCommandOutcome::Accepted,
                {}, {}, {}, {}, {}, submission_receipt);
            PublishWorksetState(
                accepted_id,
                WorkerWorksetState::Admitted);
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
            WorkerWorksetState::Initializing;
        ChangeState(WorkerState::InitializingWorkset);
        PublishWorksetState(
            active_workset->definition.workset_id,
            active_workset->state);
        ProgramBaselineComponentResult prepared =
            workset_state->Initialize(
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
        active_workset->state = WorkerWorksetState::Ready;
        PublishWorksetState(
            active_workset->definition.workset_id,
            active_workset->state);
        ChangeState(WorkerState::Ready);
        QueueWorksetBegin();
    }

    void BeginReadyWorksetExecution()
    {
        if (!active_workset || active_invocation ||
            active_workset->state != WorkerWorksetState::Ready)
        {
            return;
        }
        StartNextWorksetItem();
    }

    void DrainActiveCaptureProgress(
        InvocationId invocation_id,
        AttemptId attempt_id)
    {
        if (!active_invocation || !active_workset ||
            active_invocation->invocation_id != invocation_id ||
            active_invocation->attempt_id != attempt_id ||
            !active_invocation->capture_progress)
        {
            return;
        }
        std::deque<savor::capture_format::Event> events;
        {
            std::lock_guard lock(
                active_invocation->capture_progress->mutex);
            events.swap(active_invocation->capture_progress->events);
            active_invocation->capture_progress
                ->notification_queued = false;
        }
        if (!active_workset->capture)
            return;
        const auto& profile = active_workset->capture->profile;
        const auto& plan = active_workset->definition.progress_plan;
        const WorksetItemTemplate& item =
            active_workset->definition.items[
                active_invocation->workset_item_ordinal];
        for (const savor::capture_format::Event& observed : events)
        {
            const auto probe = std::ranges::find_if(
                profile.probes,
                [&](const savor::probe::ProbeDefinition& candidate) {
                    return candidate.id == observed.probe_id;
                });
            if (probe == profile.probes.end())
                continue;
            const auto binding = std::ranges::find_if(
                plan.points,
                [&](const progress::ProgressPointBindingV1& point) {
                    return point.formatter.canonical_id ==
                        probe->progress_formatter;
                });
            if (binding == plan.points.end())
            {
                Publish(WorkerRuntimeDiagnosticEvent{
                    WorkerRejectionCode::InvalidArgument,
                    "Capture progress event did not resolve to the admitted progress plan",
                    invocation_id});
                continue;
            }
            const progress::ProgressPointDescriptor* descriptor =
                progress::ProductionProgressRegistry().FindPoint(
                    binding->library_id,
                    binding->library_revision,
                    binding->point_id);
            if (descriptor == nullptr)
                continue;
            progress::CanonicalProgressEventV1 event;
            event.workset_id = active_workset->definition.workset_id;
            event.item_id = item.item_id;
            event.durable_job_id = item.correlation.durable_job_id;
            event.invocation_id = invocation_id;
            event.attempt_id = attempt_id;
            event.ordinal =
                active_invocation->next_progress_ordinal++;
            event.library_id = binding->library_id;
            event.library_revision = binding->library_revision;
            event.progress_point_id = binding->point_id;
            if (observed.capture_sequence != 0)
            {
                event.routed_sequence =
                    RoutedStopSequence(observed.capture_sequence);
            }
            if (observed.snapshot_id != 0)
            {
                event.sample_snapshot_id =
                    StopSampleSnapshotId(observed.snapshot_id);
            }
            if (observed.guest_workset_epoch != 0)
            {
                event.trigger_epoch =
                    WorksetEpoch(observed.guest_workset_epoch);
            }
            event.schema = binding->schema;
            event.typed_payload =
                progress::EncodeCaptureEventPayloadV1(observed);
            event.display_text =
                progress::FormatCaptureProgressText(
                    *descriptor,
                    observed);
            Publish(std::move(event));
        }
    }

    void PublishRuntimeSampleProgress(
        const program::ForegroundSemanticStopObservationV1& observation)
    {
        if (!active_invocation || !active_workset ||
            observation.invocation_id !=
                active_invocation->invocation_id ||
            observation.attempt_id != active_invocation->attempt_id)
        {
            return;
        }
        const std::uint32_t trigger_pc =
            observation.routed_event.evidence.hit_pc;
        const WorksetItemTemplate& item =
            active_workset->definition.items[
                active_invocation->workset_item_ordinal];
        for (const progress::ProgressPointBindingV1& point :
             active_workset->definition.progress_plan.points)
        {
            if (point.provider !=
                    progress::ProgressProviderKind::RuntimeSample ||
                !std::ranges::contains(
                    point.runtime_sample_trigger_pcs,
                    trigger_pc))
            {
                continue;
            }

            progress::CanonicalProgressEventV1 event;
            event.workset_id = active_workset->definition.workset_id;
            event.item_id = item.item_id;
            event.durable_job_id = item.correlation.durable_job_id;
            event.invocation_id = observation.invocation_id;
            event.attempt_id = observation.attempt_id;
            event.ordinal = active_invocation->next_progress_ordinal++;
            event.library_id = point.library_id;
            event.library_revision = point.library_revision;
            event.progress_point_id = point.point_id;
            event.routed_sequence =
                observation.routed_event.identity.sequence;
            event.sample_snapshot_id =
                observation.routed_event.identity.sample_snapshot;
            event.trigger_epoch =
                observation.routed_event.identity.workset_epoch;
            event.schema = point.schema;

            if (point.library_id == "soa.progress.runtime.vi/1" &&
                point.point_id == "vi.current")
            {
                event.typed_payload =
                    progress::EncodeViProgressPayloadV1(
                        observation.execution_evidence.vi_count);
                event.display_text = "VI " + std::to_string(
                    observation.execution_evidence.vi_count);
            }
            else if (
                point.library_id ==
                    "soa.progress.soa.script_location/1" &&
                point.point_id == "script_location.current")
            {
                GuestMemory* memory = session->guest_memory();
                const WorksetEpoch epoch =
                    observation.routed_event.identity.workset_epoch;
                const GuestReadReceipt file_number = memory
                    ? memory->ReadScalar(
                          addr::AddrRegistry::base(
                              addr::core::SCT_FILE_NUM),
                          GuestScalarWidth::U32,
                          epoch)
                    : GuestReadReceipt{};
                const GuestReadReceipt file_letter = memory
                    ? memory->ReadScalar(
                          addr::AddrRegistry::base(
                              addr::core::SCT_FILE_LTTR),
                          GuestScalarWidth::U8,
                          epoch)
                    : GuestReadReceipt{};
                const GuestReadReceipt first_instruction = memory
                    ? memory->ReadScalar(
                          addr::AddrRegistry::base(
                              addr::core::SCT_FIRST_INST),
                          GuestScalarWidth::U32,
                          epoch)
                    : GuestReadReceipt{};
                const GuestReadReceipt current_instruction = memory
                    ? memory->ReadScalar(
                          addr::AddrRegistry::base(
                              addr::core::SCT_CURRENT_INST),
                          GuestScalarWidth::U32,
                          epoch)
                    : GuestReadReceipt{};
                if (!file_number.ok || !file_letter.ok ||
                    !first_instruction.ok || !current_instruction.ok)
                {
                    Publish(WorkerRuntimeDiagnosticEvent{
                        WorkerRejectionCode::BackendFailure,
                        "Script-location progress could not read its registered guest fields",
                        observation.invocation_id});
                    continue;
                }
                std::string file = "SCT_FILE:";
                const std::string numeric =
                    std::to_string(file_number.value);
                if (numeric.size() < 3)
                    file.append(3 - numeric.size(), '0');
                file += numeric;
                const char letter = static_cast<char>(file_letter.value);
                file.push_back(
                    letter >= 0x20 && letter <= 0x7e
                    ? letter
                    : '?');
                std::ostringstream section;
                section << "instruction+0x" << std::hex;
                if (current_instruction.value >=
                    first_instruction.value)
                {
                    section << (current_instruction.value -
                        first_instruction.value);
                }
                else
                {
                    section << current_instruction.value;
                }
                const std::string section_text = section.str();
                event.typed_payload =
                    progress::EncodeScriptLocationProgressPayloadV1(
                        file,
                        section_text,
                        trigger_pc);
                event.display_text = file + ":" + section_text +
                    " at 0x";
                std::ostringstream pc_text;
                pc_text << std::hex << trigger_pc;
                event.display_text += pc_text.str();
            }
            else
            {
                Publish(WorkerRuntimeDiagnosticEvent{
                    WorkerRejectionCode::InvalidArgument,
                    "Admitted progress plan names an unavailable runtime-sample provider",
                    observation.invocation_id});
                continue;
            }
            Publish(std::move(event));
        }
    }

    [[nodiscard]] RuntimeError AttachActiveWorksetCapture(
        const SessionSnapshot& current)
    {
        if (!active_invocation || !active_workset ||
            !active_workset->capture)
        {
            return {};
        }
        CaptureService* capture = session->capture_service();
        if (capture == nullptr)
        {
            return {
                WorkerRejectionCode::Unsupported,
                "Workset observation binding requires CaptureService"};
        }
        auto progress_buffer =
            std::make_shared<CaptureProgressBuffer>();
        savor::probe::SessionOptions options;
        options.metadata.session_id =
            std::to_string(current.session_id.value());
        options.metadata.source_identity =
            "worker-workset:" +
            std::to_string(
                active_workset->definition.workset_id.value());
        options.metadata.executable_sha256 =
            active_workset->capture->profile
                .expected_module_sha256;
        if (active_workset->capture->output_directory)
        {
            try
            {
                std::filesystem::create_directories(
                    *active_workset->capture->output_directory);
                const WorksetItemTemplate& item =
                    active_workset->definition.items[
                        active_invocation->workset_item_ordinal];
                active_invocation->capture_path =
                    *active_workset->capture->output_directory /
                    ("workset-" + std::to_string(
                         active_workset->definition.workset_id.value()) +
                     "-item-" + std::to_string(item.item_id.value()) +
                     "-attempt-" + std::to_string(
                         active_invocation->attempt_id.value()) +
                     ".scap");
                options.capture_path =
                    *active_invocation->capture_path;
            }
            catch (const std::exception& exception)
            {
                return {
                    WorkerRejectionCode::BackendFailure,
                    std::string(
                        "Capture output path could not be prepared: ") +
                        exception.what()};
            }
        }
        const InvocationId invocation_id =
            active_invocation->invocation_id;
        const AttemptId attempt_id = active_invocation->attempt_id;
        options.progress_callback =
            [weak_mailbox = std::weak_ptr<Mailbox>(mailbox),
             progress_buffer,
             invocation_id,
             attempt_id](const savor::capture_format::Event& event,
                         bool) {
                EnqueueCaptureProgress(
                    weak_mailbox,
                    progress_buffer,
                    invocation_id,
                    attempt_id,
                    event);
            };
        CaptureAttachmentRequest request;
        request.profile_json =
            active_workset->capture->profile_json;
        request.profile = active_workset->capture->profile;
        request.options = std::move(options);
        request.expected_epoch = current.workset_epoch;
        const CaptureServiceReceipt attached =
            capture->Attach(std::move(request));
        if (!attached.ok)
        {
            return {
                attached.requires_session_taint
                    ? WorkerRejectionCode::SessionTainted
                    : WorkerRejectionCode::BackendFailure,
                attached.error.message.empty()
                    ? "Workset capture session could not be attached"
                    : attached.error.message};
        }
        active_invocation->capture_attachment =
            attached.attachment;
        active_invocation->capture_progress =
            std::move(progress_buffer);
        return {};
    }

    [[nodiscard]] RuntimeError FinalizeActiveWorksetCapture(
        ProgramInvocationTerminalEvent& terminal,
        bool& requires_taint)
    {
        requires_taint = false;
        if (!active_invocation ||
            !active_invocation->capture_attachment)
        {
            return {};
        }
        CaptureService* capture = session->capture_service();
        if (capture == nullptr)
        {
            requires_taint = true;
            return {
                WorkerRejectionCode::SessionTainted,
                "Active workset capture lost CaptureService during unwind"};
        }
        const CaptureAttachmentId attachment =
            *active_invocation->capture_attachment;
        CaptureServiceReceipt finalized =
            capture->Detach(attachment);
        active_invocation->capture_attachment.reset();
        DrainActiveCaptureProgress(
            active_invocation->invocation_id,
            active_invocation->attempt_id);
        if (!finalized.ok)
        {
            requires_taint = finalized.requires_session_taint;
            return {
                requires_taint
                    ? WorkerRejectionCode::SessionTainted
                    : WorkerRejectionCode::BackendFailure,
                finalized.error.message.empty()
                    ? "Workset capture finalization failed"
                    : finalized.error.message};
        }
        if (!finalized.capture_complete ||
            finalized.capture_drop_count != 0 ||
            finalized.progress_drop_count != 0)
        {
            std::string diagnostic =
                finalized.incomplete_reason.empty()
                ? "Workset capture completed with incomplete diagnostic evidence"
                : finalized.incomplete_reason;
            diagnostic += ";capture_drops=" +
                std::to_string(finalized.capture_drop_count) +
                ";progress_drops=" +
                std::to_string(finalized.progress_drop_count);
            terminal.diagnostics.push_back(std::move(diagnostic));
            if (terminal.cleanup == CleanupStatus::Clean)
                terminal.cleanup = CleanupStatus::CleanWithDiagnostics;
            if (terminal.session_disposition == SessionDisposition::Clean)
            {
                terminal.session_disposition =
                    SessionDisposition::CleanWithDiagnostics;
            }
        }
        if (!active_invocation->capture_path)
            return {};
        try
        {
            if (!std::filesystem::is_regular_file(
                    *active_invocation->capture_path))
            {
                return {
                    WorkerRejectionCode::BackendFailure,
                    "Workset capture finalization did not publish its artifact"};
            }
            const std::string digest = hash::sha256_of_file(
                active_invocation->capture_path->string());
            const auto content_hash =
                program::ContentHash256::FromHex(digest);
            constexpr std::string_view schema_contract =
                "savor.capture.profile/1 artifact bytes";
            const auto schema_hash =
                program::ContentHash256::FromHex(
                    hash::sha256(
                        schema_contract.data(),
                        schema_contract.size()));
            if (!content_hash || !schema_hash)
            {
                return {
                    WorkerRejectionCode::InternalFailure,
                    "Workset capture artifact identity could not be constructed"};
            }
            terminal.workset_artifacts.push_back(
                program::ArtifactReferenceValue{
                    "capture:workset-" +
                        std::to_string(
                            active_workset->definition.workset_id.value()) +
                        ":item-" +
                        std::to_string(
                            active_invocation->workset_item_id->value()),
                    program::SchemaIdentity{
                        .canonical_id =
                            "savor.capture.profile.artifact",
                        .version = 1,
                        .schema_hash = *schema_hash,
                    },
                    *content_hash,
                    active_invocation->capture_path->string(),
                    finalized.capture_complete &&
                        finalized.artifacts_finalized});
        }
        catch (const std::exception& exception)
        {
            return {
                WorkerRejectionCode::BackendFailure,
                std::string(
                    "Workset capture artifact could not be finalized: ") +
                    exception.what()};
        }
        return {};
    }

    void StartNextWorksetItem()
    {
        if (!active_workset || active_invocation)
            return;
        if (retained_terminals.size() >=
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
                if (!active_workset->initially_suppressed[ordinal])
                {
                    RetainUnstartedTerminal(
                        *active_workset,
                        ordinal,
                        active_workset->admission_close_reason.empty()
                            ? "Workset item was cancelled before admission"
                            : active_workset
                                  ->admission_close_reason);
                }
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
            const auto* session_action_host =
                dynamic_cast<const program::SessionProgramActionHost*>(
                    program_action_host.get());
            const program::SessionProgramActionHostSnapshot action =
                session_action_host
                ? session_action_host->snapshot()
                : program::SessionProgramActionHostSnapshot{};
            const std::optional<ExecutionSnapshot> execution =
                session->execution_snapshot();
            SessionResourceLedger* resources = session->resources();
            program::SessionResourceBindingTable* bindings =
                session->resource_bindings();
            CaptureService* capture = session->capture_service();
            const auto* derived_state = session->derived_state();
            StopPointRouter* stops = session->stop_points();
            if ((session_action_host &&
                 (action.invocation_active ||
                  action.mapped_scope_count != 0 ||
                  action.mapped_resource_count != 0 ||
                  action.execution_pending ||
                  action.queued_completion_count != 0)) ||
                !execution ||
                execution->activity != ExecutionActivity::IdlePaused ||
                execution->active_operation.has_value() ||
                execution->interruption_depth != 0 ||
                !resources ||
                resources->snapshot().state !=
                    ResourceLedgerState::Accepting ||
                resources->snapshot().active_resource_count != 0 ||
                resources->snapshot().cleanup_execution_pending ||
                !bindings || bindings->size() != 0 ||
                (capture && capture->snapshot().attached) ||
                (derived_state && derived_state->active()) ||
                !stops || !stops->ingress_enabled())
            {
                const std::string diagnostic =
                    "The previous workset item did not reach a clean restoration boundary";
                FailRemainingWorksetItems(
                    WorkerRejectionCode::SessionTainted,
                    diagnostic);
                session->MarkTainted(diagnostic);
                FinishCurrentWorkset(WorkerWorksetState::Failed);
                EnterTainted(diagnostic);
                return;
            }
            (void)session->DrainStopPointEvents();
            if (!session->DrainStopPointEvents().empty())
            {
                const std::string diagnostic =
                    "Stop-point ingress did not drain to a stable workset item boundary";
                FailRemainingWorksetItems(
                    WorkerRejectionCode::SessionTainted,
                    diagnostic);
                session->MarkTainted(diagnostic);
                FinishCurrentWorkset(WorkerWorksetState::Failed);
                EnterTainted(diagnostic);
                return;
            }
            const SessionOperationReceipt resetting =
                session->BeginWorksetItemReset(
                    active_workset->definition.workset_id);
            if (!resetting.ok)
            {
                FailRemainingWorksetItems(
                    resetting.disposition == SessionDisposition::Tainted ||
                            resetting.backend.integrity ==
                                BackendIntegrity::Unknown
                        ? WorkerRejectionCode::SessionTainted
                        : WorkerRejectionCode::BackendFailure,
                    resetting.backend.message.empty()
                        ? "Workset item reset could not remove prior execution evidence"
                        : resetting.backend.message);
                FinishCurrentWorkset(WorkerWorksetState::Failed);
                if (resetting.disposition == SessionDisposition::Tainted ||
                    resetting.backend.integrity ==
                        BackendIntegrity::Unknown)
                {
                    EnterTainted(resetting.backend.message);
                }
                return;
            }
            active_workset->state = WorkerWorksetState::ResettingItem;
            RefreshSnapshot();
            PublishWorksetState(
                active_workset->definition.workset_id,
                active_workset->state);
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
        if (!prepared)
        {
            EnterTainted(
                "Executable WorkerWorkset item has no prepared invocation template");
            return;
        }
        const SessionSnapshot current = session->snapshot();
        const BackendResult derived_activation =
            session->ActivateDerivedStateForItem(
                active_workset->definition.derived_state,
                item.item_id);
        if (!derived_activation.ok)
        {
            active_invocation.emplace(
                item.execution.execution_id,
                item.execution.attempt_id,
                current.workset_epoch,
                prepared.maximum_artifacts);
            active_invocation->workset_id =
                active_workset->definition.workset_id;
            active_invocation->workset_item_id = item.item_id;
            active_invocation->workset_item_ordinal = ordinal;
            ProgramInvocationTerminalEvent terminal;
            terminal.invocation_id = item.execution.execution_id;
            terminal.attempt_id = item.execution.attempt_id;
            terminal.status = InvocationTerminalStatus::InfrastructureFailure;
            terminal.cleanup = derived_activation.integrity ==
                    BackendIntegrity::Unknown
                ? CleanupStatus::Failed
                : CleanupStatus::Clean;
            terminal.session_disposition = derived_activation.integrity ==
                    BackendIntegrity::Unknown
                ? SessionDisposition::Tainted
                : session->snapshot().disposition;
            terminal.workset_epoch = current.workset_epoch;
            terminal.error = {
                derived_activation.integrity == BackendIntegrity::Unknown
                    ? WorkerRejectionCode::SessionTainted
                    : WorkerRejectionCode::BackendFailure,
                derived_activation.message.empty()
                    ? "Derived state could not activate for the workset item"
                    : derived_activation.message};
            HandleInvocationTerminal(std::move(terminal));
            return;
        }
        active_workset->state = WorkerWorksetState::Running;
        PublishWorksetState(
            active_workset->definition.workset_id,
            active_workset->state);
        ChangeState(WorkerState::Running);
        active_invocation.emplace(
            item.execution.execution_id,
            item.execution.attempt_id,
            current.workset_epoch,
            prepared.maximum_artifacts);
        active_invocation->workset_id =
            active_workset->definition.workset_id;
        active_invocation->workset_item_id = item.item_id;
        active_invocation->workset_item_ordinal = ordinal;
        if (SessionVisualMessageService* visual =
                session->visual_messages())
        {
            const std::string_view display_name = ProgramKindDisplayName(
                active_workset->definition.phase_invocation
                    .program_package.identity.program_kind);
            if (!display_name.empty())
                (void)visual->SetCurrentPhase(display_name);
        }
        const WorkerOutboundSequence start_sequence =
            NextOutboundSequence();
        if (!start_sequence)
            return;
        if (!Publish(WorkerWorksetItemStartedEvent{
            start_sequence,
            active_workset->definition.workset_id,
            item.item_id,
            ordinal,
            item.execution.execution_id,
            item.execution.attempt_id,
            current.session_id,
            current.workset_epoch,
            active_workset->baseline}))
        {
            EnterTainted(
                "WorkerWorkset item-start event could not be published before execution");
            return;
        }
        const RuntimeError capture_error =
            AttachActiveWorksetCapture(current);
        if (capture_error)
        {
            ProgramInvocationTerminalEvent terminal;
            terminal.invocation_id = item.execution.execution_id;
            terminal.attempt_id = item.execution.attempt_id;
            terminal.status =
                InvocationTerminalStatus::InfrastructureFailure;
            terminal.cleanup =
                capture_error.code == WorkerRejectionCode::SessionTainted
                    ? CleanupStatus::Failed
                    : CleanupStatus::Clean;
            terminal.session_disposition =
                capture_error.code == WorkerRejectionCode::SessionTainted
                    ? SessionDisposition::Tainted
                    : session->snapshot().disposition;
            terminal.workset_epoch = current.workset_epoch;
            terminal.error = capture_error;
            HandleInvocationTerminal(std::move(terminal));
            return;
        }

        ProgramRuntimeSubmission submission;
        bool start_threw = false;
        try
        {
            submission = program_runtime->StartPreparedInvocation(
                {
                    current_snapshot.last_command_sequence,
                    prepared.template_id,
                    current.session_id,
                    current.workset_epoch,
                    active_workset->baseline.key.sha256,
                    active_workset->baseline.lineage,
                    active_workset->baseline.state_established,
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
            ProgramInvocationTerminalEvent terminal;
            terminal.invocation_id =
                item.execution.execution_id;
            terminal.attempt_id = item.execution.attempt_id;
            terminal.status =
                InvocationTerminalStatus::CleanupFailure;
            terminal.cleanup = CleanupStatus::Failed;
            terminal.session_disposition =
                SessionDisposition::Tainted;
            terminal.workset_epoch =
                session->snapshot().workset_epoch;
            terminal.error = {
                submission.error.code ==
                        WorkerRejectionCode::None
                    ? WorkerRejectionCode::InternalFailure
                    : submission.error.code,
                failure};
            HandleInvocationTerminal(std::move(terminal));
            return;
        }
        RefreshSnapshot();
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
        if (submission.execution_already_finished)
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
                WorkerRejectionCode::WorksetItemAlreadyTerminal,
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
            if (package->prepared[ordinal].template_id)
            {
                (void)program_runtime->ReleaseInvocationTemplate(
                    package->prepared[ordinal].template_id);
            }
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
        if (!taint_transition_active &&
            active_workset && !active_invocation)
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
        if (submission.execution_already_finished)
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
                        package->terminal_count
                            + package->initially_suppressed_count,
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
                result += pending->definition.items.size()
                    - pending->initial_cancellations.item_ids.size();
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
                if (ordinal < package.initially_suppressed.size() &&
                    package.initially_suppressed[ordinal])
                {
                    continue;
                }
                const WorksetItemTemplate& item =
                    package.definition.items[ordinal];
                items.emplace(WorkerItemExecutionCorrelation{
                    package.definition.workset_id,
                    item.item_id,
                    static_cast<std::uint32_t>(ordinal),
                    item.execution.execution_id,
                    item.execution.attempt_id});
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
                    if (std::ranges::find(
                            pending->initial_cancellations.item_ids,
                            item.item_id) !=
                        pending->initial_cancellations.item_ids.end())
                    {
                        continue;
                    }
                    items.emplace(WorkerItemExecutionCorrelation{
                        pending->definition.workset_id,
                        item.item_id,
                        static_cast<std::uint32_t>(ordinal),
                        item.execution.execution_id,
                        item.execution.attempt_id});
                }
            }
        }
        for (const auto& [_, terminal] : retained_terminals)
            items.emplace(ExecutionCorrelation(
                terminal.event.correlation));
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
            retained_terminals.size();
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
                retained_terminals.size())});
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
        bool unstarted)
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
                std::size_t bytes =
                    sizeof(WorkerWorksetItemTerminalEvent) +
                    value.output_payload.size() +
                    value.error.message.size();
                for (const auto& artifact : value.workset_artifacts)
                {
                    bytes += artifact.artifact_id.size() +
                        artifact.schema.canonical_id.size() +
                        artifact.storage_reference.size() + 128;
                }
                for (const std::string& diagnostic : value.diagnostics)
                    bytes += diagnostic.size();
                return bytes;
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
        if (retained_terminals.size() >=
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
            item.execution.execution_id,
            item.execution.attempt_id,
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
        // The ledger's byte bound covers the complete retained event, not
        // only ProgramResult bytes. Its immutable payload is internal
        // retention evidence; the typed event remains authoritative.
        std::vector<std::uint8_t> ledger_payload(
            encoded_bytes,
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
        package.terminalized[ordinal] = true;
        ++package.terminal_count;
        if (unstarted)
            ++package.unstarted_count;
        retained_terminal_bytes += encoded_bytes;
        const WorkerItemTerminalCorrelation completed_correlation =
            event.correlation;
        retained_terminals.emplace(
            event.correlation.terminal_id.value(),
            RetainedTerminal{
                std::move(event),
                encoded_bytes});
        CloseActiveItemAfterTerminalRetention(
            completed_correlation);
        RefreshSnapshot();
        if (!terminal_publication_deferred)
        {
            if (!PublishReadyWorksetTerminals())
                return;
            PublishCredits();
        }
    }

    void CloseActiveItemAfterTerminalRetention(
        const WorkerItemTerminalCorrelation& correlation)
    {
        if (!active_invocation ||
            !active_invocation->workset_id ||
            !active_invocation->workset_item_id ||
            *active_invocation->workset_id != correlation.workset_id ||
            *active_invocation->workset_item_id != correlation.item_id ||
            active_invocation->workset_item_ordinal !=
                correlation.item_ordinal ||
            active_invocation->invocation_id !=
                correlation.invocation_id ||
            active_invocation->attempt_id != correlation.attempt_id)
        {
            return;
        }

        auto& transaction = active_invocation->outputs;
        if (transaction.state ==
            ActiveInvocation::OutputTransactionState::Finalizing)
        {
            const bool published = std::ranges::all_of(
                transaction.outputs,
                [](const ActiveInvocation::Output& output) {
                    return output.state ==
                        ActiveInvocation::OutputState::Published;
                });
            transaction.state = published
                ? ActiveInvocation::OutputTransactionState::Committed
                : ActiveInvocation::OutputTransactionState::Abandoned;
        }
        else if (transaction.state ==
                 ActiveInvocation::OutputTransactionState::SealedForAbandon)
        {
            const bool abandoned = std::ranges::all_of(
                transaction.outputs,
                [](const ActiveInvocation::Output& output) {
                    return output.state ==
                        ActiveInvocation::OutputState::Abandoned;
                });
            if (abandoned)
            {
                transaction.state =
                    ActiveInvocation::OutputTransactionState::Abandoned;
            }
        }
        if (transaction.state !=
                ActiveInvocation::OutputTransactionState::Committed &&
            transaction.state !=
                ActiveInvocation::OutputTransactionState::Abandoned)
        {
            if (taint_transition_active)
                return;
            EnterTainted(
                "Active workset item reached terminal retention with unresolved outputs");
            return;
        }

        const std::uint32_t ordinal =
            active_invocation->workset_item_ordinal;
        if (SessionVisualMessageService* visual = session->visual_messages())
            (void)visual->SetIdle();
        active_invocation.reset();
        if (active_workset &&
            active_workset->definition.workset_id ==
                correlation.workset_id &&
            active_workset->next_item == ordinal)
        {
            ++active_workset->next_item;
        }
        RefreshSnapshot();

        if (handling_execution_finished)
            return;
        if (!pending_shutdown_commands.empty() ||
            Snapshot().state == WorkerState::Stopping)
        {
            FinishShutdown(false);
        }
        else if (active_workset)
        {
            StartNextWorksetItem();
        }
        else if (session->snapshot().disposition !=
                 SessionDisposition::Tainted)
        {
            ChangeState(WorkerState::Ready);
        }
    }

    void RetainUnstartedTerminal(
        WorksetPackage& package,
        std::uint32_t ordinal,
        std::string message)
    {
        if (ordinal < package.initially_suppressed.size()
            && package.initially_suppressed[ordinal])
        {
            return;
        }
        const WorksetItemTemplate& item =
            package.definition.items[ordinal];
        ProgramInvocationTerminalEvent terminal;
        terminal.invocation_id =
            item.execution.execution_id;
        terminal.attempt_id = item.execution.attempt_id;
        terminal.status = InvocationTerminalStatus::Cancelled;
        terminal.cleanup = CleanupStatus::Clean;
        terminal.session_disposition = session
            ? session->snapshot().disposition
            : SessionDisposition::Closed;
        terminal.workset_epoch = session
            ? session->snapshot().workset_epoch
            : WorksetEpoch{};
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
                item.execution.execution_id;
            terminal.attempt_id = item.execution.attempt_id;
            terminal.status =
                InvocationTerminalStatus::InfrastructureFailure;
            terminal.cleanup = CleanupStatus::Clean;
            terminal.session_disposition =
                session->snapshot().disposition;
            terminal.workset_epoch =
                session->snapshot().workset_epoch;
            terminal.error = {code, message};
            RetainWorksetTerminal(
                *active_workset,
                ordinal,
                std::move(terminal),
                true);
            if (active_workset->prepared[ordinal].template_id)
            {
                (void)program_runtime->ReleaseInvocationTemplate(
                    active_workset->prepared[ordinal].template_id);
            }
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
        PublishWorksetState(
            package.definition.workset_id,
            WorkerWorksetState::Draining);
        DrainingWorkset draining;
        draining.item_count =
            static_cast<std::uint32_t>(
                package.definition.items.size());
        draining.terminal_count = package.terminal_count;
        draining.unstarted_count = package.unstarted_count;
        draining.initially_suppressed_count =
            package.initially_suppressed_count;
        draining.unacknowledged =
            static_cast<std::uint32_t>(
                std::ranges::count_if(
                    retained_terminals,
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
            draining.unstarted_count,
            draining.initially_suppressed_count});
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

        if (!active_workset || !active_invocation ||
            !command.workset_id || !command.item_id ||
            active_workset->definition.workset_id != command.workset_id ||
            active_invocation->workset_id != command.workset_id ||
            active_invocation->workset_item_id != command.item_id)
        {
            Reject(
                queued,
                WorkerRejectionCode::WorksetItemNotFound,
                "Screenshot does not identify the exact active workset item");
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
        if (worker.state != WorkerState::Running &&
            worker.state != WorkerState::Cancelling)
        {
            Reject(
                queued,
                worker.state == WorkerState::Tainted
                    ? WorkerRejectionCode::SessionTainted
                    : WorkerRejectionCode::InvalidState,
                "Execution control requires a running workset item");
            return;
        }
        if (!active_invocation || !active_workset ||
            !command.workset_id || !command.item_id ||
            active_workset->definition.workset_id != command.workset_id ||
            active_invocation->workset_id != command.workset_id ||
            active_invocation->workset_item_id != command.item_id)
        {
            Reject(
                queued,
                WorkerRejectionCode::WorksetItemNotFound,
                "Execution control does not identify the exact active workset item");
            return;
        }
        if (worker_mode != WorkerMode::VisualDebug)
        {
            Reject(
                queued,
                WorkerRejectionCode::Unsupported,
                "Execution control is reserved for VisualDebug workers");
            return;
        }
        const auto execution_capabilities =
            session->execution_capabilities();
        if (!HasExecutionCapability(
                execution_capabilities,
                BackendExecutionCapability::Pause) ||
            !HasExecutionCapability(
                execution_capabilities,
                BackendExecutionCapability::Resume) ||
            !HasExecutionCapability(
                execution_capabilities,
                BackendExecutionCapability::FrameStep))
        {
            Reject(
                queued,
                WorkerRejectionCode::Unsupported,
                "Interactive visual-debug execution control is unavailable");
            return;
        }

        const auto* session_action_host =
            dynamic_cast<const program::SessionProgramActionHost*>(
                program_action_host.get());
        const program::SessionProgramActionHostSnapshot action =
            session_action_host
            ? session_action_host->snapshot()
            : program::SessionProgramActionHostSnapshot{};
        if (action.execution_pending)
        {
            Reject(
                queued,
                WorkerRejectionCode::InvalidState,
                "Execution control cannot overlap a program-owned execution action");
            return;
        }
        const SessionSnapshot session_snapshot = session->snapshot();

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
            resume.expected_epoch = session_snapshot.workset_epoch;
            request = std::move(resume);
        }
        else
        {
            ExecutionRequestPolicy policy;
            policy.expected_epoch = session_snapshot.workset_epoch;
            policy.interruptions = ExecutionInterruptionPolicy::Reject;
            switch (command.control)
            {
            case WorkerExecutionControlKind::Pause:
                request = SafePauseRequest{
                    std::move(policy),
                    command.timeout};
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
                    ? "ExecutionControlCore rejected execution control"
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

        std::optional<ExecutionSnapshot> execution = session
            ? session->execution_snapshot()
            : std::nullopt;
        if (execution && execution->activity !=
                ExecutionActivity::IdlePaused)
        {
            (void)session->CancelExecution(CancellationReason::Shutdown);
            PumpExecutionEvents();
            execution = session
                ? session->execution_snapshot()
                : std::nullopt;
            if (execution && execution->activity !=
                    ExecutionActivity::IdlePaused)
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
                if (submission.execution_already_finished)
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

    [[nodiscard]] bool AbandonStagedOutputs(
        std::vector<program::StagedProgramOutput>& outputs) noexcept
    {
        bool proven = true;
        for (program::StagedProgramOutput& output : outputs)
        {
            auto* savestate =
                std::get_if<program::StagedSavestateOutput>(&output);
            if (!savestate || !savestate->capture.artifact)
                continue;
            const SavestateServiceResult abandoned =
                session->AbandonImmutableSavestateArtifact(
                    savestate->capture.artifact);
            if (!abandoned.ok &&
                abandoned.code != SavestateServiceErrorCode::NotFound)
            {
                proven = false;
            }
        }
        outputs.clear();
        return proven;
    }

    void QueueWorksetBegin()
    {
        bool queued = false;
        {
            std::lock_guard lock(mailbox->mutex);
            if (!mailbox->workset_begin_queued)
            {
                MailboxItem item;
                item.kind = MailboxItemKind::BeginWorksetExecution;
                mailbox->items.push_back(std::move(item));
                mailbox->workset_begin_queued = true;
                queued = true;
            }
        }
        if (queued)
            SignalMailbox(*mailbox);
    }

    [[nodiscard]] bool AbandonActiveOutputTransaction() noexcept
    {
        if (!active_invocation)
            return true;
        auto& transaction = active_invocation->outputs;
        if (transaction.state ==
                ActiveInvocation::OutputTransactionState::Committed ||
            transaction.state ==
                ActiveInvocation::OutputTransactionState::Abandoned)
        {
            return true;
        }
        transaction.state =
            ActiveInvocation::OutputTransactionState::SealedForAbandon;
        bool proven = true;
        for (auto& output : transaction.outputs)
        {
            if (output.state == ActiveInvocation::OutputState::Published ||
                output.state == ActiveInvocation::OutputState::Abandoned)
            {
                continue;
            }
            const SavestateServiceResult abandoned =
                session->AbandonImmutableSavestateArtifact(
                    output.savestate.capture.artifact);
            if (!abandoned.ok &&
                abandoned.code != SavestateServiceErrorCode::NotFound)
            {
                proven = false;
            }
            else
            {
                output.state = ActiveInvocation::OutputState::Abandoned;
            }
        }
        if (proven)
            transaction.state =
                ActiveInvocation::OutputTransactionState::Abandoned;
        return proven;
    }

    // Seals the actor-owned transaction without stealing immutable bytes from
    // an accepted finalizer job. Outputs that have not left the actor are
    // abandoned immediately. Accepted finalizer jobs are drained to a typed
    // completion, but their evidence is deliberately not committed or exposed
    // as an authoritative program artifact.
    [[nodiscard]] bool SealActiveOutputTransactionForAbandon() noexcept
    {
        if (!active_invocation)
            return true;
        auto& transaction = active_invocation->outputs;
        if (transaction.state ==
                ActiveInvocation::OutputTransactionState::Committed ||
            transaction.state ==
                ActiveInvocation::OutputTransactionState::Abandoned)
        {
            return true;
        }
        if (transaction.state ==
                ActiveInvocation::OutputTransactionState::Open ||
            transaction.state ==
                ActiveInvocation::OutputTransactionState::SealedForCommit)
        {
            return AbandonActiveOutputTransaction();
        }

        transaction.state =
            ActiveInvocation::OutputTransactionState::SealedForAbandon;
        bool proven = true;
        bool awaiting_finalizer = false;
        for (auto& output : transaction.outputs)
        {
            if (output.state == ActiveInvocation::OutputState::Abandoned)
                continue;
            if (output.state == ActiveInvocation::OutputState::Adopted)
            {
                const SavestateServiceResult abandoned =
                    session->AbandonImmutableSavestateArtifact(
                        output.savestate.capture.artifact);
                if (!abandoned.ok &&
                    abandoned.code != SavestateServiceErrorCode::NotFound)
                {
                    proven = false;
                }
                else
                {
                    output.state = ActiveInvocation::OutputState::Abandoned;
                }
                continue;
            }

            if (output.finalization_id)
            {
                const auto publication = artifact_publications.find(
                    output.finalization_id.value());
                if (publication == artifact_publications.end())
                {
                    proven = false;
                    continue;
                }
                publication->second.abandon_requested = true;
                publication->second.artifact.reset();
                awaiting_finalizer =
                    awaiting_finalizer || !publication->second.completed;
            }
            output.artifact.reset();
            output.state = ActiveInvocation::OutputState::Abandoned;
        }
        if (proven && !awaiting_finalizer)
        {
            transaction.state =
                ActiveInvocation::OutputTransactionState::Abandoned;
        }
        return proven;
    }

    [[nodiscard]] bool
    AbandonOutputsAfterFinalizerShutdown() noexcept
    {
        if (!active_invocation)
            return true;
        if (!artifact_finalizer)
            return AbandonActiveOutputTransaction();
        const SavestateArtifactFinalizerSnapshot finalizer =
            artifact_finalizer->snapshot();
        if (!finalizer.shutdown || finalizer.outstanding_jobs != 0 ||
            finalizer.running != 0 || finalizer.queued != 0)
        {
            return false;
        }

        bool proven = true;
        auto& transaction = active_invocation->outputs;
        transaction.state =
            ActiveInvocation::OutputTransactionState::SealedForAbandon;
        for (auto& output : transaction.outputs)
        {
            if (output.state == ActiveInvocation::OutputState::Abandoned)
                continue;
            const SavestateServiceResult abandoned =
                session->AbandonImmutableSavestateArtifact(
                    output.savestate.capture.artifact);
            if (!abandoned.ok &&
                abandoned.code != SavestateServiceErrorCode::NotFound)
            {
                proven = false;
                continue;
            }
            output.artifact.reset();
            output.state = ActiveInvocation::OutputState::Abandoned;
            if (output.finalization_id)
            {
                artifact_publications.erase(
                    output.finalization_id.value());
            }
        }
        if (proven)
        {
            transaction.state =
                ActiveInvocation::OutputTransactionState::Abandoned;
        }
        return proven;
    }

    program::ProgramActionResolution AdoptActorActionResult(
        program::ActorActionResult result)
    {
        auto fail = [&](std::string message) {
            const bool abandonment_proven =
                AbandonStagedOutputs(result.staged_outputs);
            if (!abandonment_proven)
            {
                session->MarkTainted(
                    "Rejected staged output ownership could not be abandoned");
            }
            result.resolution.status =
                abandonment_proven
                ? program::ProgramActionResolutionStatus::Failed
                : program::ProgramActionResolutionStatus::CleanupFailed;
            result.resolution.output = {};
            result.resolution.resources.clear();
            result.resolution.cleanup = abandonment_proven
                ? program::ProgramCleanupStatus::Clean
                : program::ProgramCleanupStatus::Tainted;
            result.resolution.session_disposition =
                abandonment_proven
                ? session->snapshot().disposition
                : SessionDisposition::Tainted;
            result.resolution.code = abandonment_proven
                ? "staged_output_adoption_failed"
                : "staged_output_abandonment_failed";
            result.resolution.message = std::move(message);
            return std::move(result.resolution);
        };

        if (result.staged_outputs.empty())
            return std::move(result.resolution);
        if (!active_invocation ||
            result.resolution.invocation_id !=
                active_invocation->invocation_id ||
            result.resolution.attempt_id != active_invocation->attempt_id ||
            result.resolution.workset_epoch !=
                active_invocation->workset_epoch)
        {
            return fail(
                "Staged output does not match the active workset item");
        }
        if (result.resolution.status !=
                program::ProgramActionResolutionStatus::Completed ||
            active_invocation->outputs.state !=
                ActiveInvocation::OutputTransactionState::Open)
        {
            return fail(
                "Only a successful action may adopt output into an open transaction");
        }
        if (active_invocation->outputs.outputs.size() +
                result.staged_outputs.size() >
            active_invocation->outputs.maximum_artifacts)
        {
            return fail(
                "Staged output exceeds the verified invocation artifact allowance");
        }

        std::unordered_set<std::string> logical_ids;
        std::unordered_set<std::uint64_t> service_ids;
        std::unordered_set<std::string> paths;
        std::size_t resident = active_invocation->outputs.resident_bytes;
        for (const auto& output : active_invocation->outputs.outputs)
        {
            logical_ids.emplace(output.savestate.artifact_id);
            service_ids.emplace(
                output.savestate.capture.artifact.value());
            paths.emplace(
                output.savestate.capture.final_path
                    .lexically_normal().string());
            if (output.savestate.capture.movie_bytes)
            {
                paths.emplace(
                    SavestateDtmSidecarPath(
                        output.savestate.capture.final_path)
                        .lexically_normal().string());
            }
        }
        for (const program::StagedProgramOutput& staged :
             result.staged_outputs)
        {
            const auto* output =
                std::get_if<program::StagedSavestateOutput>(&staged);
            if (!output || output->artifact_id.empty() ||
                !output->capture.result.ok ||
                !output->capture.artifact ||
                output->capture.captured_epoch !=
                    active_invocation->workset_epoch ||
                !output->capture.state_bytes ||
                output->capture.final_path.empty() ||
                !logical_ids.emplace(output->artifact_id).second ||
                !service_ids.emplace(
                    output->capture.artifact.value()).second ||
                !paths.emplace(
                    output->capture.final_path
                        .lexically_normal().string()).second)
            {
                return fail(
                    "Staged savestate output identity, path, bytes, or correlation is invalid");
            }
            const bool has_movie =
                output->capture.movie.has_value();
            const bool has_movie_bytes =
                output->capture.movie_bytes.has_value();
            if (has_movie != has_movie_bytes)
            {
                return fail(
                    "Staged savestate sidecar metadata and bytes disagree");
            }
            if (has_movie_bytes &&
                !paths.emplace(
                    SavestateDtmSidecarPath(
                        output->capture.final_path)
                        .lexically_normal().string()).second)
            {
                return fail(
                    "Staged savestate sidecar path is duplicated");
            }
            if (output->capture.resident_bytes() >
                workset_limits.maximum_pending_finalizer_bytes -
                    std::min(
                        resident,
                        workset_limits.maximum_pending_finalizer_bytes))
            {
                return fail(
                    "Staged output exceeds worker finalizer resident-byte capacity");
            }
            resident += output->capture.resident_bytes();
        }

        for (program::StagedProgramOutput& staged : result.staged_outputs)
        {
            auto& output =
                std::get<program::StagedSavestateOutput>(staged);
            active_invocation->outputs.outputs.push_back({
                result.resolution.request_id,
                std::move(output)});
        }
        active_invocation->outputs.resident_bytes = resident;
        return std::move(result.resolution);
    }

    [[nodiscard]] bool QueueProgramActionResolution(
        program::ProgramActionResolution completion)
    {
        bool queued = false;
        {
            std::lock_guard lock(mailbox->mutex);
            if (mailbox->accept_program_actions)
            {
                MailboxItem item;
                item.kind =
                    MailboxItemKind::ProgramActionResolution;
                item.program_action_resolution.emplace(
                    std::move(completion));
                mailbox->items.push_back(std::move(item));
                queued = true;
            }
        }
        if (queued)
            SignalMailbox(*mailbox);
        return queued;
    }

    [[nodiscard]] program::ProgramActionResolution
    RejectProgramAction(
        const program::ProgramActionRequest& request,
        program::ProgramActionResolutionStatus status,
        std::string code,
        std::string message) const
    {
        const WorksetEpoch current_epoch =
            session ? session->snapshot().workset_epoch : WorksetEpoch{};
        return {
            .request_id = request.request_id,
            .invocation_id = request.invocation_id,
            .attempt_id = request.attempt_id,
            .operation = request.operation,
            .status = status,
            .workset_epoch = current_epoch,
            .cleanup =
                status ==
                    program::ProgramActionResolutionStatus::CleanupFailed
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
            (void)QueueProgramActionResolution(RejectProgramAction(
                request,
                program::ProgramActionResolutionStatus::Rejected,
                "invocation_mismatch",
                "Program action does not identify the active invocation"));
            return;
        }
        if (!request.request_id)
        {
            (void)QueueProgramActionResolution(RejectProgramAction(
                request,
                program::ProgramActionResolutionStatus::Rejected,
                "invalid_request",
                "Program action request identity is zero"));
            return;
        }
        const SessionSnapshot current = session->snapshot();
        if (!request.expected_epoch ||
            request.expected_epoch != current.workset_epoch)
        {
            (void)QueueProgramActionResolution(RejectProgramAction(
                request,
                program::ProgramActionResolutionStatus::StaleEpoch,
                "stale_epoch",
                "Program action expected a stale WorksetEpoch"));
            return;
        }
        if (!program_action_host)
        {
            (void)QueueProgramActionResolution(RejectProgramAction(
                request,
                program::ProgramActionResolutionStatus::Unsupported,
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
            (void)QueueProgramActionResolution(
                dispatched.immediate_result
                    ? AdoptActorActionResult(
                          std::move(*dispatched.immediate_result))
                    : RejectProgramAction(
                        request,
                        program::ProgramActionResolutionStatus::Rejected,
                        "action_rejected",
                        dispatched.diagnostic.empty()
                            ? "Program action host rejected the request"
                            : std::move(dispatched.diagnostic)));
            return;
        }
        // Even an immediate service result crosses the mailbox before it can
        // resume ProgramRuntime.
        if (dispatched.immediate_result)
        {
            const bool queued = QueueProgramActionResolution(
                AdoptActorActionResult(
                    std::move(*dispatched.immediate_result)));
            if (!queued)
            {
                const bool abandoned =
                    AbandonActiveOutputTransaction();
                EnterTainted(
                    abandoned
                        ? "Adopted program output could not be delivered to ProgramRuntime"
                        : "Adopted program output delivery and abandonment both failed");
            }
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
            if (active_workset->prepared[ordinal].template_id)
            {
                (void)program_runtime->ReleaseInvocationTemplate(
                    active_workset->prepared[ordinal].template_id);
            }
        }
        active_workset->admission_close_reason =
            std::string(reason);
    }

    [[nodiscard]] std::string FinalizeActiveOutputs()
    {
        if (!active_invocation)
            return "Staged output transaction has no active invocation owner";
        auto& transaction = active_invocation->outputs;
        if (transaction.state !=
            ActiveInvocation::OutputTransactionState::Open)
        {
            return "Staged output transaction is not open for commit";
        }
        transaction.state =
            ActiveInvocation::OutputTransactionState::SealedForCommit;
        if (transaction.outputs.empty())
        {
            transaction.state =
                ActiveInvocation::OutputTransactionState::Committed;
            return {};
        }
        if (!active_invocation->workset_id ||
            !active_invocation->workset_item_id ||
            !artifact_finalizer)
        {
            (void)AbandonActiveOutputTransaction();
            return "Staged output transaction has no exact workset item owner";
        }

        const WorkerItemExecutionCorrelation correlation{
            *active_invocation->workset_id,
            *active_invocation->workset_item_id,
            active_invocation->workset_item_ordinal,
            active_invocation->invocation_id,
            active_invocation->attempt_id};
        transaction.state =
            ActiveInvocation::OutputTransactionState::Finalizing;
        for (auto& output : transaction.outputs)
        {
            auto& publication = output.savestate;
            SavestateArtifactFinalizationRequest request;
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
                    publication.capture.movie->dtm_sha256.empty())
                {
                    (void)session
                        ->AbandonImmutableSavestateArtifact(
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
                    SavestateDtmSidecarPath(
                        publication.capture.final_path);
                sidecar.bytes = std::move(
                    *publication.capture.movie_bytes);
                sidecar.expected_sha256 =
                    publication.capture.movie->dtm_sha256;
                request.sidecars.push_back(
                    std::move(sidecar));
            }
            const SavestateArtifactId state_artifact_id =
                request.state_artifact_id;
            const std::string logical_artifact_id =
                request.logical_artifact_id;
            const SavestateArtifactFinalizerSubmission submitted =
                artifact_finalizer->Submit(std::move(request));
            if (!submitted.result.ok)
            {
                (void)session->AbandonImmutableSavestateArtifact(
                    state_artifact_id);
                active_invocation
                    ->artifact_finalization_failure =
                    submitted.result.message.empty()
                    ? "State artifact finalizer rejected captured bytes"
                    : submitted.result.message;
                CloseActiveWorksetAdmissionAfterPublication(
                    "Artifact publication failure closed later workset admission");
                output.state =
                    ActiveInvocation::OutputState::Abandoned;
                output.failure = active_invocation
                    ->artifact_finalization_failure;
                continue;
            }
            active_invocation->artifact_publication_promoted =
                true;
            active_invocation->artifact_finalizations.push_back(
                submitted.finalization_id);
            output.finalization_id = submitted.finalization_id;
            output.state = ActiveInvocation::OutputState::Finalizing;
            artifact_publications.emplace(
                submitted.finalization_id.value(),
                ArtifactPublication{
                    correlation,
                    state_artifact_id,
                    logical_artifact_id});
        }
        RefreshSnapshot();
        PublishCredits();
        if (!active_invocation
                 ->artifact_finalization_failure.empty())
        {
            (void)SealActiveOutputTransactionForAbandon();
            return active_invocation
                ->artifact_finalization_failure;
        }
        return {};
    }

    [[nodiscard]] bool TryRetainActiveItemTerminal()
    {
        if (!active_invocation ||
            !active_invocation->terminal_draft)
        {
            return false;
        }
        if (!active_workset ||
            !active_invocation->workset_id ||
            !active_invocation->workset_item_id ||
            active_workset->definition.workset_id !=
                *active_invocation->workset_id ||
            active_invocation->workset_item_ordinal >=
                active_workset->definition.items.size())
        {
            EnterTainted(
                "A finished program execution lost its active workset item");
            return false;
        }

        auto& transaction = active_invocation->outputs;
        if (std::ranges::any_of(
                transaction.outputs,
                [](const ActiveInvocation::Output& output) {
                    return output.state ==
                        ActiveInvocation::OutputState::Finalizing;
                }))
        {
            return false;
        }

        if (transaction.state ==
            ActiveInvocation::OutputTransactionState::Finalizing)
        {
            const bool published = std::ranges::all_of(
                transaction.outputs,
                [](const ActiveInvocation::Output& output) {
                    return output.state ==
                            ActiveInvocation::OutputState::Published &&
                        output.artifact.has_value() &&
                        output.failure.empty();
                });
            transaction.state = published
                ? ActiveInvocation::OutputTransactionState::Committed
                : ActiveInvocation::OutputTransactionState::Abandoned;
        }
        else if (transaction.state ==
                 ActiveInvocation::OutputTransactionState::SealedForAbandon)
        {
            const bool abandoned = std::ranges::all_of(
                transaction.outputs,
                [](const ActiveInvocation::Output& output) {
                    return output.state ==
                        ActiveInvocation::OutputState::Abandoned;
                });
            if (!abandoned)
                return false;
            transaction.state =
                ActiveInvocation::OutputTransactionState::Abandoned;
        }

        if (transaction.state !=
                ActiveInvocation::OutputTransactionState::Committed &&
            transaction.state !=
                ActiveInvocation::OutputTransactionState::Abandoned)
        {
            EnterTainted(
                "A finished program execution retained an unresolved output transaction");
            return false;
        }

        ProgramInvocationTerminalEvent terminal =
            std::move(*active_invocation->terminal_draft);
        active_invocation->terminal_draft.reset();
        std::string publication_failure =
            active_invocation->artifact_finalization_failure;
        if (terminal.status == InvocationTerminalStatus::Completed &&
            transaction.state ==
                ActiveInvocation::OutputTransactionState::Committed &&
            !transaction.outputs.empty())
        {
            auto decoded = program::DecodeProgramResultV1(
                terminal.output_payload);
            if (!decoded)
            {
                publication_failure = decoded.status.message.empty()
                    ? "ProgramResult could not be decoded for output finalization"
                    : decoded.status.message;
            }
            else
            {
                program::ProgramResult result =
                    std::move(*decoded.value);
                std::uint64_t next_sequence = 1;
                for (const program::ProgramArtifact& artifact :
                     result.artifacts)
                {
                    next_sequence = std::max(
                        next_sequence,
                        artifact.sequence.value() + 1);
                }
                for (const ActiveInvocation::Output& output :
                     transaction.outputs)
                {
                    if (!output.failure.empty() || !output.artifact)
                    {
                        publication_failure = output.failure.empty()
                            ? "Finalized output did not produce authoritative evidence"
                            : output.failure;
                        break;
                    }
                    result.artifacts.push_back(
                        program::ProgramArtifact{
                            program::ProgramArtifactSequence(
                                next_sequence++),
                            *output.artifact});
                }
                if (publication_failure.empty())
                {
                    const program::EncodeResult encoded =
                        program::EncodeProgramResultV1(result);
                    if (!encoded)
                    {
                        publication_failure =
                            encoded.status.message.empty()
                            ? "Final ProgramResult artifact encoding failed"
                            : encoded.status.message;
                    }
                    else
                    {
                        terminal.output_payload =
                            std::move(encoded.bytes);
                    }
                }
            }
        }
        else if (terminal.status ==
                     InvocationTerminalStatus::Completed &&
                 transaction.state ==
                     ActiveInvocation::OutputTransactionState::Abandoned)
        {
            publication_failure = publication_failure.empty()
                ? "Program output transaction was abandoned before commit"
                : publication_failure;
        }

        if (!publication_failure.empty())
        {
            terminal.status =
                InvocationTerminalStatus::InfrastructureFailure;
            terminal.output_payload.clear();
            terminal.error = {
                WorkerRejectionCode::BackendFailure,
                publication_failure};
        }

        for (const SavestateArtifactFinalizationId id :
             active_invocation->artifact_finalizations)
        {
            artifact_publications.erase(id.value());
        }
        const std::uint32_t ordinal =
            active_invocation->workset_item_ordinal;
        handling_execution_finished = true;
        RetainWorksetTerminal(
            *active_workset,
            ordinal,
            std::move(terminal),
            false);
        handling_execution_finished = false;
        return !active_invocation;
    }

    void HandleProgramActionResolution(
        program::ProgramActionResolution completion)
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
            delivered = program_runtime->DeliverActionResolution(
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
        TakeFinishedExecution();
        QueueProgramPump();
    }

    void TakeFinishedExecution()
    {
        if (!program_runtime || !active_invocation ||
            active_invocation->execution_finished)
        {
            return;
        }
        ProgramExecutionTakeResult taken;
        try
        {
            taken = program_runtime->TakeFinishedExecution(
                active_invocation->invocation_id,
                active_invocation->attempt_id);
        }
        catch (const std::exception& ex)
        {
            EnterTainted(
                std::string("Finished-execution take threw: ") +
                ex.what());
            return;
        }
        catch (...)
        {
            EnterTainted("Finished-execution take threw");
            return;
        }
        if (taken.error)
        {
            EnterTainted(
                taken.error.message.empty()
                    ? "ProgramRuntime rejected its exact finished-execution take"
                    : taken.error.message);
            return;
        }
        if (!taken.taken)
            return;
        if (!taken.finished)
        {
            EnterTainted(
                "ProgramRuntime reported a taken execution without its draft");
            return;
        }
        active_invocation->execution_finished =
            std::move(*taken.finished);
        const ProgramExecutionFinished& finished =
            *active_invocation->execution_finished;
        ProgramInvocationTerminalEvent terminal{
            finished.invocation_id,
            finished.attempt_id,
            finished.status,
            finished.cleanup,
            finished.session_disposition,
            finished.workset_epoch,
            finished.output_payload,
            finished.error,
            {},
            {},
            finished.cancellation_reason};
        HandleInvocationTerminal(std::move(terminal));
    }

    void PumpProgramRuntime()
    {
        if (!program_runtime || !active_invocation)
            return;
        try
        {
            if (program_runtime->Pump())
                QueueProgramPump();
            TakeFinishedExecution();
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
            std::vector<program::ActorActionResult> completions =
                program_action_host->DrainResults();
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
                    for (program::ActorActionResult& completion :
                         completions)
                    {
                        MailboxItem item;
                        item.kind =
                            MailboxItemKind::ProgramActionResolution;
                        item.program_action_resolution.emplace(
                            AdoptActorActionResult(
                                std::move(completion)));
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
            if (!queued)
            {
                bool abandoned = true;
                for (program::ActorActionResult& completion : completions)
                {
                    abandoned =
                        AbandonStagedOutputs(
                            completion.staged_outputs) &&
                        abandoned;
                }
                if (active_invocation)
                {
                    abandoned =
                        AbandonActiveOutputTransaction() &&
                        abandoned;
                    EnterTainted(
                        abandoned
                            ? "Deferred action resolution could not be delivered to ProgramRuntime"
                            : "Deferred action resolution delivery and output abandonment both failed");
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
                [this](ProgramInvocationObservationEvent& observation) {
                    if (!active_invocation ||
                        !active_workset ||
                        observation.invocation_id !=
                            active_invocation->invocation_id ||
                        observation.attempt_id != active_invocation->attempt_id)
                    {
                        Publish(WorkerRuntimeDiagnosticEvent{
                            WorkerRejectionCode::InvocationMismatch,
                            "Ignored observation for a non-active invocation "
                            "attempt",
                            observation.invocation_id});
                        return;
                    }
                    const auto point = std::ranges::find_if(
                        active_workset->definition.progress_plan.points,
                        [](const progress::ProgressPointBindingV1& candidate) {
                            return candidate.provider ==
                                    progress::ProgressProviderKind::PhaseLibrary &&
                                candidate.library_id ==
                                    "soa.progress.predicate.evaluations/1" &&
                                candidate.point_id ==
                                    "predicate.evaluated";
                        });
                    if (point == active_workset->definition
                            .progress_plan.points.end())
                    {
                        return;
                    }
                    const auto root = std::ranges::find(
                        observation.emission.value.values,
                        observation.emission.value.root,
                        &program::ProgramValue::id);
                    const auto* evaluation = root ==
                            observation.emission.value.values.end()
                        ? nullptr
                        : std::get_if<program::EnumValue>(
                              &root->payload);
                    if (!observation.emission.complete ||
                        !observation.emission.schema.canonical_id.ends_with(
                            ".Evaluation") ||
                        evaluation == nullptr ||
                        evaluation->schema != observation.emission.schema ||
                        (evaluation->value != 0 && evaluation->value != 1))
                    {
                        Publish(WorkerRuntimeDiagnosticEvent{
                            WorkerRejectionCode::InvalidArgument,
                            "Phase-library progress emission is not a registered predicate evaluation",
                            observation.invocation_id});
                        return;
                    }
                    const program::EncodeResult encoded =
                        program::EncodeProgramEmissionV1(
                            observation.emission);
                    if (!encoded)
                    {
                        Publish(WorkerRuntimeDiagnosticEvent{
                            WorkerRejectionCode::InternalFailure,
                            encoded.status.message.empty()
                                ? "Program observation could not be encoded"
                                : encoded.status.message,
                            observation.invocation_id});
                        return;
                    }
                    std::string display = "Predicate " +
                        observation.emission.schema.canonical_id +
                        (evaluation->value == 0
                            ? ": Passed"
                            : ": Failed");
                    const WorksetItemTemplate& item =
                        active_workset->definition.items[
                            active_invocation->workset_item_ordinal];
                    Publish(progress::CanonicalProgressEventV1{
                        .workset_id = active_workset->definition.workset_id,
                        .item_id = item.item_id,
                        .durable_job_id = item.correlation.durable_job_id,
                        .invocation_id = observation.invocation_id,
                        .attempt_id = observation.attempt_id,
                        .ordinal = active_invocation
                            ->next_progress_ordinal++,
                        .library_id = point->library_id,
                        .library_revision = point->library_revision,
                        .progress_point_id = point->point_id,
                        .schema = point->schema,
                        .typed_payload = encoded.bytes,
                        .display_text = std::move(display),
                    });
                },
                [this](ProgramInvocationCompletionAvailableEvent& completion) {
                    if (!active_invocation ||
                        completion.invocation_id !=
                            active_invocation->invocation_id ||
                        completion.attempt_id !=
                            active_invocation->attempt_id)
                    {
                        Publish(WorkerRuntimeDiagnosticEvent{
                            WorkerRejectionCode::InvocationMismatch,
                            "Ignored completion notification for a non-active invocation attempt",
                            completion.invocation_id});
                    }
                }},
            event);
        TakeFinishedExecution();
    }

    void HandleHostEvent(PendingHostEvent event)
    {
        const SessionSnapshot current = session
            ? session->snapshot()
            : SessionSnapshot{};
        const bool belongs_to_active_workset =
            current.open &&
            current.workset_epoch &&
            event.observed_session_id == current.session_id &&
            event.observed_workset_epoch == current.workset_epoch;

        if (event.name == "Host_JitCacheInvalidation" &&
            belongs_to_active_workset)
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
        else if (event.name == "Host_PPCBreakpointsChanged" &&
                 belongs_to_active_workset)
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
            event.observed_workset_epoch,
            std::move(event.name),
            std::move(event.encoded_payload)});
    }

    [[nodiscard]] std::uint64_t DrainAuthoritativeIngressToStable(
        bool expose_test_window)
    {
        for (;;)
        {
            const std::uint64_t observed_generation =
                mailbox->ingress_generation.load(std::memory_order_acquire);
            PumpExecutionEvents();

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
                    for (const auto& observation :
                         program_action_host
                             ->DrainForegroundSemanticStops())
                    {
                        PublishRuntimeSampleProgress(observation);
                    }
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
                            case ExecutionTerminalStatus::WorksetEpochMismatch:
                                code = WorkerRejectionCode::WorksetEpochMismatch;
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
        const std::optional<ExecutionSnapshot> execution = session
            ? session->execution_snapshot()
            : std::nullopt;
        if (!finishing_shutdown &&
            !pending_shutdown_commands.empty() &&
            !active_invocation && session &&
            (!execution || execution->activity ==
                ExecutionActivity::IdlePaused))
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

        // The actor queue is the cancellation/completion arbitration point. Once
        // an exact cancellation command has been accepted, a later successful
        // terminal cannot resurrect the invocation as completed.
        if (active_invocation->cancellation.is_cancellation_requested() &&
            terminal.status == InvocationTerminalStatus::Completed)
        {
            terminal.status = InvocationTerminalStatus::Cancelled;
            terminal.output_payload.clear();
        }

        bool capture_requires_taint = false;
        const RuntimeError capture_finalization =
            FinalizeActiveWorksetCapture(
                terminal,
                capture_requires_taint);
        if (capture_finalization)
        {
            if (terminal.error &&
                !terminal.error.message.empty())
            {
                terminal.diagnostics.push_back(
                    "Program terminal before capture failure: " +
                    terminal.error.message);
            }
            terminal.status = capture_requires_taint
                ? InvocationTerminalStatus::CleanupFailure
                : InvocationTerminalStatus::InfrastructureFailure;
            terminal.cleanup = capture_requires_taint
                ? CleanupStatus::Failed
                : CleanupStatus::CleanWithDiagnostics;
            terminal.error = capture_finalization;
        }

        const BackendResult derived_close =
            session->CloseDerivedStateItem();
        if (!derived_close.ok)
        {
            if (terminal.error && !terminal.error.message.empty())
            {
                terminal.diagnostics.push_back(
                    "Program terminal before derived-state cleanup failure: " +
                    terminal.error.message);
            }
            terminal.status = InvocationTerminalStatus::CleanupFailure;
            terminal.cleanup = CleanupStatus::Failed;
            terminal.session_disposition = SessionDisposition::Tainted;
            terminal.error = {
                WorkerRejectionCode::SessionTainted,
                derived_close.message.empty()
                    ? "Derived-state item cleanup did not preserve routing integrity"
                    : derived_close.message};
        }

        std::string taint_reason;
        if (terminal.workset_epoch !=
                 active_invocation->workset_epoch)
        {
            taint_reason =
                "ProgramRuntime returned an invocation completion for a stale WorksetEpoch";
            terminal.status = InvocationTerminalStatus::InfrastructureFailure;
            terminal.cleanup = CleanupStatus::Failed;
            terminal.error = {
                WorkerRejectionCode::WorksetEpochMismatch,
                taint_reason};
        }
        else if (terminal.cleanup == CleanupStatus::Failed ||
                 terminal.status == InvocationTerminalStatus::CleanupFailure ||
                 terminal.session_disposition == SessionDisposition::Tainted ||
                 terminal.session_disposition == SessionDisposition::Closed ||
                 session->snapshot().disposition ==
                     SessionDisposition::Tainted)
        {
            taint_reason = !terminal.error.message.empty()
                ? terminal.error.message
                : !session->taint_diagnostic().empty()
                ? session->taint_diagnostic()
                : "Invocation cleanup did not prove a clean session boundary";
            terminal.status = InvocationTerminalStatus::CleanupFailure;
            terminal.cleanup = CleanupStatus::Failed;
            if (!terminal.error)
            {
                terminal.error = {
                    WorkerRejectionCode::SessionTainted,
                    taint_reason};
            }
        }
        const bool invocation_succeeded =
            taint_reason.empty() &&
            terminal.status == InvocationTerminalStatus::Completed &&
            terminal.cleanup != CleanupStatus::Failed &&
            terminal.session_disposition != SessionDisposition::Tainted &&
            terminal.session_disposition != SessionDisposition::Closed;
        std::string output_transaction_failure;
        if (invocation_succeeded)
        {
            const auto decoded = program::DecodeProgramResultV1(
                terminal.output_payload);
            if (!decoded ||
                decoded.value->artifacts.size() +
                        active_invocation->outputs.outputs.size() >
                    active_invocation->outputs.maximum_artifacts)
            {
                output_transaction_failure = !decoded
                    ? (decoded.status.message.empty()
                           ? "ProgramResult draft could not be decoded before output finalization"
                           : decoded.status.message)
                    : "Combined program and staged outputs exceed the verified artifact allowance";
                (void)AbandonActiveOutputTransaction();
            }
            else
            {
                output_transaction_failure = FinalizeActiveOutputs();
            }
        }
        else
        {
            if (!AbandonActiveOutputTransaction())
            {
                taint_reason =
                    "Failed execution outputs could not be abandoned";
            }
        }

        if (!output_transaction_failure.empty())
        {
            terminal.status =
                InvocationTerminalStatus::InfrastructureFailure;
            terminal.output_payload.clear();
            terminal.error = {
                WorkerRejectionCode::BackendFailure,
                std::move(output_transaction_failure)};
        }
        else if (!active_invocation
                      ->artifact_finalization_failure.empty())
        {
            terminal.status =
                InvocationTerminalStatus::InfrastructureFailure;
            terminal.error = {
                WorkerRejectionCode::BackendFailure,
                active_invocation
                    ->artifact_finalization_failure};
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
        RefreshSnapshot();
        terminal.session_disposition = session->snapshot().disposition;
        if (terminal_workset && terminal_workset_item &&
            active_workset &&
            active_workset->definition.workset_id ==
                *terminal_workset &&
            terminal_workset_ordinal <
                active_workset->definition.items.size())
        {
            active_invocation->terminal_draft =
                std::move(terminal);
            (void)TryRetainActiveItemTerminal();
        }
        else
        {
            taint_reason =
                "A completed program execution had no active workset item";
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
            return;
        }
        // The active item retains the execution draft and its output
        // transaction until every finalizer completion has been observed.
        // Only TryRetainActiveItemTerminal may then construct and retain the
        // authoritative worker terminal.
        if (!active_invocation && !tainted_terminal)
        {
            if (!pending_shutdown_commands.empty() ||
                Snapshot().state == WorkerState::Stopping)
            {
                FinishShutdown(false);
            }
            else if (active_workset)
            {
                StartNextWorksetItem();
            }
        }
    }

    void EnterTainted(
        std::string diagnostic,
        bool synthesize_terminal = true,
        bool notify_program_runtime = true)
    {
        if (diagnostic.empty())
            diagnostic = "Session integrity could not be proven";
        if (taint_transition_active)
        {
            if (session)
                session->MarkTainted(diagnostic);
            Publish(WorkerRuntimeDiagnosticEvent{
                WorkerRejectionCode::SessionTainted,
                std::move(diagnostic),
                active_invocation
                    ? std::optional<InvocationId>(
                          active_invocation->invocation_id)
                    : std::nullopt});
            return;
        }
        taint_transition_active = true;
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
                active_invocation->workset_epoch,
                {},
                RuntimeError{
                    WorkerRejectionCode::SessionTainted,
                    diagnostic}};
        }

        if (active_invocation)
        {
            if (!SealActiveOutputTransactionForAbandon())
            {
                diagnostic +=
                    "; active output ownership could not be proven abandoned";
            }
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
            synthetic_terminal->error.message = diagnostic;
            active_invocation->terminal_draft =
                std::move(*synthetic_terminal);
            (void)TryRetainActiveItemTerminal();
            synthetic_terminal.reset();
        }
        if (active_invocation && artifact_finalizer &&
            active_invocation->outputs.state ==
                ActiveInvocation::OutputTransactionState::SealedForAbandon &&
            !active_invocation->artifact_finalizations.empty())
        {
            // A tainted worker cannot admit more output, but every accepted
            // finalizer job must reach a typed completion before the active
            // item and SavestateService ownership can be released.
            artifact_finalizer->Shutdown();
            DrainArtifactFinalizers();
            if (active_invocation)
            {
                if (!AbandonOutputsAfterFinalizerShutdown())
                {
                    diagnostic +=
                        "; stopped finalizer ownership could not be proven abandoned";
                }
                (void)TryRetainActiveItemTerminal();
            }
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
                    if (failed.prepared[ordinal].template_id)
                    {
                        (void)program_runtime
                            ->ReleaseInvocationTemplate(
                                failed.prepared[ordinal]
                                    .template_id);
                    }
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

        // WorksetStateCoordinator and its session cache retain references to
        // SavestateService. Tear them down while the session service composition
        // is still alive; EmulationSession::Shutdown destroys SavestateService.
        // Reset the coordinator even when cleanup reports a failure so its
        // destructor cannot revisit those borrowed references afterward.
        if (workset_state)
        {
            const ProgramBaselineComponentResult workset_shutdown =
                workset_state->Shutdown();
            if (!workset_shutdown.ok)
            {
                diagnostic += "; ";
                diagnostic += workset_shutdown.error.message.empty()
                    ? "workset state cleanup failed during session taint"
                    : workset_shutdown.error.message;
            }
            workset_state.reset();
        }

        if (session)
        {
            session->MarkTainted(diagnostic);
            (void)session->Shutdown();
        }

        if (SessionVisualMessageService* visual = session->visual_messages())
            (void)visual->SetIdle();
        active_invocation.reset();
        RefreshSnapshot();
        ChangeState(WorkerState::Tainted);
        terminal_publication_deferred = publication_was_deferred;
        if (!publication_was_deferred)
            (void)PublishReadyWorksetTerminals();
        if (synthetic_terminal)
        {
            Publish(WorkerRuntimeDiagnosticEvent{
                WorkerRejectionCode::SessionTainted,
                "An active invocation could not retain its authoritative workset terminal",
                synthetic_terminal->invocation_id});
        }
        Publish(WorkerRuntimeDiagnosticEvent{
            WorkerRejectionCode::SessionTainted,
            std::move(diagnostic),
            invocation});
        taint_transition_active = false;
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
                if (stopped.prepared[ordinal].template_id)
                {
                    (void)program_runtime
                        ->ReleaseInvocationTemplate(
                            stopped.prepared[ordinal].template_id);
                }
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
                retained_terminals.size(),
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

        if (SessionVisualMessageService* visual = session->visual_messages())
            (void)visual->SetIdle();
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
                    MailboxItemKind::ProgramActionResolution ||
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
                    ProjectExecutionSnapshot();
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
            current_snapshot.resident_workset_state = active_workset
                ? std::optional<WorkerWorksetState>(active_workset->state)
                : staged_workset
                ? std::optional<WorkerWorksetState>(staged_workset->state)
                : pending_workset_staging
                ? std::optional<WorkerWorksetState>(
                    WorkerWorksetState::Validating)
                : std::nullopt;
            current_snapshot.resident_cancellation_sidecar_sha256 =
                active_workset
                ? active_workset->cancellation_sidecar_sha256
                : staged_workset
                ? staged_workset->cancellation_sidecar_sha256
                : pending_workset_staging
                ? pending_workset_staging
                      ->cancellation_sidecar_sha256
                : std::string{};
            current_snapshot.active_workset_item =
                active_invocation &&
                    active_invocation->workset_item_id
                ? active_invocation->workset_item_id
                : std::nullopt;
            current_snapshot.available_item_credits =
                AvailableItemCredits();
            current_snapshot.retained_terminal_count =
                static_cast<std::uint32_t>(
                    retained_terminals.size());
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
            current_snapshot.execution = ProjectExecutionSnapshot();
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
        current_snapshot.resident_workset_state = active_workset
            ? std::optional<WorkerWorksetState>(active_workset->state)
            : staged_workset
            ? std::optional<WorkerWorksetState>(staged_workset->state)
            : pending_workset_staging
            ? std::optional<WorkerWorksetState>(
                WorkerWorksetState::Validating)
            : std::nullopt;
        current_snapshot.resident_cancellation_sidecar_sha256 =
            active_workset
            ? active_workset->cancellation_sidecar_sha256
            : staged_workset
            ? staged_workset->cancellation_sidecar_sha256
            : pending_workset_staging
            ? pending_workset_staging->cancellation_sidecar_sha256
            : std::string{};
        current_snapshot.active_workset_item =
            active_invocation &&
                active_invocation->workset_item_id
            ? active_invocation->workset_item_id
            : std::nullopt;
        current_snapshot.available_item_credits =
            AvailableItemCredits();
        current_snapshot.retained_terminal_count =
            static_cast<std::uint32_t>(
                retained_terminals.size());
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
        std::optional<ExecutionTerminalResult> execution_terminal = {},
        std::optional<SubmitWorksetResultV1> workset_submission = {})
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
        result.workset_submission = std::move(workset_submission);
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
        std::vector<SavestateArtifactFinalizationCompletion>
            completions =
                artifact_finalizer->DrainResults();
        if (completions.empty())
            return;
        for (SavestateArtifactFinalizationCompletion& completion :
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
            if (publication.abandon_requested)
            {
                publication.artifact.reset();
                const SavestateServiceResult abandoned =
                    session->AbandonImmutableSavestateArtifact(
                        publication.state_artifact_id);
                if (!abandoned.ok &&
                    abandoned.code != SavestateServiceErrorCode::NotFound)
                {
                    publication.failure = abandoned.message.empty()
                        ? "Abandoned output finalization retained service ownership"
                        : abandoned.message;
                }
            }
            else if (!completion.result.ok)
            {
                publication.failure =
                    completion.result.message.empty()
                    ? "State artifact finalization failed"
                    : completion.result.message;
                (void)session->AbandonImmutableSavestateArtifact(
                    publication.state_artifact_id);
            }
            else
            {
                ImmutableSavestateArtifactPublicationReceipt evidence;
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
                    (void)session->AbandonImmutableSavestateArtifact(
                        publication.state_artifact_id);
                }
                if (publication.failure.empty())
                {
                    const SavestateFileArtifactReceipt committed =
                        session->CommitImmutableSavestateArtifact(
                            evidence);
                    if (!committed.result.ok)
                    {
                        publication.failure =
                            committed.result.message.empty()
                            ? "SavestateService rejected finalized artifact evidence"
                            : committed.result.message;
                        (void)session
                            ->AbandonImmutableSavestateArtifact(
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
                                        SavestateSaveImmutableArtifact);
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
                        const SavestateServiceResult released =
                            session->ReleaseSavestateArtifact(
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
                }
            }
            if (active_invocation &&
                active_invocation->invocation_id ==
                    publication.item.invocation_id &&
                active_invocation->attempt_id ==
                    publication.item.attempt_id)
            {
                const auto output = std::ranges::find_if(
                    active_invocation->outputs.outputs,
                    [&](const ActiveInvocation::Output& candidate) {
                        return candidate.finalization_id ==
                            completion.finalization_id;
                    });
                if (output ==
                    active_invocation->outputs.outputs.end())
                {
                    EnterTainted(
                        "State artifact completion lost its active output transaction entry");
                    continue;
                }
                output->failure = publication.failure;
                output->artifact = publication.artifact;
                output->state = publication.failure.empty() &&
                        publication.artifact
                    ? ActiveInvocation::OutputState::Published
                    : ActiveInvocation::OutputState::Abandoned;
            }
            if (active_invocation &&
                active_invocation->invocation_id ==
                    publication.item.invocation_id &&
                active_invocation->attempt_id ==
                    publication.item.attempt_id)
            {
                (void)TryRetainActiveItemTerminal();
            }
        }
        RefreshSnapshot();
        PublishCredits();
        if (active_workset && !active_invocation)
            StartNextWorksetItem();
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
    std::unique_ptr<SavestateArtifactFinalizer>
        artifact_finalizer;
    std::unique_ptr<WorksetStateCoordinator> workset_state;
    bool program_runtime_shutdown = false;
    bool program_action_host_shutdown = false;
    WorkerMode worker_mode = WorkerMode::Headless;
    WorkerRuntimeContractV1 runtime_contract_value;

    mutable std::mutex snapshot_mutex;
    WorkerSnapshot current_snapshot;
    std::optional<ActiveInvocation> active_invocation;
    std::optional<WorksetPackage> active_workset;
    std::optional<WorksetPackage> staged_workset;
    std::optional<PendingWorksetStaging>
        pending_workset_staging;
    std::unordered_map<std::uint64_t, DrainingWorkset>
        draining_worksets;
    std::unordered_map<std::uint64_t, AcceptedWorksetSubmission>
        accepted_workset_submissions;
    std::unordered_map<std::uint64_t, RetainedTerminal>
        retained_terminals;
    std::unordered_map<std::uint64_t, ArtifactPublication>
        artifact_publications;
    std::size_t retained_terminal_bytes = 0;
    bool terminal_publication_deferred = false;
    bool handling_execution_finished = false;
    bool taint_transition_active = false;
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

WorkerRuntimeContractV1 WorkerRuntime::runtime_contract() const
{
    return impl_->RuntimeContract();
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
    WorksetEpoch observed_workset_epoch,
    std::string name,
    std::vector<std::uint8_t> encoded_payload)
{
    return impl_->EnqueueHostEvent(
        observed_session_id,
        observed_workset_epoch,
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
