#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Runner/Runtime/RuntimeTypes.h"
#include "Utils/ModulePath.h"
#include "Worker/ProcessWorker.h"

namespace savor {

class ProcessWorkerTestPeer {
public:
    template <typename Payload>
    static bool DeliverPayload(
        ProcessWorker& worker,
        wrms::MessageKind kind,
        const Payload& value,
        std::uint64_t request_id = 0)
    {
        std::vector<std::uint8_t> payload;
        if (!wrms::EncodePayload(value, payload))
            return false;
        worker.handle_frame(wrms::FrameView{
            .header = {
                .kind = kind,
                .payload_size = static_cast<std::uint32_t>(payload.size()),
                .request_id = request_id,
            },
            .payload = payload,
        });
        return true;
    }

    static bool DeliverOpenSessionResult(
        ProcessWorker& worker,
        const wrms::OpenSessionResultPayload& result)
    {
        return DeliverPayload(
            worker,
            wrms::MessageKind::OpenSessionResult,
            result,
            991);
    }

    static std::uint32_t EffectiveExecutionCommandTimeout(
        std::uint32_t operation_timeout_ms,
        std::uint32_t command_timeout_ms)
    {
        return ProcessWorker::effective_execution_command_timeout(
            operation_timeout_ms,
            command_timeout_ms);
    }

    static bool ValidateExecutionResult(
        wrms::ExecutionControlKind control,
        runtime::SessionId session_id,
        runtime::StateEpoch expected_state_epoch,
        std::uint32_t requested_count,
        const wrms::ExecutionResultPayload& result,
        std::string* error_out = nullptr)
    {
        return ProcessWorker::validate_execution_result(
            control,
            session_id,
            expected_state_epoch,
            requested_count,
            result,
            error_out);
    }

    static bool StartNegotiatedVisualTransport(
        ProcessWorker& worker,
        runtime::SessionId session_id,
        runtime::StateEpoch state_epoch)
    {
        if (worker.writer_.joinable() || worker.child_stdin_write_)
            return false;

        HANDLE sink = CreateFileW(
            L"NUL",
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (sink == INVALID_HANDLE_VALUE)
            return false;

        {
            std::lock_guard<std::mutex> lock(worker.writer_mutex_);
            worker.writer_queue_.clear();
            worker.writer_exit_requested_ = false;
            worker.writer_active_ = false;
            worker.writer_cancel_requested_.store(
                false,
                std::memory_order_release);
        }
        worker.child_stdin_write_ = sink;
        worker.running_.store(true, std::memory_order_release);
        worker.accepting_writes_.store(true, std::memory_order_release);

        const auto capabilities = runtime::AddCapability(
            runtime::kSlice1ProductionCapabilities,
            runtime::WorkerCapability::InteractiveVisualDebug);
        {
            std::lock_guard<std::mutex> lock(worker.snapshot_mutex_);
            worker.snapshot_.running = true;
            worker.snapshot_.hello_received = true;
            worker.snapshot_.process_capabilities = capabilities;
            worker.snapshot_.session_open = true;
            worker.snapshot_.session_visual_intent = true;
            worker.snapshot_.session_id = session_id;
            worker.snapshot_.state_epoch = state_epoch;
            worker.snapshot_.session_capabilities = capabilities;
            worker.snapshot_.worker_state = runtime::WorkerState::Ready;
            worker.snapshot_.session_disposition =
                runtime::SessionDisposition::Clean;
            worker.snapshot_.execution_activity =
                wrms::ExecutionActivityCode::IdlePaused;
        }
        worker.writer_ =
            std::thread([&worker]() { worker.writer_thread(); });
        return true;
    }

    static void StopNegotiatedVisualTransport(ProcessWorker& worker)
    {
        {
            std::lock_guard<std::mutex> lock(worker.writer_mutex_);
            worker.accepting_writes_.store(false, std::memory_order_release);
            worker.writer_exit_requested_ = true;
        }
        worker.writer_cv_.notify_all();
        if (worker.writer_.joinable())
            worker.writer_.join();
        if (worker.child_stdin_write_)
        {
            CloseHandle(worker.child_stdin_write_);
            worker.child_stdin_write_ = nullptr;
        }
        worker.running_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(worker.snapshot_mutex_);
        worker.snapshot_.running = false;
    }

    static std::pair<bool, bool> ClassifyShutdownResponse(
        const wrms::ShutdownResultPayload& result)
    {
        std::vector<std::uint8_t> payload;
        if (!wrms::EncodePayload(result, payload))
            return {false, false};
        bool graceful = false;
        const bool valid =
            ProcessWorker::classify_shutdown_response(payload, &graceful);
        return {valid, graceful};
    }

    static std::pair<bool, bool> ClassifyMalformedShutdownResponse()
    {
        const std::array<std::uint8_t, 1> malformed{0xff};
        bool graceful = true;
        const bool valid =
            ProcessWorker::classify_shutdown_response(
                malformed,
                &graceful);
        return {valid, graceful};
    }
};

} // namespace savor

namespace {

std::filesystem::path FindBuiltWorker()
{
    const auto test_directory = utils::getExecutablePath();
    const std::array candidates{
        test_directory / "SavorWorker.exe",
        test_directory.parent_path().parent_path() /
            "bin" /
            test_directory.parent_path().filename() /
            test_directory.filename() /
            "SavorWorker.exe",
    };
    std::error_code error;
    for (const auto& candidate : candidates)
    {
        if (std::filesystem::is_regular_file(candidate, error) && !error)
            return candidate;
        error.clear();
    }
    return {};
}

void ExpectErrorContains(const savor::ProcessWorker& worker, const char* expected)
{
    const auto error = worker.last_error();
    EXPECT_NE(error.find(expected), std::string::npos) << error;
}

} // namespace

TEST(ProcessWorkerV1, RetainedLegacyApisFailLocallyWithHardCutoverDiagnostics)
{
    savor::ProcessWorker worker;
    savor::PSInit init;
    savor::PSJob job;

    EXPECT_FALSE(worker.ctl_set_program(1, 1, init));
    ExpectErrorContains(worker, "ctl_set_program");
    ExpectErrorContains(worker, savor::kDisconnectedWorkerApiDiagnostic);

    EXPECT_FALSE(worker.ctl_run_init_once());
    ExpectErrorContains(worker, "ctl_run_init_once");
    ExpectErrorContains(worker, savor::kDisconnectedWorkerApiDiagnostic);

    EXPECT_FALSE(worker.ctl_activate_main());
    ExpectErrorContains(worker, "ctl_activate_main");
    ExpectErrorContains(worker, savor::kDisconnectedWorkerApiDiagnostic);

    EXPECT_FALSE(worker.send_job(41, 7, job));
    ExpectErrorContains(worker, "send_job");
    ExpectErrorContains(worker, savor::kDisconnectedWorkerApiDiagnostic);

    EXPECT_FALSE(worker.visual_pause_emulation());
    ExpectErrorContains(worker, "legacy visual pause");
    ExpectErrorContains(worker, "pause_guest_execution");

    EXPECT_FALSE(worker.visual_resume_emulation());
    ExpectErrorContains(worker, "legacy visual resume");
    ExpectErrorContains(worker, "resume_guest_execution");

    EXPECT_FALSE(worker.visual_step_vm());
    ExpectErrorContains(worker, "program/VM stepping");
    ExpectErrorContains(worker, "ProgramRuntime");

    EXPECT_FALSE(worker.is_running());
    EXPECT_EQ(worker.GetPid(), 0);
}

TEST(ProcessWorkerV1, StopIsIdempotentWithoutAStartedProcess)
{
    savor::ProcessWorker worker;

    EXPECT_EQ(
        savor::kProcessWorkerDefaultStopGrace,
        std::chrono::seconds(5));
    worker.stop();
    const auto first = worker.last_stop_snapshot();
    EXPECT_FALSE(first.already_stopping);
    EXPECT_FALSE(first.was_running);
    EXPECT_EQ(first.stop_grace_ms, 5000u);
    EXPECT_FALSE(first.deadline_expired);
    EXPECT_FALSE(first.shutdown_frame_attempted);
    EXPECT_FALSE(first.forced);
    EXPECT_FALSE(first.termination_attempted);

    worker.stop();
    const auto second = worker.last_stop_snapshot();
    EXPECT_TRUE(second.already_stopping);
    EXPECT_FALSE(second.was_running);
    EXPECT_FALSE(second.forced);
    EXPECT_FALSE(worker.is_running());
}

TEST(ProcessWorkerV1, ExecutionControlsFailLocallyWithoutNegotiatedCapability)
{
    savor::ProcessWorker worker;
    const auto session = savor::runtime::SessionId{91};
    const auto epoch = savor::runtime::StateEpoch{4};

    EXPECT_FALSE(worker.pause_guest_execution(session, epoch));
    ExpectErrorContains(worker, "does not advertise");
    EXPECT_FALSE(worker.resume_guest_execution(session, epoch));
    ExpectErrorContains(worker, "does not advertise");
    EXPECT_FALSE(worker.step_guest_frames(session, epoch, 1));
    ExpectErrorContains(worker, "does not advertise");
    EXPECT_FALSE(worker.step_guest_instructions(session, epoch, 1));
    ExpectErrorContains(worker, "does not advertise");
}

TEST(
    ProcessWorkerV1,
    CorrelatesConcurrentExecutionControlsOverNegotiatedFakeTransport)
{
    struct ObservedRequest
    {
        std::uint64_t request_id = 0;
        savor::wrms::ControlExecutionPayload payload;
    };

    constexpr auto session = savor::runtime::SessionId{91};
    constexpr auto epoch = savor::runtime::StateEpoch{4};
    std::mutex observed_mutex;
    std::vector<ObservedRequest> observed;
    std::atomic<bool> decoded_all{true};
    savor::ProcessWorker* worker_ptr = nullptr;

    auto hooks = std::make_shared<savor::ProcessWorkerTestHooks>();
    hooks->observe_writer_frame =
        [&](savor::wrms::MessageKind kind,
            std::span<const std::uint8_t> bytes) {
            if (kind != savor::wrms::MessageKind::ControlExecution)
                return;

            const auto frame = savor::wrms::DecodeFrame(bytes, true);
            savor::wrms::ControlExecutionPayload request;
            if (frame.status != savor::wrms::FrameDecodeStatus::Complete ||
                frame.frame.header.kind !=
                    savor::wrms::MessageKind::ControlExecution ||
                !savor::wrms::DecodePayload(frame.frame.payload, request))
            {
                decoded_all.store(false, std::memory_order_release);
                return;
            }

            std::vector<ObservedRequest> responses;
            {
                std::lock_guard<std::mutex> lock(observed_mutex);
                observed.push_back({
                    frame.frame.header.request_id,
                    request,
                });
                if (observed.size() == 2)
                    responses = observed;
            }

            // Resolve the second request first. Each waiting caller must still
            // receive the result carrying its own WRMS request ID.
            for (auto it = responses.rbegin(); it != responses.rend(); ++it)
            {
                const auto delivered =
                    savor::ProcessWorkerTestPeer::DeliverPayload(
                        *worker_ptr,
                        savor::wrms::MessageKind::ExecutionResult,
                        savor::wrms::ExecutionResultPayload{
                            .command_sequence = 2000 + it->request_id,
                            .control = it->payload.control,
                            .status =
                                savor::wrms::CommandStatus::Succeeded,
                            .session_id = it->payload.session_id,
                            .state_epoch =
                                it->payload.expected_state_epoch,
                            .operation_id = 1000 + it->request_id,
                            .activity =
                                savor::wrms::ExecutionActivityCode::
                                    IdlePaused,
                            .has_terminal_status = true,
                            .terminal_status =
                                savor::wrms::ExecutionTerminalStatusCode::
                                    StepsCompleted,
                            .completed_count = it->payload.count,
                            .program_counter = 0x801dc288u,
                        },
                        it->request_id);
                if (!delivered)
                    decoded_all.store(false, std::memory_order_release);
            }
        };

    savor::ProcessWorker worker{hooks};
    worker_ptr = &worker;
    ASSERT_TRUE(
        savor::ProcessWorkerTestPeer::StartNegotiatedVisualTransport(
            worker,
            session,
            epoch));

    std::latch ready{2};
    std::latch go{1};
    const auto submit = [&](std::uint32_t count) {
        ready.count_down();
        go.wait();
        savor::wrms::ExecutionResultPayload result;
        const bool succeeded = worker.step_guest_frames(
            session,
            epoch,
            count,
            &result,
            1000,
            5000);
        return std::pair{succeeded, result};
    };
    auto first = std::async(std::launch::async, submit, 2u);
    auto second = std::async(std::launch::async, submit, 3u);
    ready.wait();
    go.count_down();

    const auto first_result = first.get();
    const auto second_result = second.get();
    savor::ProcessWorkerTestPeer::StopNegotiatedVisualTransport(worker);

    ASSERT_TRUE(decoded_all.load(std::memory_order_acquire));
    ASSERT_TRUE(first_result.first);
    ASSERT_TRUE(second_result.first);
    EXPECT_EQ(first_result.second.completed_count, 2u);
    EXPECT_EQ(second_result.second.completed_count, 3u);
    EXPECT_NE(
        first_result.second.operation_id,
        second_result.second.operation_id);
    EXPECT_NE(
        first_result.second.command_sequence,
        second_result.second.command_sequence);

    std::lock_guard<std::mutex> lock(observed_mutex);
    ASSERT_EQ(observed.size(), 2u);
    EXPECT_NE(observed[0].request_id, observed[1].request_id);
    for (const ObservedRequest& request : observed)
    {
        EXPECT_EQ(
            request.payload.control,
            savor::wrms::ExecutionControlKind::StepFrame);
        EXPECT_EQ(request.payload.session_id, session.value());
        EXPECT_EQ(
            request.payload.expected_state_epoch,
            epoch.value());
        EXPECT_TRUE(
            request.payload.count == 2 ||
            request.payload.count == 3);
        const auto& result = request.payload.count == 2
            ? first_result.second
            : second_result.second;
        EXPECT_EQ(result.operation_id, 1000 + request.request_id);
        EXPECT_EQ(
            result.command_sequence,
            2000 + request.request_id);
    }
}

TEST(ProcessWorkerV1, ExecutionControlWaitOutlivesBoundedOperation)
{
    EXPECT_EQ(
        savor::ProcessWorkerTestPeer::EffectiveExecutionCommandTimeout(
            3000,
            10000),
        10000u);
    EXPECT_EQ(
        savor::ProcessWorkerTestPeer::EffectiveExecutionCommandTimeout(
            15000,
            10000),
        16000u);
    EXPECT_EQ(
        savor::ProcessWorkerTestPeer::EffectiveExecutionCommandTimeout(
            (std::numeric_limits<std::uint32_t>::max)(),
            1),
        (std::numeric_limits<std::uint32_t>::max)());
    EXPECT_EQ(
        savor::ProcessWorkerTestPeer::EffectiveExecutionCommandTimeout(
            0,
            0),
        10000u);
}

TEST(ProcessWorkerV1, SuccessfulExecutionResultsMustMatchControlSemantics)
{
    using savor::wrms::CommandStatus;
    using savor::wrms::ExecutionActivityCode;
    using savor::wrms::ExecutionControlKind;
    using savor::wrms::ExecutionResultPayload;
    using savor::wrms::ExecutionTerminalStatusCode;

    const auto session = savor::runtime::SessionId{91};
    const auto epoch = savor::runtime::StateEpoch{4};
    const ExecutionResultPayload valid_step{
        .control = ExecutionControlKind::StepFrame,
        .status = CommandStatus::Succeeded,
        .session_id = session.value(),
        .state_epoch = epoch.value(),
        .operation_id = 17,
        .activity = ExecutionActivityCode::IdlePaused,
        .has_terminal_status = true,
        .terminal_status = ExecutionTerminalStatusCode::StepsCompleted,
        .completed_count = 2,
    };
    EXPECT_TRUE(savor::ProcessWorkerTestPeer::ValidateExecutionResult(
        ExecutionControlKind::StepFrame,
        session,
        epoch,
        2,
        valid_step));

    auto incomplete_step = valid_step;
    incomplete_step.has_terminal_status = false;
    std::string error;
    EXPECT_FALSE(savor::ProcessWorkerTestPeer::ValidateExecutionResult(
        ExecutionControlKind::StepFrame,
        session,
        epoch,
        2,
        incomplete_step,
        &error));
    EXPECT_NE(error.find("step"), std::string::npos);

    auto short_step = valid_step;
    short_step.completed_count = 1;
    EXPECT_FALSE(savor::ProcessWorkerTestPeer::ValidateExecutionResult(
        ExecutionControlKind::StepFrame,
        session,
        epoch,
        2,
        short_step));

    const ExecutionResultPayload valid_resume{
        .control = ExecutionControlKind::Resume,
        .status = CommandStatus::Succeeded,
        .session_id = session.value(),
        .state_epoch = epoch.value(),
        .operation_id = 18,
        .activity = ExecutionActivityCode::InteractiveRunning,
    };
    EXPECT_TRUE(savor::ProcessWorkerTestPeer::ValidateExecutionResult(
        ExecutionControlKind::Resume,
        session,
        epoch,
        0,
        valid_resume));

    auto terminal_resume = valid_resume;
    terminal_resume.has_terminal_status = true;
    terminal_resume.terminal_status =
        ExecutionTerminalStatusCode::Paused;
    EXPECT_FALSE(savor::ProcessWorkerTestPeer::ValidateExecutionResult(
        ExecutionControlKind::Resume,
        session,
        epoch,
        0,
        terminal_resume));

    const ExecutionResultPayload valid_pause{
        .control = ExecutionControlKind::Pause,
        .status = CommandStatus::Succeeded,
        .session_id = session.value(),
        .state_epoch = epoch.value(),
        .operation_id = 19,
        .activity = ExecutionActivityCode::IdlePaused,
        .has_terminal_status = true,
        .terminal_status = ExecutionTerminalStatusCode::Paused,
    };
    EXPECT_TRUE(savor::ProcessWorkerTestPeer::ValidateExecutionResult(
        ExecutionControlKind::Pause,
        session,
        epoch,
        0,
        valid_pause));
}

TEST(ProcessWorkerV1, CleanupFailedOrMalformedShutdownIsNotGraceful)
{
    const auto graceful =
        savor::ProcessWorkerTestPeer::ClassifyShutdownResponse(
            savor::wrms::ShutdownResultPayload{
                .status = savor::wrms::ShutdownStatus::Graceful,
            });
    EXPECT_TRUE(graceful.first);
    EXPECT_TRUE(graceful.second);

    const auto cleanup_failed =
        savor::ProcessWorkerTestPeer::ClassifyShutdownResponse(
            savor::wrms::ShutdownResultPayload{
                .status = savor::wrms::ShutdownStatus::CleanupFailed,
                .final_disposition =
                    savor::wrms::SessionDispositionCode::Tainted,
            });
    EXPECT_TRUE(cleanup_failed.first);
    EXPECT_FALSE(cleanup_failed.second);

    const auto malformed =
        savor::ProcessWorkerTestPeer::ClassifyMalformedShutdownResponse();
    EXPECT_FALSE(malformed.first);
    EXPECT_FALSE(malformed.second);
}

TEST(ProcessWorkerV1, RejectedDuplicateOpenPreservesExistingSessionSnapshot)
{
    savor::ProcessWorker worker;
    const auto session_capabilities = savor::runtime::AddCapability(
        savor::runtime::kSlice1ProductionCapabilities,
        savor::runtime::WorkerCapability::ProgramInvocation);

    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverOpenSessionResult(
        worker,
        savor::wrms::OpenSessionResultPayload{
            .success = true,
            .session_id = 71,
            .state_epoch = 9,
            .capability_mask = session_capabilities,
            .worker_state = savor::wrms::WorkerStateCode::Ready,
            .session_disposition =
                savor::wrms::SessionDispositionCode::Clean,
        }));
    const auto opened = worker.latest_snapshot();
    ASSERT_TRUE(opened.session_open);
    ASSERT_EQ(opened.session_id, savor::runtime::SessionId{71});
    ASSERT_EQ(opened.state_epoch, savor::runtime::StateEpoch{9});

    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverOpenSessionResult(
        worker,
        savor::wrms::OpenSessionResultPayload{
            .success = false,
            .worker_state = savor::wrms::WorkerStateCode::AwaitingSession,
            .session_disposition =
                savor::wrms::SessionDispositionCode::Closed,
            .rejection_code = savor::wrms::RejectionCode::InvalidState,
            .error_code = "session_already_open",
            .message = "the process already owns a session",
        }));
    const auto rejected = worker.latest_snapshot();
    EXPECT_TRUE(rejected.session_open);
    EXPECT_EQ(rejected.session_id, opened.session_id);
    EXPECT_EQ(rejected.state_epoch, opened.state_epoch);
    EXPECT_EQ(rejected.session_capabilities, opened.session_capabilities);
    EXPECT_EQ(rejected.worker_state, opened.worker_state);
    EXPECT_EQ(rejected.session_disposition, opened.session_disposition);
    EXPECT_EQ(
        rejected.last_error,
        "the process already owns a session");
}

TEST(ProcessWorkerV1, RuntimeDiagnosticPreservesSessionAndCallbacksCarryAttemptId)
{
    savor::ProcessWorker worker;
    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverOpenSessionResult(
        worker,
        savor::wrms::OpenSessionResultPayload{
            .success = true,
            .session_id = 72,
            .state_epoch = 10,
            .capability_mask =
                savor::runtime::kSlice1ProductionCapabilities,
            .worker_state = savor::wrms::WorkerStateCode::Ready,
            .session_disposition =
                savor::wrms::SessionDispositionCode::Clean,
        }));
    const auto authoritative = worker.latest_snapshot();

    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::RuntimeDiagnostic,
        savor::wrms::RuntimeDiagnosticPayload{
            .rejection_code =
                savor::wrms::RejectionCode::InvocationMismatch,
            .command_sequence = 17,
            .invocation_id = 700,
            .message = "late invocation event was ignored",
        }));
    const auto diagnosed = worker.latest_snapshot();
    EXPECT_EQ(diagnosed.session_open, authoritative.session_open);
    EXPECT_EQ(diagnosed.session_id, authoritative.session_id);
    EXPECT_EQ(diagnosed.state_epoch, authoritative.state_epoch);
    EXPECT_EQ(
        diagnosed.session_capabilities,
        authoritative.session_capabilities);
    EXPECT_EQ(diagnosed.worker_state, authoritative.worker_state);
    EXPECT_EQ(
        diagnosed.session_disposition,
        authoritative.session_disposition);
    EXPECT_EQ(
        diagnosed.last_rejection_code,
        savor::runtime::WorkerRejectionCode::InvocationMismatch);
    EXPECT_EQ(
        diagnosed.last_error,
        "late invocation event was ignored");

    std::optional<savor::wrms::InvocationProgressPayload> observed_progress;
    std::optional<savor::wrms::InvocationTerminalPayload> observed_terminal;
    worker.set_invocation_progress_callback(
        [&](const auto& progress) { observed_progress = progress; });
    worker.set_invocation_terminal_callback(
        [&](const auto& terminal) { observed_terminal = terminal; });

    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::InvocationProgress,
        savor::wrms::InvocationProgressPayload{
            .invocation_id = 701,
            .attempt_id = 801,
            .ordinal = 1,
            .progress = {1, 2},
        }));
    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::InvocationTerminal,
        savor::wrms::InvocationTerminalPayload{
            .invocation_id = 701,
            .attempt_id = 801,
            .status = savor::wrms::InvocationTerminalStatus::Failed,
            .session_disposition =
                savor::wrms::SessionDispositionCode::Clean,
            .state_epoch = 10,
            .rejection_code =
                savor::wrms::RejectionCode::ProgramRuntimeUnavailable,
            .error_code = "program_runtime_unavailable",
            .message = "canonical ProgramRuntime is unavailable",
        }));

    ASSERT_TRUE(observed_progress.has_value());
    ASSERT_TRUE(observed_terminal.has_value());
    EXPECT_EQ(observed_progress->invocation_id, 701u);
    EXPECT_EQ(observed_progress->attempt_id, 801u);
    EXPECT_EQ(observed_terminal->invocation_id, 701u);
    EXPECT_EQ(observed_terminal->attempt_id, 801u);
}

TEST(ProcessWorkerV1, ExecutionStateUpdatesSnapshotAndUsesDedicatedCallback)
{
    savor::ProcessWorker worker;
    std::optional<savor::wrms::ExecutionStatePayload> observed;
    worker.set_execution_state_callback(
        [&](const auto& state) { observed = state; });

    const savor::wrms::ExecutionStatePayload running{
        .session_id = 81,
        .state_epoch = 12,
        .operation_id = 501,
        .activity =
            savor::wrms::ExecutionActivityCode::InteractiveRunning,
        .has_active_control = true,
        .active_control = savor::wrms::ExecutionControlKind::Resume,
        .completed_count = 3,
        .program_counter = 0x801dc288,
    };
    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::ExecutionState,
        running));

    ASSERT_TRUE(observed.has_value());
    EXPECT_EQ(*observed, running);
    const auto snapshot = worker.latest_snapshot();
    EXPECT_EQ(snapshot.session_id, savor::runtime::SessionId{81});
    EXPECT_EQ(snapshot.state_epoch, savor::runtime::StateEpoch{12});
    EXPECT_EQ(
        snapshot.execution_activity,
        savor::wrms::ExecutionActivityCode::InteractiveRunning);
    EXPECT_EQ(snapshot.execution_operation_id, 501u);
    ASSERT_TRUE(snapshot.active_execution_control.has_value());
    EXPECT_EQ(
        *snapshot.active_execution_control,
        savor::wrms::ExecutionControlKind::Resume);
    EXPECT_EQ(snapshot.execution_completed_count, 3u);
    EXPECT_EQ(snapshot.execution_program_counter, 0x801dc288u);
}

TEST(ProcessWorkerV1, BlockedWriteIsCancelledAndJoinedBeforeStdinCloseAndForce)
{
    const auto worker_path = FindBuiltWorker();
    if (worker_path.empty())
        GTEST_SKIP() << "SavorWorker.exe was not built beside the test outputs";

    std::latch write_entered{1};
    std::latch release_blocked_write{1};
    std::latch acceptance_closed{1};
    std::latch cancellation_requested{1};
    std::atomic<bool> writer_completed{false};
    std::atomic<bool> close_observed{false};
    std::atomic<bool> close_followed_writer{false};

    auto hooks = std::make_shared<savor::ProcessWorkerTestHooks>();
    hooks->stop_grace = std::chrono::milliseconds{25};
    hooks->stop_acceptance_closed = [&]() {
        acceptance_closed.count_down();
    };
    hooks->before_writer_write = [&](savor::wrms::MessageKind kind) {
        if (kind != savor::wrms::MessageKind::PrepareModule)
            return;
        write_entered.count_down();
        release_blocked_write.wait();
    };
    hooks->after_writer_write =
        [&](savor::wrms::MessageKind kind, bool) {
            if (kind == savor::wrms::MessageKind::PrepareModule)
                writer_completed.store(true, std::memory_order_release);
        };
    hooks->writer_cancel_requested = [&]() {
        cancellation_requested.count_down();
        release_blocked_write.count_down();
    };
    hooks->before_stdin_close = [&]() {
        close_followed_writer.store(
            writer_completed.load(std::memory_order_acquire),
            std::memory_order_release);
        close_observed.store(true, std::memory_order_release);
    };
    hooks->process_alive_override = []() {
        return std::optional<bool>{true};
    };

    savor::ProcessWorker worker{hooks};
    std::string launch_error;
    ASSERT_TRUE(worker.launch_and_negotiate(
        savor::ProcessLaunchOptions{
            .worker_id = 38,
            .exe_path = worker_path.string(),
            .hello_timeout_ms = 10000,
        },
        &launch_error)) << launch_error;

    std::atomic<bool> prepare_succeeded{true};
    std::promise<void> producer_finished;
    auto producer_finished_future = producer_finished.get_future();
    std::thread blocked_producer([&]() {
        const savor::runtime::EncodedModuleEnvelope module{
            .identity = {
                .canonical_id = "test.blocked-write",
                .revision = 1,
                .canonical_hash = "blocked-write-hash",
            },
            .format_version = 1,
            .payload = {1, 2, 3},
        };
        prepare_succeeded.store(
            worker.prepare_encoded_module(module, nullptr, 25),
            std::memory_order_release);
        producer_finished.set_value();
    });
    write_entered.wait();
    EXPECT_EQ(
        producer_finished_future.wait_for(std::chrono::seconds(1)),
        std::future_status::ready);

    std::thread stopper([&]() { worker.stop(); });
    acceptance_closed.wait();

    savor::wrms::CommandResultPayload post_stop_result;
    EXPECT_FALSE(worker.cancel_invocation(
        savor::runtime::InvocationId{500},
        "must be rejected after stop acceptance closes",
        &post_stop_result,
        1000));

    cancellation_requested.wait();
    stopper.join();
    blocked_producer.join();

    EXPECT_FALSE(prepare_succeeded.load(std::memory_order_acquire));
    EXPECT_TRUE(writer_completed.load(std::memory_order_acquire));
    EXPECT_TRUE(close_observed.load(std::memory_order_acquire));
    EXPECT_TRUE(close_followed_writer.load(std::memory_order_acquire));

    const auto first = worker.last_stop_snapshot();
    EXPECT_TRUE(first.was_running);
    EXPECT_EQ(first.stop_grace_ms, 25u);
    EXPECT_TRUE(first.deadline_expired);
    EXPECT_TRUE(first.shutdown_frame_attempted);
    EXPECT_FALSE(first.shutdown_frame_succeeded);
    EXPECT_FALSE(first.shutdown_result_received);
    EXPECT_TRUE(first.cancel_writer_attempted);
    EXPECT_TRUE(first.writer_joined);
    EXPECT_TRUE(first.stdin_close_attempted);
    EXPECT_TRUE(first.stdin_close_succeeded);
    EXPECT_TRUE(first.forced);
    EXPECT_TRUE(first.termination_attempted);
    EXPECT_TRUE(first.termination_succeeded);
    EXPECT_FALSE(first.termination_method.empty());
    EXPECT_FALSE(first.graceful);
    EXPECT_FALSE(worker.is_running());

    worker.stop();
    const auto second = worker.last_stop_snapshot();
    EXPECT_TRUE(second.already_stopping);
    EXPECT_TRUE(second.forced);
    EXPECT_TRUE(second.writer_joined);
}

TEST(ProcessWorkerV1, NegotiatesSliceThreeCapabilitiesCorrelatesConcurrentRequestsAndStopsGracefully)
{
    const auto worker_path = FindBuiltWorker();
    if (worker_path.empty())
        GTEST_SKIP() << "SavorWorker.exe was not built beside the test outputs";

    const savor::runtime::EncodedModuleEnvelope module{
        .identity = {
            .canonical_id = "test.full-envelope",
            .revision = 14,
            .canonical_hash = "module-hash-14",
        },
        .format_version = 3,
        .payload = {1, 2, 3},
    };
    const savor::runtime::EncodedInvocationEnvelope invocation{
        .invocation_id = savor::runtime::InvocationId{101},
        .attempt_id = savor::runtime::AttemptId{201},
        .module = module.identity,
        .entrypoint = "main",
        .expected_state_epoch = savor::runtime::StateEpoch{41},
        .input_payload = {4, 5, 6},
    };
    std::mutex observed_mutex;
    std::optional<savor::wrms::PrepareModulePayload> observed_module;
    std::optional<savor::wrms::SubmitInvocationPayload> observed_invocation;
    auto hooks = std::make_shared<savor::ProcessWorkerTestHooks>();
    hooks->observe_writer_frame =
        [&](savor::wrms::MessageKind kind,
            std::span<const std::uint8_t> bytes) {
            const auto decoded = savor::wrms::DecodeFrame(bytes, true);
            if (!decoded)
                return;
            std::lock_guard<std::mutex> lock(observed_mutex);
            if (kind == savor::wrms::MessageKind::PrepareModule)
            {
                savor::wrms::PrepareModulePayload payload;
                if (savor::wrms::DecodePayload(
                        decoded.frame.payload,
                        payload))
                {
                    observed_module = std::move(payload);
                }
            }
            else if (kind == savor::wrms::MessageKind::SubmitInvocation)
            {
                savor::wrms::SubmitInvocationPayload payload;
                if (savor::wrms::DecodePayload(
                        decoded.frame.payload,
                        payload))
                {
                    observed_invocation = std::move(payload);
                }
            }
        };

    savor::ProcessWorker worker{hooks};
    std::string launch_error;
    ASSERT_TRUE(worker.launch_and_negotiate(
        savor::ProcessLaunchOptions{
            .worker_id = 37,
            .exe_path = worker_path.string(),
            .hello_timeout_ms = 10000,
        },
        &launch_error)) << launch_error;

    const auto capabilities = worker.process_capabilities();
    EXPECT_TRUE(savor::runtime::HasCapability(
        capabilities,
        savor::runtime::WorkerCapability::SessionLifecycle));
    EXPECT_TRUE(savor::runtime::HasCapability(
        capabilities,
        savor::runtime::WorkerCapability::Screenshot));
    EXPECT_TRUE(savor::runtime::HasCapability(
        capabilities,
        savor::runtime::WorkerCapability::HostEvents));
    EXPECT_TRUE(savor::runtime::HasCapability(
        capabilities,
        savor::runtime::WorkerCapability::CancellationProtocol));
    EXPECT_TRUE(savor::runtime::HasCapability(
        capabilities,
        savor::runtime::WorkerCapability::Shutdown));
    EXPECT_FALSE(savor::runtime::HasCapability(
        capabilities,
        savor::runtime::WorkerCapability::ProgramInvocation));
    EXPECT_TRUE(savor::runtime::HasCapability(
        capabilities,
        savor::runtime::WorkerCapability::InteractiveVisualDebug));

    struct CommandOutcome
    {
        bool succeeded = false;
        savor::wrms::CommandResultPayload result;
    };
    CommandOutcome prepare;
    CommandOutcome invoke;
    CommandOutcome cancel;
    std::latch ready{3};
    std::latch go{1};

    std::thread prepare_thread([&]() {
        ready.count_down();
        go.wait();
        prepare.succeeded = worker.prepare_encoded_module(module, &prepare.result, 10000);
    });
    std::thread invoke_thread([&]() {
        ready.count_down();
        go.wait();
        invoke.succeeded = worker.submit_encoded_invocation(
            invocation,
            &invoke.result,
            10000);
    });
    std::thread cancel_thread([&]() {
        ready.count_down();
        go.wait();
        cancel.succeeded = worker.cancel_invocation(
            savor::runtime::InvocationId{301},
            "test cancellation",
            &cancel.result,
            10000);
    });

    ready.wait();
    go.count_down();
    prepare_thread.join();
    invoke_thread.join();
    cancel_thread.join();

    EXPECT_FALSE(prepare.succeeded);
    EXPECT_FALSE(invoke.succeeded);
    EXPECT_FALSE(cancel.succeeded);
    EXPECT_EQ(prepare.result.command_kind, savor::wrms::MessageKind::PrepareModule);
    EXPECT_EQ(invoke.result.command_kind, savor::wrms::MessageKind::SubmitInvocation);
    EXPECT_EQ(cancel.result.command_kind, savor::wrms::MessageKind::CancelInvocation);
    EXPECT_NE(prepare.result.status, savor::wrms::CommandStatus::Succeeded);
    EXPECT_NE(invoke.result.status, savor::wrms::CommandStatus::Succeeded);
    EXPECT_NE(cancel.result.status, savor::wrms::CommandStatus::Succeeded);

    const std::set<std::uint64_t> command_sequences{
        prepare.result.command_sequence,
        invoke.result.command_sequence,
        cancel.result.command_sequence,
    };
    EXPECT_EQ(command_sequences.size(), 3u);
    EXPECT_EQ(command_sequences.count(0), 0u);

    {
        std::lock_guard<std::mutex> lock(observed_mutex);
        ASSERT_TRUE(observed_module.has_value());
        ASSERT_TRUE(observed_invocation.has_value());
        EXPECT_EQ(
            *observed_module,
            (savor::wrms::PrepareModulePayload{
                .canonical_id = module.identity.canonical_id,
                .revision = module.identity.revision,
                .canonical_hash = module.identity.canonical_hash,
                .format_version = module.format_version,
                .encoded_module = module.payload,
            }));
        EXPECT_EQ(
            *observed_invocation,
            (savor::wrms::SubmitInvocationPayload{
                .invocation_id = invocation.invocation_id.value(),
                .attempt_id = invocation.attempt_id.value(),
                .module_canonical_id = invocation.module.canonical_id,
                .module_revision = invocation.module.revision,
                .module_canonical_hash =
                    invocation.module.canonical_hash,
                .entrypoint = invocation.entrypoint,
                .expected_state_epoch =
                    invocation.expected_state_epoch.value(),
                .encoded_invocation = invocation.input_payload,
            }));
    }

    worker.stop();
    const auto stop = worker.last_stop_snapshot();
    EXPECT_TRUE(stop.was_running);
    EXPECT_TRUE(stop.shutdown_frame_attempted);
    EXPECT_TRUE(stop.shutdown_frame_succeeded);
    EXPECT_TRUE(stop.shutdown_result_received);
    EXPECT_TRUE(stop.graceful);
    EXPECT_FALSE(stop.forced);
    EXPECT_FALSE(worker.is_running());
}
