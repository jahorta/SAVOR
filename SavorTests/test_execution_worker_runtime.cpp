#include <gtest/gtest.h>

#include "Runner/Runtime/EmulationSession.h"
#include "Runner/Runtime/IProgramRuntimePort.h"
#include "Runner/Runtime/WorkerRuntime.h"
#include "common/FakePhysicalStopBackend.h"
#include "common/ScriptedDolphinBackend.h"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace savor::runtime;
using savor::test_support::ScriptedDolphinBackend;
using savor::test_support::ScriptedDolphinBackendControl;
using savor::test_support::FakePhysicalStopBackend;
using savor::test_support::FakePhysicalStopBackendControl;

static_assert(!std::is_same_v<StateEpoch, WorkerCommandSequence>);
static_assert(!std::is_convertible_v<StateEpoch, WorkerCommandSequence>);
static_assert(!std::is_convertible_v<WorkerCommandSequence, StateEpoch>);
static_assert(!std::is_convertible_v<StateEpoch, std::uint64_t>);
static_assert(!std::is_convertible_v<std::uint64_t, StateEpoch>);

ExecutionRequestPolicy SessionExecutionPolicy(
    StateEpoch epoch,
    std::chrono::milliseconds timeout = 100ms)
{
    ExecutionRequestPolicy policy;
    policy.expected_epoch = epoch;
    policy.active_timeout = timeout;
    return policy;
}

StopSubscriptionGroupDefinition WorkerWakeGroup(std::uint32_t pc)
{
    return {
        .id = StopSubscriptionGroupId(900),
        .source = {
            .id = StopSourceId(900),
            .stable_name = "test.worker-runtime.wake",
            .diagnostic_label = "worker runtime ingress ordering",
        },
        .epoch_policy = StopEpochPolicy::EndOnEpochChange,
        .subscriptions = {{
            .id = StopSubscriptionId(900),
            .point = PcStopPointSpec{pc},
            .delivery = StopDeliveryMode::Wake,
            .policy = StopRoutingPolicy::Pass,
            .suppress_immediate_reentry = true,
        }},
    };
}

std::optional<ExecutionTerminalResult> DrainSessionExecution(
    EmulationSession& session,
    int maximum_pumps = 32)
{
    for (int pump = 0; pump < maximum_pumps; ++pump)
    {
        session.PumpExecution();
        for (ExecutionEvent& event : session.DrainExecutionEvents())
        {
            if (event.kind == ExecutionEventKind::Terminal &&
                event.terminal)
            {
                return std::move(event.terminal);
            }
        }
    }
    return std::nullopt;
}

struct FakeProgramRuntimeControl
{
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<IProgramRuntimeEventSink> event_sink;
    std::optional<ProgramInvocationRequest> last_invocation;
    CancellationToken last_token;
    int prepare_count = 0;
    int start_count = 0;
    int cancellation_count = 0;
    int shutdown_count = 0;
    bool throw_prepare = false;
    bool throw_start = false;
    ProgramRuntimeSubmission prepare_submission =
        ProgramRuntimeSubmission::Accepted();
    ProgramRuntimeSubmission start_submission =
        ProgramRuntimeSubmission::Accepted();
    ProgramRuntimeSubmission cancellation_submission =
        ProgramRuntimeSubmission::Accepted();

    [[nodiscard]] bool WaitForStarts(int expected)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return start_count >= expected; });
    }

    [[nodiscard]] bool WaitForCancellations(int expected)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(
            lock,
            5s,
            [&] { return cancellation_count >= expected; });
    }

    [[nodiscard]] bool WaitForShutdowns(int expected)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return shutdown_count >= expected; });
    }

    [[nodiscard]] int StartCount() const
    {
        std::lock_guard lock(mutex);
        return start_count;
    }

    [[nodiscard]] int ShutdownCount() const
    {
        std::lock_guard lock(mutex);
        return shutdown_count;
    }

    [[nodiscard]] int CancellationCount() const
    {
        std::lock_guard lock(mutex);
        return cancellation_count;
    }

    [[nodiscard]] CancellationToken LastToken() const
    {
        std::lock_guard lock(mutex);
        return last_token;
    }

    bool EmitTerminal(
        InvocationTerminalStatus status,
        CleanupStatus cleanup = CleanupStatus::Clean,
        SessionDisposition disposition = SessionDisposition::Clean,
        std::optional<StateEpoch> epoch_override = std::nullopt,
        RuntimeError error = {})
    {
        std::shared_ptr<IProgramRuntimeEventSink> sink;
        ProgramInvocationRequest invocation;
        {
            std::lock_guard lock(mutex);
            if (!event_sink || !last_invocation)
                return false;
            sink = event_sink;
            invocation = *last_invocation;
        }

        sink->Publish(ProgramInvocationTerminalEvent{
            invocation.invocation.invocation_id,
            invocation.invocation.attempt_id,
            status,
            cleanup,
            disposition,
            epoch_override.value_or(
                invocation.invocation.expected_state_epoch),
            {},
            std::move(error)});
        return true;
    }

    bool EmitProgress(
        std::uint64_t sequence,
        std::string text,
        std::optional<AttemptId> attempt_override = std::nullopt)
    {
        std::shared_ptr<IProgramRuntimeEventSink> sink;
        InvocationId invocation;
        AttemptId attempt;
        {
            std::lock_guard lock(mutex);
            if (!event_sink || !last_invocation)
                return false;
            sink = event_sink;
            invocation = last_invocation->invocation.invocation_id;
            attempt = last_invocation->invocation.attempt_id;
        }
        sink->Publish(ProgramInvocationProgressEvent{
            invocation,
            attempt_override.value_or(attempt),
            sequence,
            std::move(text),
            true});
        return true;
    }
};

class FakeProgramRuntimePort final : public IProgramRuntimePort
{
public:
    explicit FakeProgramRuntimePort(
        std::shared_ptr<FakeProgramRuntimeControl> control)
        : control_(std::move(control))
    {
    }

    WorkerCapabilityMask capabilities() const noexcept override
    {
        return CapabilityMask(WorkerCapability::ProgramInvocation);
    }

    ProgramRuntimeSubmission PrepareModule(
        ModulePreparationRequest request,
        std::shared_ptr<IProgramRuntimeEventSink> events) override
    {
        bool should_throw = false;
        ProgramRuntimeSubmission submission;
        {
            std::lock_guard lock(control_->mutex);
            ++control_->prepare_count;
            control_->event_sink = std::move(events);
            should_throw = control_->throw_prepare;
            submission = control_->prepare_submission;
            control_->changed.notify_all();
        }
        if (should_throw)
            throw std::runtime_error("fake prepare failure after admission");
        if (submission.accepted)
        {
            control_->event_sink->Publish(ModulePreparationEvent{
                request.command_sequence,
                std::move(request.module.identity),
                true,
                {}});
        }
        return submission;
    }

    ProgramRuntimeSubmission StartInvocation(
        ProgramInvocationRequest request,
        CancellationToken cancellation,
        std::shared_ptr<IProgramRuntimeEventSink> events) override
    {
        bool should_throw = false;
        ProgramRuntimeSubmission submission;
        {
            std::lock_guard lock(control_->mutex);
            ++control_->start_count;
            control_->last_invocation = request;
            control_->last_token = std::move(cancellation);
            control_->event_sink = std::move(events);
            should_throw = control_->throw_start;
            submission = control_->start_submission;
            control_->changed.notify_all();
        }
        if (should_throw)
            throw std::runtime_error("fake invocation failure after admission");
        return submission;
    }

    ProgramRuntimeSubmission RequestCancellation(
        InvocationId) override
    {
        std::lock_guard lock(control_->mutex);
        ++control_->cancellation_count;
        control_->changed.notify_all();
        return control_->cancellation_submission;
    }

    void Shutdown() noexcept override
    {
        std::lock_guard lock(control_->mutex);
        ++control_->shutdown_count;
        control_->changed.notify_all();
    }

private:
    std::shared_ptr<FakeProgramRuntimeControl> control_;
};

class WorkerEventLog
{
public:
    void Record(const WorkerEvent& event)
    {
        std::lock_guard lock(mutex_);
        events_.push_back(event);
        changed_.notify_all();
    }

    [[nodiscard]] bool WaitForTerminalCount(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 5s, [&] {
            return TerminalCountLocked() >= count;
        });
    }

    [[nodiscard]] bool WaitForCommandCount(
        WorkerCommandKind kind,
        std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 5s, [&] {
            return CommandResultsLocked(kind).size() >= count;
        });
    }

    [[nodiscard]] bool WaitForExecutionTerminalCount(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 5s, [&] {
            return ExecutionTerminalCountLocked() >= count;
        });
    }

    [[nodiscard]] std::size_t TerminalCount() const
    {
        std::lock_guard lock(mutex_);
        return TerminalCountLocked();
    }

    [[nodiscard]] std::vector<ProgramInvocationTerminalEvent> Terminals() const
    {
        std::lock_guard lock(mutex_);
        std::vector<ProgramInvocationTerminalEvent> terminals;
        for (const WorkerEvent& event : events_)
        {
            if (const auto* terminal =
                    std::get_if<ProgramInvocationTerminalEvent>(&event))
            {
                terminals.push_back(*terminal);
            }
        }
        return terminals;
    }

    [[nodiscard]] std::vector<ProgramInvocationProgressEvent> Progress() const
    {
        std::lock_guard lock(mutex_);
        std::vector<ProgramInvocationProgressEvent> progress;
        for (const WorkerEvent& event : events_)
        {
            if (const auto* update =
                    std::get_if<ProgramInvocationProgressEvent>(&event))
            {
                progress.push_back(*update);
            }
        }
        return progress;
    }

    [[nodiscard]] std::vector<WorkerCommandResult> CommandResults(
        WorkerCommandKind kind) const
    {
        std::lock_guard lock(mutex_);
        return CommandResultsLocked(kind);
    }

    [[nodiscard]] std::vector<WorkerEvent> Events() const
    {
        std::lock_guard lock(mutex_);
        return events_;
    }

private:
    [[nodiscard]] std::size_t TerminalCountLocked() const
    {
        return static_cast<std::size_t>(std::count_if(
            events_.begin(),
            events_.end(),
            [](const WorkerEvent& event) {
                return std::holds_alternative<
                    ProgramInvocationTerminalEvent>(event);
            }));
    }

    [[nodiscard]] std::size_t ExecutionTerminalCountLocked() const
    {
        return static_cast<std::size_t>(std::count_if(
            events_.begin(),
            events_.end(),
            [](const WorkerEvent& event) {
                const auto* execution =
                    std::get_if<WorkerExecutionEvent>(&event);
                return execution && execution->event.terminal.has_value();
            }));
    }

    [[nodiscard]] std::vector<WorkerCommandResult> CommandResultsLocked(
        WorkerCommandKind kind) const
    {
        std::vector<WorkerCommandResult> results;
        for (const WorkerEvent& event : events_)
        {
            if (const auto* completed =
                    std::get_if<WorkerCommandCompletedEvent>(&event);
                completed && completed->result.command_kind == kind)
            {
                results.push_back(completed->result);
            }
        }
        return results;
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<WorkerEvent> events_;
};

struct RuntimeHarness
{
    SessionId session_id{77};
    std::shared_ptr<ScriptedDolphinBackendControl> backend =
        std::make_shared<ScriptedDolphinBackendControl>();
    std::shared_ptr<FakeProgramRuntimeControl> program =
        std::make_shared<FakeProgramRuntimeControl>();
    WorkerEventLog events;
    std::unique_ptr<WorkerRuntime> runtime;
    std::uint64_t next_request = 1;

    explicit RuntimeHarness(
        std::unique_ptr<IPhysicalStopPointBackendPort> physical_stop_points = {},
        std::shared_ptr<const WorkerRuntimeTestHooks> test_hooks = {})
    {
        runtime = std::make_unique<WorkerRuntime>(
            std::make_unique<EmulationSession>(
                session_id,
                std::make_unique<ScriptedDolphinBackend>(
                    backend,
                    std::move(physical_stop_points))),
            std::make_unique<FakeProgramRuntimePort>(program),
            [this](const WorkerEvent& event) { events.Record(event); },
            std::move(test_hooks));
    }

    [[nodiscard]] WireRequestId NextRequest()
    {
        return WireRequestId(next_request++);
    }

    [[nodiscard]] SessionOpenOptions OpenOptions() const
    {
        SessionOpenOptions options;
        options.backend.runtime_root = "fake-runtime";
        options.backend.user_directory = "fake-user";
        options.backend.dolphin_base_directory = "fake-dolphin";
        options.backend.iso_path = "fake.iso";
        return options;
    }

    [[nodiscard]] WorkerCommandResult Open(
        std::optional<SessionOpenOptions> options = std::nullopt)
    {
        return runtime->Submit(
            NextRequest(),
            OpenSessionCommand{options.value_or(OpenOptions())}).get();
    }

    [[nodiscard]] EncodedInvocationEnvelope Invocation(
        std::uint64_t invocation_id,
        std::uint64_t attempt_id = 1) const
    {
        EncodedInvocationEnvelope invocation;
        invocation.invocation_id = InvocationId(invocation_id);
        invocation.attempt_id = AttemptId(attempt_id);
        invocation.module.canonical_id = "test.module/1";
        invocation.module.revision = 1;
        invocation.module.canonical_hash = "test-hash";
        invocation.entrypoint = "main";
        invocation.expected_state_epoch =
            runtime->snapshot().session.state_epoch;
        invocation.input_payload = {0x01, 0x02};
        return invocation;
    }

    [[nodiscard]] WorkerCommandResult Invoke(
        std::uint64_t invocation_id,
        std::uint64_t attempt_id = 1)
    {
        return runtime->Submit(
            NextRequest(),
            InvokeProgramCommand{
                Invocation(invocation_id, attempt_id)}).get();
    }

    [[nodiscard]] WorkerCommandResult Shutdown()
    {
        auto future = runtime->Submit(NextRequest(), ShutdownCommand{});
        WorkerCommandResult result = future.get();
        runtime->WaitStopped();
        return result;
    }
};

TEST(EmulationSession, BootFailureDoesNotAdvanceEpochAndShutdownIsIdempotent)
{
    auto control = std::make_shared<ScriptedDolphinBackendControl>();
    control->SetOpenResult(
        BackendResult::Failure(
            BackendErrorCode::BootFailed,
            "expected boot failure"),
        BackendCoreState::Closed);
    EmulationSession session(
        SessionId(1),
        std::make_unique<ScriptedDolphinBackend>(control));

    SessionOpenOptions options;
    const SessionOperationReceipt open = session.Open(options);
    EXPECT_FALSE(open.ok);
    EXPECT_EQ(open.resulting_epoch, StateEpoch{});
    EXPECT_EQ(open.disposition, SessionDisposition::Closed);

    const SessionOperationReceipt first_shutdown = session.Shutdown();
    const SessionOperationReceipt second_shutdown = session.Shutdown();
    EXPECT_TRUE(first_shutdown.ok);
    EXPECT_EQ(first_shutdown.ok, second_shutdown.ok);
    EXPECT_EQ(first_shutdown.disposition, second_shutdown.disposition);
    EXPECT_EQ(first_shutdown.backend.code, second_shutdown.backend.code);
    EXPECT_EQ(control->OpenCount(), 1);
    EXPECT_EQ(control->CloseCount(), 1);
}

TEST(EmulationSession, EpochAdvancesOnlyForSuccessfulStateReplacement)
{
    auto control = std::make_shared<ScriptedDolphinBackendControl>();
    EmulationSession session(
        SessionId(2),
        std::make_unique<ScriptedDolphinBackend>(control));

    const SessionOperationReceipt open = session.Open({});
    ASSERT_TRUE(open.ok);
    EXPECT_EQ(open.origin_epoch, StateEpoch{});
    EXPECT_EQ(open.resulting_epoch, StateEpoch{1});
    EXPECT_EQ(session.snapshot().core_state, BackendCoreState::Paused);

    const ExecutionSubmissionReceipt instruction =
        session.SubmitExecution(StepInstructionsRequest{
            .policy = SessionExecutionPolicy(StateEpoch{1}),
            .count = 1,
        });
    ASSERT_TRUE(instruction.accepted) << instruction.error.message;
    const auto instruction_terminal = DrainSessionExecution(session);
    ASSERT_TRUE(instruction_terminal.has_value());
    EXPECT_EQ(
        instruction_terminal->status,
        ExecutionTerminalStatus::StepsCompleted);

    const ExecutionSubmissionReceipt frame =
        session.SubmitExecution(StepFramesRequest{
            .policy = SessionExecutionPolicy(StateEpoch{1}),
            .count = 1,
        });
    ASSERT_TRUE(frame.accepted) << frame.error.message;
    const auto frame_terminal = DrainSessionExecution(session);
    ASSERT_TRUE(frame_terminal.has_value());
    EXPECT_EQ(
        frame_terminal->status,
        ExecutionTerminalStatus::StepsCompleted);
    EXPECT_TRUE(session.SaveStateFile("saved.state").ok);
    const SessionBufferReceipt saved = session.SaveStateBuffer();
    EXPECT_TRUE(saved.operation.ok);
    EXPECT_EQ(saved.bytes, (std::vector<std::uint8_t>{0x10, 0x20, 0x30}));
    EXPECT_TRUE(session.CaptureScreenshot("shot.png", 100ms).ok);
    EXPECT_EQ(session.snapshot().state_epoch, StateEpoch{1});

    EXPECT_TRUE(session.Reboot().ok);
    EXPECT_EQ(session.snapshot().state_epoch, StateEpoch{2});
    EXPECT_TRUE(session.RestoreStateFile("input.state").ok);
    EXPECT_EQ(session.snapshot().state_epoch, StateEpoch{3});
    EXPECT_TRUE(session.RestoreStateBuffer({0x01, 0x02}).ok);
    EXPECT_EQ(session.snapshot().state_epoch, StateEpoch{4});

    control->SetRestoreFileResult(BackendResult::Failure(
        BackendErrorCode::OperationFailed,
        "preserved failure",
        BackendIntegrity::Preserved));
    const SessionOperationReceipt failed =
        session.RestoreStateFile("failed.state");
    EXPECT_FALSE(failed.ok);
    EXPECT_EQ(failed.origin_epoch, StateEpoch{4});
    EXPECT_EQ(failed.resulting_epoch, StateEpoch{4});
    EXPECT_EQ(failed.disposition, SessionDisposition::Clean);

    EXPECT_TRUE(session.Shutdown().ok);
    EXPECT_EQ(control->CloseCount(), 1);
}

TEST(EmulationSession, UnknownIntegrityAndStoppedCoreTaintWithoutAdvancingEpoch)
{
    {
        auto control = std::make_shared<ScriptedDolphinBackendControl>();
        control->SetOpenResult(
            BackendResult::Success(),
            BackendCoreState::Stopped);
        EmulationSession session(
            SessionId(3),
            std::make_unique<ScriptedDolphinBackend>(control));

        const SessionOperationReceipt open = session.Open({});
        EXPECT_FALSE(open.ok);
        EXPECT_EQ(open.resulting_epoch, StateEpoch{});
        EXPECT_EQ(open.disposition, SessionDisposition::Tainted);
        EXPECT_TRUE(session.Shutdown().ok);
    }

    auto control = std::make_shared<ScriptedDolphinBackendControl>();
    EmulationSession session(
        SessionId(4),
        std::make_unique<ScriptedDolphinBackend>(control));
    ASSERT_TRUE(session.Open({}).ok);

    control->SetStepInstructionResult(BackendResult::Failure(
        BackendErrorCode::Timeout,
        "step completion is uncertain",
        BackendIntegrity::Unknown));
    const ExecutionSubmissionReceipt step =
        session.SubmitExecution(StepInstructionsRequest{
            .policy = SessionExecutionPolicy(StateEpoch{1}),
            .count = 1,
        });
    ASSERT_TRUE(step.accepted) << step.error.message;
    const auto step_terminal = DrainSessionExecution(session);
    ASSERT_TRUE(step_terminal.has_value());
    EXPECT_EQ(
        step_terminal->status,
        ExecutionTerminalStatus::BackendFailure);
    EXPECT_EQ(
        step_terminal->integrity,
        BackendIntegrity::Unknown);
    EXPECT_EQ(
        session.snapshot().state_epoch,
        StateEpoch{1});
    EXPECT_EQ(
        session.snapshot().disposition,
        SessionDisposition::Tainted);
    EXPECT_FALSE(session.CaptureScreenshot("after-taint.png", 100ms).ok);
    EXPECT_TRUE(session.Shutdown().ok);
}

TEST(EmulationSession, RejectsOffOwnerCallsBeforeBackendMutation)
{
    auto control = std::make_shared<ScriptedDolphinBackendControl>();
    EmulationSession session(
        SessionId(5),
        std::make_unique<ScriptedDolphinBackend>(control));
    ASSERT_TRUE(session.Open({}).ok);

    std::promise<SessionOperationReceipt> attempted;
    std::thread other([&] {
        attempted.set_value(
            session.CaptureScreenshot("wrong-thread.png", 100ms));
    });
    other.join();

    const SessionOperationReceipt receipt = attempted.get_future().get();
    EXPECT_FALSE(receipt.ok);
    EXPECT_EQ(receipt.backend.code, BackendErrorCode::InvalidState);
    EXPECT_EQ(control->ScreenshotCount(), 0);
    EXPECT_TRUE(session.Shutdown().ok);
    EXPECT_FALSE(control->HasOwnerViolation());
}

TEST(EmulationSession, StateEpochIsNotACommandOrDispatchEpoch)
{
    const StateEpoch state_epoch{9};
    const WorkerCommandSequence command_sequence{9};
    const std::uint64_t dispatch_epoch = 9;

    EXPECT_EQ(state_epoch.value(), dispatch_epoch);
    EXPECT_EQ(command_sequence.value(), dispatch_epoch);
    EXPECT_FALSE((std::is_same_v<StateEpoch, WorkerCommandSequence>));
}

TEST(ExecutionWorkerRuntime, SerializesConcurrentProducersOnOneBackendOwner)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);

    constexpr std::size_t producer_count = 8;
    std::barrier start(static_cast<std::ptrdiff_t>(producer_count + 1));
    std::vector<std::thread> producers;
    std::vector<std::thread::id> producer_threads(producer_count);
    std::vector<WorkerCommandResult> results(producer_count);
    producers.reserve(producer_count);

    for (std::size_t index = 0; index < producer_count; ++index)
    {
        producers.emplace_back([&, index] {
            producer_threads[index] = std::this_thread::get_id();
            start.arrive_and_wait();
            results[index] = harness.runtime->Submit(
                WireRequestId(100 + index),
                CaptureScreenshotCommand{
                    harness.session_id,
                    std::filesystem::path(
                        "producer-" + std::to_string(index) + ".png"),
                    100ms}).get();
        });
    }
    start.arrive_and_wait();
    for (std::thread& producer : producers)
        producer.join();

    std::vector<std::uint64_t> sequences;
    for (const WorkerCommandResult& result : results)
    {
        EXPECT_EQ(result.outcome, WorkerCommandOutcome::Completed);
        sequences.push_back(result.command_sequence.value());
    }
    std::sort(sequences.begin(), sequences.end());
    ASSERT_EQ(sequences.size(), producer_count);
    for (std::size_t index = 1; index < sequences.size(); ++index)
        EXPECT_EQ(sequences[index], sequences[index - 1] + 1);

    ASSERT_TRUE(harness.events.WaitForCommandCount(
        WorkerCommandKind::CaptureScreenshot,
        producer_count));
    const auto completion_events = harness.events.CommandResults(
        WorkerCommandKind::CaptureScreenshot);
    ASSERT_EQ(completion_events.size(), producer_count);
    EXPECT_TRUE(std::is_sorted(
        completion_events.begin(),
        completion_events.end(),
        [](const WorkerCommandResult& lhs, const WorkerCommandResult& rhs) {
            return lhs.command_sequence < rhs.command_sequence;
        }));

    EXPECT_EQ(harness.backend->ScreenshotCount(), producer_count);
    EXPECT_FALSE(harness.backend->HasOwnerViolation());
    const auto owner = harness.backend->OwnerThread();
    ASSERT_TRUE(owner.has_value());
    for (const std::thread::id producer : producer_threads)
        EXPECT_NE(*owner, producer);

    EXPECT_EQ(harness.Shutdown().outcome, WorkerCommandOutcome::Completed);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
}

TEST(ExecutionWorkerRuntime, EnforcesOneSessionAndOneActiveInvocation)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);

    const WorkerCommandResult second_open = harness.runtime->Submit(
        harness.NextRequest(),
        OpenSessionCommand{harness.OpenOptions()}).get();
    EXPECT_EQ(second_open.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(second_open.error.code, WorkerRejectionCode::InvalidState);
    EXPECT_EQ(harness.backend->OpenCount(), 1);

    const WorkerCommandResult first = harness.Invoke(101);
    ASSERT_EQ(first.outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    const WorkerCommandResult second = harness.Invoke(102);
    EXPECT_EQ(second.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(
        second.error.code,
        WorkerRejectionCode::InvocationAlreadyActive);
    EXPECT_EQ(harness.program->StartCount(), 1);

    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));
    const WorkerCommandResult ready_barrier = harness.runtime->Submit(
        harness.NextRequest(),
        CaptureScreenshotCommand{
            harness.session_id,
            "ready-state-barrier.png",
            100ms}).get();
    ASSERT_EQ(ready_barrier.outcome, WorkerCommandOutcome::Completed);
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Ready);
    EXPECT_EQ(harness.events.TerminalCount(), 1);

    EXPECT_EQ(harness.Shutdown().outcome, WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    RejectsProgressFromAStaleAttemptWhenInvocationIdIsReused)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    ASSERT_EQ(harness.Invoke(151, 1).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));
    const WorkerCommandResult first_barrier = harness.runtime->Submit(
        harness.NextRequest(),
        CaptureScreenshotCommand{
            harness.session_id,
            "first-attempt-terminal-barrier.png",
            100ms}).get();
    ASSERT_EQ(first_barrier.outcome, WorkerCommandOutcome::Completed);

    ASSERT_EQ(harness.Invoke(151, 2).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->EmitProgress(
        1,
        "late-attempt-one-progress",
        AttemptId(1)));
    ASSERT_TRUE(harness.program->EmitProgress(
        2,
        "current-attempt-two-progress"));
    const WorkerCommandResult progress_barrier = harness.runtime->Submit(
        harness.NextRequest(),
        CaptureScreenshotCommand{
            harness.session_id,
            "progress-attempt-barrier.png",
            100ms}).get();
    ASSERT_EQ(progress_barrier.outcome, WorkerCommandOutcome::Completed);

    const auto progress = harness.events.Progress();
    ASSERT_EQ(progress.size(), 1);
    EXPECT_EQ(progress.front().invocation_id, InvocationId(151));
    EXPECT_EQ(progress.front().attempt_id, AttemptId(2));
    EXPECT_EQ(progress.front().progress_sequence, 2);
    EXPECT_EQ(progress.front().text, "current-attempt-two-progress");

    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    ASSERT_TRUE(harness.events.WaitForTerminalCount(2));
    EXPECT_EQ(harness.Shutdown().outcome, WorkerCommandOutcome::Completed);
}

TEST(ExecutionWorkerRuntime, CompletionWinningCancelRaceHasOneTerminal)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    ASSERT_EQ(harness.Invoke(201).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    const WorkerCommandResult cancel = harness.runtime->Submit(
        harness.NextRequest(),
        CancelInvocationCommand{InvocationId(201)}).get();
    EXPECT_EQ(cancel.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(cancel.error.code, WorkerRejectionCode::InvocationNotActive);
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));

    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    const WorkerCommandResult barrier = harness.runtime->Submit(
        harness.NextRequest(),
        CaptureScreenshotCommand{
            harness.session_id,
            "duplicate-terminal-barrier.png",
            100ms}).get();
    ASSERT_EQ(barrier.outcome, WorkerCommandOutcome::Completed);
    EXPECT_EQ(harness.events.TerminalCount(), 1);

    EXPECT_EQ(harness.Shutdown().outcome, WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    CancelWinningRaceNormalizesLaterCompletionAndRejectsDuplicateMismatchAndStale)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    ASSERT_EQ(harness.Invoke(301, 9).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    const WorkerCommandResult cancel = harness.runtime->Submit(
        harness.NextRequest(),
        CancelInvocationCommand{InvocationId(301)}).get();
    EXPECT_EQ(cancel.outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForCancellations(1));
    EXPECT_TRUE(harness.program->LastToken().is_cancellation_requested());
    EXPECT_EQ(
        harness.program->LastToken().reason(),
        CancellationReason::ExternalRequest);

    const WorkerCommandResult duplicate = harness.runtime->Submit(
        harness.NextRequest(),
        CancelInvocationCommand{InvocationId(301)}).get();
    EXPECT_EQ(duplicate.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(
        duplicate.error.code,
        WorkerRejectionCode::DuplicateCancellation);

    const WorkerCommandResult mismatch = harness.runtime->Submit(
        harness.NextRequest(),
        CancelInvocationCommand{InvocationId(999)}).get();
    EXPECT_EQ(mismatch.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(mismatch.error.code, WorkerRejectionCode::InvocationMismatch);

    // The fake deliberately reports success after the actor has accepted the
    // exact cancellation. Actor queue order owns the race and normalizes it.
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));
    const auto terminals = harness.events.Terminals();
    ASSERT_EQ(terminals.size(), 1);
    EXPECT_EQ(
        terminals.front().status,
        InvocationTerminalStatus::Cancelled);

    const WorkerCommandResult stale = harness.runtime->Submit(
        harness.NextRequest(),
        CancelInvocationCommand{InvocationId(301)}).get();
    EXPECT_EQ(stale.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(stale.error.code, WorkerRejectionCode::InvocationNotActive);
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Ready);
    EXPECT_EQ(harness.events.TerminalCount(), 1);

    EXPECT_EQ(harness.Shutdown().outcome, WorkerCommandOutcome::Completed);
}

TEST(
    ExecutionWorkerRuntime,
    CancellationDeliveryFailureTaintsAndSynthesizesExactlyOneTerminal)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    ASSERT_EQ(harness.Invoke(351).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->cancellation_submission =
            ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InternalFailure,
                "injected cancellation delivery failure");
    }

    const WorkerCommandResult cancel = harness.runtime->Submit(
        harness.NextRequest(),
        CancelInvocationCommand{InvocationId(351)}).get();

    EXPECT_EQ(cancel.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(cancel.error.code, WorkerRejectionCode::InternalFailure);
    EXPECT_EQ(harness.program->CancellationCount(), 1);
    EXPECT_EQ(harness.program->ShutdownCount(), 1);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Tainted);
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));

    const auto terminals = harness.events.Terminals();
    ASSERT_EQ(terminals.size(), 1);
    EXPECT_EQ(
        terminals.front().status,
        InvocationTerminalStatus::InfrastructureFailure);
    EXPECT_EQ(terminals.front().cleanup, CleanupStatus::Failed);
    EXPECT_EQ(
        terminals.front().session_disposition,
        SessionDisposition::Tainted);

    // Retired ingress cannot publish a second terminal after the fault.
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    const WorkerCommandResult shutdown = harness.Shutdown();
    EXPECT_EQ(shutdown.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Stopped);
    EXPECT_EQ(harness.events.TerminalCount(), 1);
    EXPECT_EQ(harness.program->CancellationCount(), 1);
    EXPECT_EQ(harness.program->ShutdownCount(), 1);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
}

TEST(
    ExecutionWorkerRuntime,
    ShutdownCancellationDeliveryFailureStopsDeterministically)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    ASSERT_EQ(harness.Invoke(352).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->cancellation_submission =
            ProgramRuntimeSubmission::Rejected(
                WorkerRejectionCode::InternalFailure,
                "injected shutdown cancellation failure");
    }

    auto shutdown = harness.runtime->Submit(
        harness.NextRequest(),
        ShutdownCommand{});
    ASSERT_EQ(shutdown.wait_for(5s), std::future_status::ready);
    const WorkerCommandResult result = shutdown.get();
    harness.runtime->WaitStopped();

    EXPECT_EQ(result.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(result.error.code, WorkerRejectionCode::SessionTainted);
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Stopped);
    EXPECT_EQ(harness.program->CancellationCount(), 1);
    EXPECT_EQ(harness.program->ShutdownCount(), 1);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));
    const auto terminals = harness.events.Terminals();
    ASSERT_EQ(terminals.size(), 1);
    EXPECT_EQ(
        terminals.front().status,
        InvocationTerminalStatus::InfrastructureFailure);
    EXPECT_EQ(terminals.front().cleanup, CleanupStatus::Failed);
    EXPECT_EQ(
        terminals.front().session_disposition,
        SessionDisposition::Tainted);
}

TEST(ExecutionWorkerRuntime, ShutdownWinningRaceWaitsForTerminalAndClosesOnce)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    ASSERT_EQ(harness.Invoke(401).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    auto shutdown = harness.runtime->Submit(
        harness.NextRequest(),
        ShutdownCommand{});
    ASSERT_TRUE(harness.program->WaitForCancellations(1));
    EXPECT_EQ(shutdown.wait_for(0ms), std::future_status::timeout);

    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Cancelled));
    const WorkerCommandResult result = shutdown.get();
    harness.runtime->WaitStopped();

    EXPECT_EQ(result.outcome, WorkerCommandOutcome::Completed);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
    EXPECT_EQ(harness.program->ShutdownCount(), 1);
    EXPECT_EQ(harness.events.TerminalCount(), 1);
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Stopped);
}

TEST(ExecutionWorkerRuntime, CompletionWinningShutdownRaceStillClosesCleanly)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    ASSERT_EQ(harness.Invoke(402).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    const WorkerCommandResult shutdown = harness.Shutdown();

    EXPECT_EQ(shutdown.outcome, WorkerCommandOutcome::Completed);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
    EXPECT_EQ(harness.program->ShutdownCount(), 1);
    EXPECT_EQ(harness.events.TerminalCount(), 1);
}

TEST(ExecutionWorkerRuntime, CleanDiagnosticsReuseButTaintRejectsFurtherWork)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);

    ASSERT_EQ(harness.Invoke(501).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed,
        CleanupStatus::CleanWithDiagnostics,
        SessionDisposition::CleanWithDiagnostics));
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));

    ASSERT_EQ(harness.Invoke(502).outcome, WorkerCommandOutcome::Accepted);
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Running);
    EXPECT_EQ(
        harness.runtime->snapshot().session.disposition,
        SessionDisposition::CleanWithDiagnostics);
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed,
        CleanupStatus::Failed,
        SessionDisposition::Tainted,
        std::nullopt,
        RuntimeError{
            WorkerRejectionCode::SessionTainted,
            "cleanup proof failed"}));
    ASSERT_TRUE(harness.events.WaitForTerminalCount(2));
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Tainted);
    EXPECT_EQ(
        harness.runtime->snapshot().session.disposition,
        SessionDisposition::Tainted);
    EXPECT_EQ(harness.backend->CloseCount(), 1);

    const WorkerCommandResult rejected = harness.Invoke(503);
    EXPECT_EQ(rejected.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(rejected.error.code, WorkerRejectionCode::SessionTainted);

    const WorkerCommandResult shutdown = harness.Shutdown();
    EXPECT_EQ(shutdown.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(shutdown.error.code, WorkerRejectionCode::SessionTainted);

    const auto terminals = harness.events.Terminals();
    ASSERT_EQ(terminals.size(), 2);
    EXPECT_EQ(
        terminals.back().status,
        InvocationTerminalStatus::CleanupFailure);
}

TEST(ExecutionWorkerRuntime, ScreenshotRequiresValidStateAndExactSession)
{
    RuntimeHarness harness;

    const WorkerCommandResult before_open = harness.runtime->Submit(
        harness.NextRequest(),
        CaptureScreenshotCommand{
            harness.session_id,
            "before-open.png",
            100ms}).get();
    EXPECT_EQ(before_open.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(before_open.error.code, WorkerRejectionCode::InvalidState);
    EXPECT_EQ(harness.backend->ScreenshotCount(), 0);

    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    const WorkerCommandResult mismatch = harness.runtime->Submit(
        harness.NextRequest(),
        CaptureScreenshotCommand{
            SessionId(999),
            "wrong-session.png",
            100ms}).get();
    EXPECT_EQ(mismatch.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(mismatch.error.code, WorkerRejectionCode::SessionMismatch);
    EXPECT_EQ(harness.backend->ScreenshotCount(), 0);

    const WorkerCommandResult ready = harness.runtime->Submit(
        harness.NextRequest(),
        CaptureScreenshotCommand{
            harness.session_id,
            "ready.png",
            100ms}).get();
    EXPECT_EQ(ready.outcome, WorkerCommandOutcome::Completed);
    EXPECT_EQ(harness.backend->ScreenshotCount(), 1);

    ASSERT_EQ(harness.Invoke(601).outcome, WorkerCommandOutcome::Accepted);
    const WorkerCommandResult running = harness.runtime->Submit(
        harness.NextRequest(),
        CaptureScreenshotCommand{
            harness.session_id,
            "running.png",
            100ms}).get();
    EXPECT_EQ(running.outcome, WorkerCommandOutcome::Completed);
    EXPECT_EQ(harness.backend->ScreenshotCount(), 2);

    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));
    EXPECT_EQ(harness.Shutdown().outcome, WorkerCommandOutcome::Completed);
}

TEST(ExecutionWorkerRuntime, ReadyExecutionControlsRequireVisualSessionAndExactEpoch)
{
    {
        RuntimeHarness headless;
        ASSERT_EQ(
            headless.Open().outcome,
            WorkerCommandOutcome::Completed);
        EXPECT_TRUE(HasCapability(
            headless.runtime->capabilities(),
            WorkerCapability::InteractiveVisualDebug));
        const WorkerCommandResult rejected = headless.runtime->Submit(
            headless.NextRequest(),
            ControlExecutionCommand{
                .control = WorkerExecutionControlKind::Pause,
                .session_id = headless.session_id,
                .expected_state_epoch =
                    headless.runtime->snapshot().session.state_epoch,
                .timeout = 100ms,
            }).get();
        EXPECT_EQ(rejected.outcome, WorkerCommandOutcome::Rejected);
        EXPECT_EQ(rejected.error.code, WorkerRejectionCode::Unsupported);
        EXPECT_EQ(
            headless.Shutdown().outcome,
            WorkerCommandOutcome::Completed);
    }

    RuntimeHarness visual;
    SessionOpenOptions options = visual.OpenOptions();
    options.backend.visual = true;
    ASSERT_EQ(
        visual.Open(std::move(options)).outcome,
        WorkerCommandOutcome::Completed);
    const StateEpoch epoch =
        visual.runtime->snapshot().session.state_epoch;

    const WorkerCommandResult wrong_session = visual.runtime->Submit(
        visual.NextRequest(),
        ControlExecutionCommand{
            .control = WorkerExecutionControlKind::StepFrame,
            .session_id = SessionId{999},
            .expected_state_epoch = epoch,
            .count = 1,
            .timeout = 100ms,
        }).get();
    EXPECT_EQ(wrong_session.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(
        wrong_session.error.code,
        WorkerRejectionCode::SessionMismatch);

    const WorkerCommandResult stale_epoch = visual.runtime->Submit(
        visual.NextRequest(),
        ControlExecutionCommand{
            .control = WorkerExecutionControlKind::StepFrame,
            .session_id = visual.session_id,
            .expected_state_epoch = StateEpoch{epoch.value() + 1},
            .count = 1,
            .timeout = 100ms,
        }).get();
    EXPECT_EQ(stale_epoch.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(
        stale_epoch.error.code,
        WorkerRejectionCode::StateEpochMismatch);

    ASSERT_EQ(visual.Invoke(602).outcome, WorkerCommandOutcome::Accepted);
    const WorkerCommandResult during_invocation = visual.runtime->Submit(
        visual.NextRequest(),
        ControlExecutionCommand{
            .control = WorkerExecutionControlKind::Pause,
            .session_id = visual.session_id,
            .expected_state_epoch = epoch,
            .timeout = 100ms,
        }).get();
    EXPECT_EQ(during_invocation.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(
        during_invocation.error.code,
        WorkerRejectionCode::InvalidState);

    ASSERT_TRUE(visual.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    ASSERT_TRUE(visual.events.WaitForTerminalCount(1));
    EXPECT_EQ(
        visual.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(ExecutionWorkerRuntime, VisualReadyControlsRemainReadyAndCompleteExactlyOnce)
{
    RuntimeHarness harness;
    SessionOpenOptions options = harness.OpenOptions();
    options.backend.visual = true;
    ASSERT_EQ(
        harness.Open(std::move(options)).outcome,
        WorkerCommandOutcome::Completed);
    const StateEpoch epoch =
        harness.runtime->snapshot().session.state_epoch;

    const WorkerCommandResult resumed = harness.runtime->Submit(
        harness.NextRequest(),
        ControlExecutionCommand{
            .control = WorkerExecutionControlKind::Resume,
            .session_id = harness.session_id,
            .expected_state_epoch = epoch,
        }).get();
    EXPECT_EQ(resumed.outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(resumed.execution_operation_id.has_value());
    EXPECT_FALSE(resumed.execution_terminal.has_value());
    EXPECT_EQ(resumed.snapshot.state, WorkerState::Ready);
    EXPECT_EQ(
        resumed.snapshot.execution.activity,
        ExecutionActivity::InteractiveRunning);

    const WorkerCommandResult paused = harness.runtime->Submit(
        harness.NextRequest(),
        ControlExecutionCommand{
            .control = WorkerExecutionControlKind::Pause,
            .session_id = harness.session_id,
            .expected_state_epoch = epoch,
            .timeout = 1s,
        }).get();
    EXPECT_EQ(paused.outcome, WorkerCommandOutcome::Completed);
    ASSERT_TRUE(paused.execution_terminal.has_value());
    EXPECT_EQ(
        paused.execution_terminal->status,
        ExecutionTerminalStatus::Paused);
    EXPECT_EQ(paused.snapshot.state, WorkerState::Ready);
    EXPECT_EQ(
        paused.snapshot.execution.activity,
        ExecutionActivity::IdlePaused);

    const WorkerCommandResult stepped = harness.runtime->Submit(
        harness.NextRequest(),
        ControlExecutionCommand{
            .control = WorkerExecutionControlKind::StepFrame,
            .session_id = harness.session_id,
            .expected_state_epoch = epoch,
            .count = 2,
            .timeout = 1s,
        }).get();
    EXPECT_EQ(stepped.outcome, WorkerCommandOutcome::Completed);
    ASSERT_TRUE(stepped.execution_terminal.has_value());
    EXPECT_EQ(
        stepped.execution_terminal->status,
        ExecutionTerminalStatus::StepsCompleted);
    EXPECT_EQ(stepped.execution_terminal->completed_count, 2u);
    EXPECT_EQ(stepped.snapshot.state, WorkerState::Ready);
    EXPECT_EQ(
        stepped.snapshot.execution.activity,
        ExecutionActivity::IdlePaused);

    ASSERT_TRUE(harness.events.WaitForCommandCount(
        WorkerCommandKind::ControlExecution,
        3));
    const auto completions =
        harness.events.CommandResults(WorkerCommandKind::ControlExecution);
    ASSERT_EQ(completions.size(), 3u);
    EXPECT_EQ(completions[0].request_id, resumed.request_id);
    EXPECT_EQ(completions[1].request_id, paused.request_id);
    EXPECT_EQ(completions[2].request_id, stepped.request_id);

    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(ExecutionWorkerRuntime, ShutdownUnwindsInteractiveResume)
{
    RuntimeHarness harness;
    SessionOpenOptions options = harness.OpenOptions();
    options.backend.visual = true;
    ASSERT_EQ(
        harness.Open(std::move(options)).outcome,
        WorkerCommandOutcome::Completed);
    const StateEpoch epoch =
        harness.runtime->snapshot().session.state_epoch;

    const WorkerCommandResult resumed = harness.runtime->Submit(
        harness.NextRequest(),
        ControlExecutionCommand{
            .control = WorkerExecutionControlKind::Resume,
            .session_id = harness.session_id,
            .expected_state_epoch = epoch,
        }).get();
    ASSERT_EQ(resumed.outcome, WorkerCommandOutcome::Accepted);
    ASSERT_EQ(
        harness.runtime->snapshot().execution.activity,
        ExecutionActivity::InteractiveRunning);

    const WorkerCommandResult shutdown = harness.Shutdown();
    EXPECT_EQ(shutdown.outcome, WorkerCommandOutcome::Completed);
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Stopped);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
}

TEST(
    ExecutionWorkerRuntime,
    RoutedStopInjectedAtCommandBoundaryIsNotLost)
{
    constexpr std::uint32_t kWakePc = 0x801dc288u;
    auto physical_control =
        std::make_shared<FakePhysicalStopBackendControl>();
    auto physical_backend =
        std::make_unique<FakePhysicalStopBackend>(physical_control);
    FakePhysicalStopBackend* physical_backend_raw =
        physical_backend.get();

    std::atomic<bool> continue_accepted{false};
    std::atomic<bool> arm_boundary_injection{false};
    std::latch boundary_open{1};
    std::latch injection_complete{1};
    auto hooks = std::make_shared<WorkerRuntimeTestHooks>();
    hooks->session_opened = [&](EmulationSession& session) {
        ExecutionRequestPolicy policy = SessionExecutionPolicy(
            session.snapshot().state_epoch,
            5s);
        const ExecutionSubmissionReceipt submission =
            session.SubmitExecution(ContinueUntilRequest{
                .policy = std::move(policy),
                .wake_group = WorkerWakeGroup(kWakePc),
            });
        continue_accepted.store(
            submission.accepted,
            std::memory_order_release);
    };
    hooks->before_ingress_stability_check = [&]() {
        if (!arm_boundary_injection.exchange(
                false,
                std::memory_order_acq_rel))
        {
            return;
        }
        boundary_open.count_down();
        injection_complete.wait();
    };

    RuntimeHarness harness(std::move(physical_backend), hooks);
    const WorkerCommandResult open = harness.Open();
    ASSERT_EQ(open.outcome, WorkerCommandOutcome::Completed);
    ASSERT_TRUE(
        continue_accepted.load(std::memory_order_acquire));

    std::atomic<bool> requested_break{false};
    std::thread injector([&]() {
        boundary_open.wait();
        const auto decision =
            physical_backend_raw->InjectJitPcStop(kWakePc);
        requested_break.store(
            decision.request_break,
            std::memory_order_release);
        injection_complete.count_down();
    });

    const WireRequestId screenshot_request = harness.NextRequest();
    arm_boundary_injection.store(true, std::memory_order_release);
    const WorkerCommandResult screenshot = harness.runtime->Submit(
        screenshot_request,
        CaptureScreenshotCommand{
            harness.session_id,
            "ingress-boundary.png",
            1s}).get();
    injector.join();

    EXPECT_TRUE(requested_break.load(std::memory_order_acquire));
    EXPECT_EQ(screenshot.outcome, WorkerCommandOutcome::Completed);
    ASSERT_TRUE(harness.events.WaitForExecutionTerminalCount(1));

    const std::vector<WorkerEvent> events = harness.events.Events();
    std::optional<std::size_t> routed_terminal_index;
    for (std::size_t index = 0; index < events.size(); ++index)
    {
        if (const auto* execution =
                std::get_if<WorkerExecutionEvent>(&events[index]);
            execution && execution->event.terminal &&
            execution->event.terminal->status ==
                ExecutionTerminalStatus::RequestedCompletion)
        {
            routed_terminal_index = index;
        }
    }

    ASSERT_TRUE(routed_terminal_index.has_value());

    EXPECT_EQ(
        harness.Shutdown().outcome,
        WorkerCommandOutcome::Completed);
}

TEST(ExecutionWorkerRuntime, TerminalScreenshotUsesConfiguredActorPath)
{
    RuntimeHarness harness;
    SessionOpenOptions options = harness.OpenOptions();
    options.screenshot_directory = "terminal-shots";
    options.screenshot_timeout = 321ms;
    options.screenshot_on_terminal = true;
    ASSERT_EQ(
        harness.Open(std::move(options)).outcome,
        WorkerCommandOutcome::Completed);

    ASSERT_EQ(harness.Invoke(701).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));

    const auto paths = harness.backend->ScreenshotPaths();
    ASSERT_EQ(paths.size(), 1);
    EXPECT_EQ(
        paths.front(),
        std::filesystem::path("terminal-shots") /
            "invocation-701-terminal.png");
    EXPECT_FALSE(harness.backend->HasOwnerViolation());

    EXPECT_EQ(harness.Shutdown().outcome, WorkerCommandOutcome::Completed);
}

TEST(ExecutionWorkerRuntime, RuntimeSubmissionExceptionTaintsAndTerminatesOnce)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    {
        std::lock_guard lock(harness.program->mutex);
        harness.program->throw_start = true;
    }

    const WorkerCommandResult result = harness.Invoke(801);
    EXPECT_EQ(result.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(result.error.code, WorkerRejectionCode::InternalFailure);
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Tainted);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
    EXPECT_EQ(harness.events.TerminalCount(), 0);
    EXPECT_EQ(harness.program->ShutdownCount(), 1);

    const WorkerCommandResult shutdown = harness.Shutdown();
    EXPECT_EQ(shutdown.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
    EXPECT_EQ(harness.program->ShutdownCount(), 1);
}

TEST(ExecutionWorkerRuntime, UnknownScreenshotFailureSynthesizesOneTerminal)
{
    RuntimeHarness harness;
    ASSERT_EQ(harness.Open().outcome, WorkerCommandOutcome::Completed);
    ASSERT_EQ(harness.Invoke(901).outcome, WorkerCommandOutcome::Accepted);
    ASSERT_TRUE(harness.program->WaitForStarts(1));

    harness.backend->SetScreenshotResult(BackendResult::Failure(
        BackendErrorCode::OperationFailed,
        "screenshot left core integrity unknown",
        BackendIntegrity::Unknown));
    const WorkerCommandResult screenshot = harness.runtime->Submit(
        harness.NextRequest(),
        CaptureScreenshotCommand{
            harness.session_id,
            "uncertain.png",
            100ms}).get();
    EXPECT_EQ(screenshot.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(harness.runtime->snapshot().state, WorkerState::Tainted);
    ASSERT_TRUE(harness.events.WaitForTerminalCount(1));

    const auto terminals = harness.events.Terminals();
    ASSERT_EQ(terminals.size(), 1);
    EXPECT_EQ(
        terminals.front().status,
        InvocationTerminalStatus::InfrastructureFailure);
    EXPECT_EQ(terminals.front().cleanup, CleanupStatus::Failed);
    EXPECT_EQ(
        terminals.front().session_disposition,
        SessionDisposition::Tainted);

    // The taint path retires ProgramRuntime ingress. A late completion cannot
    // produce a second terminal.
    ASSERT_TRUE(harness.program->EmitTerminal(
        InvocationTerminalStatus::Completed));
    const WorkerCommandResult shutdown = harness.Shutdown();
    EXPECT_EQ(shutdown.outcome, WorkerCommandOutcome::Rejected);
    EXPECT_EQ(harness.events.TerminalCount(), 1);
    EXPECT_EQ(harness.backend->CloseCount(), 1);
}

} // namespace
