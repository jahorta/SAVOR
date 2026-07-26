#include <gtest/gtest.h>

#include "Runner/Runtime/EmulationSession.h"
#include "Runner/Runtime/IProgramRuntimePort.h"
#include "Runner/Runtime/WorkerRuntime.h"

#include <algorithm>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <future>
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

static_assert(!std::is_same_v<StateEpoch, WorkerCommandSequence>);
static_assert(!std::is_convertible_v<StateEpoch, WorkerCommandSequence>);
static_assert(!std::is_convertible_v<WorkerCommandSequence, StateEpoch>);
static_assert(!std::is_convertible_v<StateEpoch, std::uint64_t>);
static_assert(!std::is_convertible_v<std::uint64_t, StateEpoch>);

struct FakeBackendControl
{
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::optional<std::thread::id> owner_thread;
    bool owner_thread_violation = false;
    std::vector<std::string> calls;
    std::vector<std::filesystem::path> screenshots;

    BackendResult open_result = BackendResult::Success();
    BackendResult reboot_result = BackendResult::Success();
    BackendResult close_result = BackendResult::Success();
    BackendResult pause_result = BackendResult::Success();
    BackendResult resume_result = BackendResult::Success();
    BackendResult step_instruction_result = BackendResult::Success();
    BackendResult step_frame_result = BackendResult::Success();
    BackendResult restore_file_result = BackendResult::Success();
    BackendResult restore_buffer_result = BackendResult::Success();
    BackendResult save_file_result = BackendResult::Success();
    BackendResult save_buffer_result = BackendResult::Success();
    BackendResult screenshot_result = BackendResult::Success();
    BackendCoreState core_state = BackendCoreState::Closed;
    BackendCoreState open_core_state = BackendCoreState::Running;

    int open_count = 0;
    int reboot_count = 0;
    int close_count = 0;
    int screenshot_count = 0;
    int restore_file_count = 0;
    int restore_buffer_count = 0;

    void RecordLocked(const char* call)
    {
        const std::thread::id current = std::this_thread::get_id();
        if (!owner_thread)
            owner_thread = current;
        else if (*owner_thread != current)
            owner_thread_violation = true;
        calls.emplace_back(call);
    }

    void SetOpenResult(BackendResult result, BackendCoreState state)
    {
        std::lock_guard lock(mutex);
        open_result = std::move(result);
        open_core_state = state;
    }

    void SetRebootResult(BackendResult result)
    {
        std::lock_guard lock(mutex);
        reboot_result = std::move(result);
    }

    void SetRestoreFileResult(BackendResult result)
    {
        std::lock_guard lock(mutex);
        restore_file_result = std::move(result);
    }

    void SetRestoreBufferResult(BackendResult result)
    {
        std::lock_guard lock(mutex);
        restore_buffer_result = std::move(result);
    }

    void SetStepInstructionResult(BackendResult result)
    {
        std::lock_guard lock(mutex);
        step_instruction_result = std::move(result);
    }

    void SetScreenshotResult(BackendResult result)
    {
        std::lock_guard lock(mutex);
        screenshot_result = std::move(result);
    }

    [[nodiscard]] int CloseCount() const
    {
        std::lock_guard lock(mutex);
        return close_count;
    }

    [[nodiscard]] int OpenCount() const
    {
        std::lock_guard lock(mutex);
        return open_count;
    }

    [[nodiscard]] int ScreenshotCount() const
    {
        std::lock_guard lock(mutex);
        return screenshot_count;
    }

    [[nodiscard]] std::vector<std::filesystem::path> ScreenshotPaths() const
    {
        std::lock_guard lock(mutex);
        return screenshots;
    }

    [[nodiscard]] bool HasOwnerViolation() const
    {
        std::lock_guard lock(mutex);
        return owner_thread_violation;
    }

    [[nodiscard]] std::optional<std::thread::id> OwnerThread() const
    {
        std::lock_guard lock(mutex);
        return owner_thread;
    }
};

class FakeDolphinBackend final : public IDolphinBackend
{
public:
    explicit FakeDolphinBackend(std::shared_ptr<FakeBackendControl> control)
        : control_(std::move(control))
    {
    }

    BackendResult Open(const BackendOpenOptions&) override
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("open");
        ++control_->open_count;
        BackendResult result = control_->open_result;
        if (result.ok)
            control_->core_state = control_->open_core_state;
        control_->changed.notify_all();
        return result;
    }

    BackendResult Reboot() override
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("reboot");
        ++control_->reboot_count;
        BackendResult result = control_->reboot_result;
        if (result.ok)
            control_->core_state = BackendCoreState::Running;
        else if (result.integrity == BackendIntegrity::Unknown)
            control_->core_state = BackendCoreState::Unknown;
        control_->changed.notify_all();
        return result;
    }

    BackendResult Close() override
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("close");
        ++control_->close_count;
        BackendResult result = control_->close_result;
        control_->core_state = result.ok
            ? BackendCoreState::Closed
            : BackendCoreState::Unknown;
        control_->changed.notify_all();
        return result;
    }

    BackendCoreState QueryCoreState() const noexcept override
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("query_core_state");
        return control_->core_state;
    }

    BackendHealthReport CheckHealth() const override
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("check_health");
        const bool healthy =
            control_->core_state == BackendCoreState::Running ||
            control_->core_state == BackendCoreState::Paused;
        return {
            healthy,
            control_->core_state,
            healthy ? std::string{} : std::string{"fake backend is unhealthy"}};
    }

    BackendResult Pause(std::chrono::milliseconds) override
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("pause");
        BackendResult result = control_->pause_result;
        if (result.ok)
            control_->core_state = BackendCoreState::Paused;
        return result;
    }

    BackendResult Resume() override
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("resume");
        BackendResult result = control_->resume_result;
        if (result.ok)
            control_->core_state = BackendCoreState::Running;
        return result;
    }

    BackendResult StepInstruction(std::chrono::milliseconds) override
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("step_instruction");
        return control_->step_instruction_result;
    }

    BackendResult StepFrame(std::chrono::milliseconds) override
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("step_frame");
        return control_->step_frame_result;
    }

    BackendResult RestoreStateFile(const std::filesystem::path&) override
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("restore_file");
        ++control_->restore_file_count;
        BackendResult result = control_->restore_file_result;
        if (!result.ok && result.integrity == BackendIntegrity::Unknown)
            control_->core_state = BackendCoreState::Unknown;
        return result;
    }

    BackendResult SaveStateFile(const std::filesystem::path&) override
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("save_file");
        return control_->save_file_result;
    }

    BackendBufferResult SaveStateBuffer() override
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("save_buffer");
        return {control_->save_buffer_result, {0x10, 0x20, 0x30}};
    }

    BackendResult RestoreStateBuffer(
        const std::vector<std::uint8_t>&) override
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("restore_buffer");
        ++control_->restore_buffer_count;
        BackendResult result = control_->restore_buffer_result;
        if (!result.ok && result.integrity == BackendIntegrity::Unknown)
            control_->core_state = BackendCoreState::Unknown;
        return result;
    }

    BackendResult CaptureScreenshot(
        const std::filesystem::path& path,
        std::chrono::milliseconds) override
    {
        std::lock_guard lock(control_->mutex);
        control_->RecordLocked("screenshot");
        ++control_->screenshot_count;
        control_->screenshots.push_back(path);
        BackendResult result = control_->screenshot_result;
        if (!result.ok && result.integrity == BackendIntegrity::Unknown)
            control_->core_state = BackendCoreState::Unknown;
        control_->changed.notify_all();
        return result;
    }

private:
    std::shared_ptr<FakeBackendControl> control_;
};

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
    std::shared_ptr<FakeBackendControl> backend =
        std::make_shared<FakeBackendControl>();
    std::shared_ptr<FakeProgramRuntimeControl> program =
        std::make_shared<FakeProgramRuntimeControl>();
    WorkerEventLog events;
    std::unique_ptr<WorkerRuntime> runtime;
    std::uint64_t next_request = 1;

    RuntimeHarness()
    {
        runtime = std::make_unique<WorkerRuntime>(
            std::make_unique<EmulationSession>(
                session_id,
                std::make_unique<FakeDolphinBackend>(backend)),
            std::make_unique<FakeProgramRuntimePort>(program),
            [this](const WorkerEvent& event) { events.Record(event); });
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
    auto control = std::make_shared<FakeBackendControl>();
    control->SetOpenResult(
        BackendResult::Failure(
            BackendErrorCode::BootFailed,
            "expected boot failure"),
        BackendCoreState::Closed);
    EmulationSession session(
        SessionId(1),
        std::make_unique<FakeDolphinBackend>(control));

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
    auto control = std::make_shared<FakeBackendControl>();
    EmulationSession session(
        SessionId(2),
        std::make_unique<FakeDolphinBackend>(control));

    const SessionOperationReceipt open = session.Open({});
    ASSERT_TRUE(open.ok);
    EXPECT_EQ(open.origin_epoch, StateEpoch{});
    EXPECT_EQ(open.resulting_epoch, StateEpoch{1});

    EXPECT_TRUE(session.Pause(100ms).ok);
    EXPECT_EQ(session.snapshot().state_epoch, StateEpoch{1});
    EXPECT_TRUE(session.Resume().ok);
    EXPECT_TRUE(session.StepInstruction(100ms).ok);
    EXPECT_TRUE(session.StepFrame(100ms).ok);
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
        auto control = std::make_shared<FakeBackendControl>();
        control->SetOpenResult(
            BackendResult::Success(),
            BackendCoreState::Stopped);
        EmulationSession session(
            SessionId(3),
            std::make_unique<FakeDolphinBackend>(control));

        const SessionOperationReceipt open = session.Open({});
        EXPECT_FALSE(open.ok);
        EXPECT_EQ(open.resulting_epoch, StateEpoch{});
        EXPECT_EQ(open.disposition, SessionDisposition::Tainted);
        EXPECT_TRUE(session.Shutdown().ok);
    }

    auto control = std::make_shared<FakeBackendControl>();
    EmulationSession session(
        SessionId(4),
        std::make_unique<FakeDolphinBackend>(control));
    ASSERT_TRUE(session.Open({}).ok);

    control->SetStepInstructionResult(BackendResult::Failure(
        BackendErrorCode::Timeout,
        "step completion is uncertain",
        BackendIntegrity::Unknown));
    const SessionOperationReceipt step = session.StepInstruction(100ms);
    EXPECT_FALSE(step.ok);
    EXPECT_EQ(step.origin_epoch, StateEpoch{1});
    EXPECT_EQ(step.resulting_epoch, StateEpoch{1});
    EXPECT_EQ(step.disposition, SessionDisposition::Tainted);
    EXPECT_FALSE(session.CaptureScreenshot("after-taint.png", 100ms).ok);
    EXPECT_TRUE(session.Shutdown().ok);
}

TEST(EmulationSession, RejectsOffOwnerCallsBeforeBackendMutation)
{
    auto control = std::make_shared<FakeBackendControl>();
    EmulationSession session(
        SessionId(5),
        std::make_unique<FakeDolphinBackend>(control));
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
