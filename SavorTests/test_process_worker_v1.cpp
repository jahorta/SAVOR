#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
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
    ExpectErrorContains(worker, "interactive pause");
    ExpectErrorContains(worker, "Dependency Slice 3");

    EXPECT_FALSE(worker.visual_resume_emulation());
    ExpectErrorContains(worker, "interactive resume");
    ExpectErrorContains(worker, "Dependency Slice 3");

    EXPECT_FALSE(worker.visual_step_vm());
    ExpectErrorContains(worker, "interactive stepping");
    ExpectErrorContains(worker, "Dependency Slice 3");

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

TEST(ProcessWorkerV1, NegotiatesSliceOneCapabilitiesCorrelatesConcurrentRequestsAndStopsGracefully)
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
    EXPECT_FALSE(savor::runtime::HasCapability(
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
