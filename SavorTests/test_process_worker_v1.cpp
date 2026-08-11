#include <gtest/gtest.h>

#include "Phases/Programs/SeedProbe/SeedProbeModule.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "Runner/Runtime/RuntimeTypes.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "Runner/Runtime/ProgramRuntime/ProgramRuntime.h"
#include "Runner/Runtime/Worksets/WorksetWireCodec.h"
#include "Utils/Hash.h"
#include "Utils/ModulePath.h"
#include "Worker/ProcessWorker.h"
#include "serial_guard.h"

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
        worker.start_callback_dispatch();
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
        constexpr std::uint64_t request_id = 991;
        auto pending =
            std::make_shared<ProcessWorker::PendingResponse>();
        pending->request_kind = wrms::MessageKind::OpenSession;
        {
            std::lock_guard<std::mutex> lock(worker.pending_mutex_);
            worker.pending_.emplace(request_id, pending);
        }
        const bool delivered = DeliverPayload(
            worker,
            wrms::MessageKind::OpenSessionResult,
            result,
            request_id);
        {
            std::lock_guard<std::mutex> lock(worker.pending_mutex_);
            worker.pending_.erase(request_id);
        }
        return delivered;
    }

    static bool DeliverRaw(
        ProcessWorker& worker,
        wrms::MessageKind kind,
        std::span<const std::uint8_t> payload,
        std::uint64_t request_id = 0)
    {
        worker.handle_frame(wrms::FrameView{
            .header = {
                .kind = kind,
                .payload_size =
                    static_cast<std::uint32_t>(payload.size()),
                .request_id = request_id,
            },
            .payload = payload,
        });
        return worker.protocol_failed_.load(std::memory_order_acquire);
    }

    static bool DeliverMismatchedCommandResult(
        ProcessWorker& worker)
    {
        constexpr std::uint64_t request_id = 701;
        auto pending =
            std::make_shared<ProcessWorker::PendingResponse>();
        pending->request_kind = wrms::MessageKind::SubmitWorkset;
        {
            std::lock_guard<std::mutex> lock(worker.pending_mutex_);
            worker.pending_.emplace(request_id, pending);
        }
        const bool delivered = DeliverPayload(
            worker,
            wrms::MessageKind::CommandResult,
            wrms::CommandResultPayload{
                .command_kind = wrms::MessageKind::OpenSession,
                .status = wrms::CommandStatus::Succeeded,
            },
            request_id);
        {
            std::lock_guard<std::mutex> lock(worker.pending_mutex_);
            worker.pending_.erase(request_id);
        }
        return delivered &&
            worker.protocol_failed_.load(std::memory_order_acquire);
    }

    static bool CallbackDispatcherJoinable(
        const ProcessWorker& worker)
    {
        return worker.callback_dispatcher_.joinable();
    }

    static bool ProtocolFailed(const ProcessWorker& worker)
    {
        return worker.protocol_failed_.load(std::memory_order_acquire);
    }

    static void MarkNegotiatedTransportStopped(ProcessWorker& worker)
    {
        worker.ready_received_.store(true, std::memory_order_release);
        worker.ready_ok_.store(true, std::memory_order_release);
        worker.running_.store(false, std::memory_order_release);
    }

    static bool DeliverDuplicateCommandResult(
        ProcessWorker& worker,
        std::uint64_t request_id,
        wrms::MessageKind request_kind)
    {
        auto pending =
            std::make_shared<ProcessWorker::PendingResponse>();
        pending->request_kind = request_kind;
        {
            std::lock_guard<std::mutex> lock(worker.pending_mutex_);
            worker.pending_.emplace(request_id, pending);
        }
        const wrms::CommandResultPayload result{
            .command_kind = request_kind,
            .status = wrms::CommandStatus::Succeeded,
        };
        const bool first = DeliverPayload(
            worker,
            wrms::MessageKind::CommandResult,
            result,
            request_id);
        const bool second = DeliverPayload(
            worker,
            wrms::MessageKind::CommandResult,
            result,
            request_id);
        {
            std::lock_guard<std::mutex> lock(worker.pending_mutex_);
            worker.pending_.erase(request_id);
        }
        return first && second &&
            worker.protocol_failed_.load(std::memory_order_acquire);
    }

    static void ConfigureExpectedHello(
        ProcessWorker& worker,
        std::uint64_t worker_id,
        std::uint32_t process_id)
    {
        worker.worker_id_ = static_cast<std::size_t>(worker_id);
        worker.process_id_ = process_id;
    }

    static void SetBusyWorksetResidency(
        ProcessWorker& worker,
        runtime::WorkerWorksetId active,
        std::optional<runtime::WorkerWorksetId> staged = std::nullopt)
    {
        worker.busy_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(worker.snapshot_mutex_);
        worker.snapshot_.active_workset = active;
        worker.snapshot_.staged_workset = staged;
    }

    static bool Busy(const ProcessWorker& worker)
    {
        return worker.busy_.load(std::memory_order_acquire);
    }

    static void FailPendingTransport(ProcessWorker& worker)
    {
        worker.fail_all_pending();
    }

    static void AddPendingRequest(
        ProcessWorker& worker,
        std::uint64_t request_id,
        wrms::MessageKind request_kind)
    {
        auto pending =
            std::make_shared<ProcessWorker::PendingResponse>();
        pending->request_kind = request_kind;
        std::lock_guard<std::mutex> lock(worker.pending_mutex_);
        worker.pending_.emplace(request_id, std::move(pending));
    }

    static void DrainCallbacks(ProcessWorker& worker)
    {
        auto drained = std::make_shared<std::promise<void>>();
        auto future = drained->get_future();
        worker.enqueue_callback(
            [drained]() { drained->set_value(); });
        ASSERT_EQ(
            future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
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
        runtime::WorksetEpoch expected_workset_epoch,
        std::uint32_t requested_count,
        const wrms::ExecutionResultPayload& result,
        std::string* error_out = nullptr)
    {
        return ProcessWorker::validate_execution_result(
            control,
            session_id,
            expected_workset_epoch,
            requested_count,
            result,
            error_out);
    }

    static bool StartNegotiatedVisualTransport(
        ProcessWorker& worker,
        runtime::SessionId session_id,
        runtime::WorksetEpoch workset_epoch,
        runtime::WorkerWorksetId active_workset = {},
        runtime::WorkerWorksetItemId active_item = {})
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

        {
            std::lock_guard<std::mutex> lock(worker.snapshot_mutex_);
            worker.snapshot_.running = true;
            worker.snapshot_.hello_received = true;
            worker.snapshot_.session_open = true;
            worker.snapshot_.worker_mode =
                runtime::WorkerMode::VisualDebug;
            worker.snapshot_.session_id = session_id;
            worker.snapshot_.workset_epoch = workset_epoch;
            worker.snapshot_.worker_state = runtime::WorkerState::Ready;
            if (active_workset && active_item)
            {
                worker.snapshot_.active_workset = active_workset;
                worker.snapshot_.active_workset_item = active_item;
                worker.snapshot_.worker_state =
                    runtime::WorkerState::Running;
            }
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

savor::runtime::WorkerWorksetDefinition MakeTransportTestWorkset()
{
    using namespace savor::runtime;
    WorkerWorksetDefinition workset;
    workset.workset_id = WorkerWorksetId{5001};
    const auto phase = savor::runtime::seedprobe::
        SeedProbeFullPhaseDefinitionV2();
    workset.phase_invocation = {
        .invocation_id = {1, 5001},
        .program_package =
            savor::runtime::fullphase::BuildFullPhaseProgramPackage(*phase),
        .common_input = savor::runtime::fullphase::MakeFullPhaseCommonInput(
            "soa.seed_probe.CommonInput", 1),
    };
    workset.baseline.artifact = ProgramBaselineArtifact{
        .kind = ProgramBaselineArtifactKind::Savestate,
        .state_path = "transport.sav",
        .state_sha256 = std::string(64, 'c'),
        .compatibility = {
            "GEAE8E",
            std::string(64, 'd'),
            "dolphin-2506a",
            "test"},
        .lineage = {.edge = "transport", .producer = "test"},
    };
    workset.baseline.lineage =
        phase->runtime_contract().baseline_lineage;
    workset.execution_key.module =
        phase->runtime_contract().module;
    workset.execution_key.entrypoint =
        phase->runtime_contract().entrypoint;
    workset.execution_key.verified_dependency_sha256 =
        phase->runtime_contract().verified_dependency_sha256;
    workset.execution_key.runtime_profile_sha256 =
        phase->runtime_contract().runtime_profile_sha256;
    workset.execution_key.baseline =
        ComputeProgramBaselineKey(workset.baseline);
    workset.execution_key.movie_policy_sha256 =
        phase->runtime_contract().movie_policy_sha256;
    workset.execution_key.service_policy_sha256 =
        phase->runtime_contract().service_policy_sha256;
    workset.execution_key.program_package_sha256 =
        workset.phase_invocation.program_package.canonical_sha256;
    workset.execution_key.common_input_sha256 =
        workset.phase_invocation.common_input.content_sha256;
    workset.execution_key.capture_binding_sha256 =
        EmptyWorksetCaptureBindingHashV1();
    workset.execution_key.progress_plan_sha256 =
        workset.progress_plan.content_sha256;
    workset.execution_key.canonical_sha256 =
        ComputeWorkerWorksetExecutionKeyHash(
            workset.execution_key);
    workset.items.push_back(WorksetItemTemplate{
        .item_id = WorkerWorksetItemId{5002},
        .ordinal = 0,
        .execution = {
            .execution_id = ProgramExecutionId{5003},
            .attempt_id = AttemptId{5004},
            .input_payload =
                savor::runtime::seedprobe::
                    EncodeSeedProbeExecutionInputV2(
                    {savor::GCInputFrame{}}),
        },
        .declared_terminal_bytes = 4096,
    });
    return workset;
}

class ScopedTemporaryDirectory
{
public:
    explicit ScopedTemporaryDirectory(std::string_view label)
    {
        const auto nonce = std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
        path_ = std::filesystem::temp_directory_path() /
            ("savor_" + std::string(label) + "_" +
             std::to_string(nonce));
        std::filesystem::create_directories(path_);
    }

    ~ScopedTemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    ScopedTemporaryDirectory(const ScopedTemporaryDirectory&) = delete;
    ScopedTemporaryDirectory& operator=(
        const ScopedTemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::string Sha256FileStreaming(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error(
            "Could not open fixture for SHA-256: " +
            path.string());
    }

    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    const auto release = [&context]() {
        mbedtls_sha256_free(&context);
    };
    if (mbedtls_sha256_starts_ret(&context, 0) != 0)
    {
        release();
        throw std::runtime_error("Could not initialize SHA-256");
    }
    std::vector<unsigned char> bytes(1024 * 1024);
    while (input)
    {
        input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        const std::streamsize count = input.gcount();
        if (count > 0 &&
            mbedtls_sha256_update_ret(
                &context,
                bytes.data(),
                static_cast<std::size_t>(count)) != 0)
        {
            release();
            throw std::runtime_error("Could not update SHA-256");
        }
    }
    if (!input.eof())
    {
        release();
        throw std::runtime_error(
            "Could not read fixture for SHA-256: " +
            path.string());
    }
    std::array<unsigned char, 32> digest{};
    if (mbedtls_sha256_finish_ret(
            &context,
            digest.data()) != 0)
    {
        release();
        throw std::runtime_error("Could not finalize SHA-256");
    }
    release();

    static constexpr char kHex[] = "0123456789abcdef";
    std::string result(64, '\0');
    for (std::size_t index = 0; index < digest.size(); ++index)
    {
        result[index * 2] = kHex[digest[index] >> 4];
        result[index * 2 + 1] =
            kHex[digest[index] & 0x0f];
    }
    return result;
}

struct DevelopmentNoEffectProgram
{
    savor::runtime::EncodedModuleEnvelope module;
    savor::runtime::program::ProgramInvocation invocation;
    std::vector<std::uint8_t> encoded_invocation;
};

DevelopmentNoEffectProgram MakeDevelopmentNoEffectProgram(
    std::string baseline_lineage)
{
    using namespace savor::runtime;
    using namespace savor::runtime::program;

    const ProgramBudgets budgets{
        .maximum_instructions = 16,
        .maximum_calls = 4,
        .maximum_call_depth = 2,
        .maximum_action_requests = 1,
        .maximum_emissions = 1,
        .maximum_artifacts = 1,
        .maximum_values = 16,
        .maximum_value_bytes = 1024,
        .maximum_trace_events = 16,
    };
    const ProgramPolicySet policies{
        .state_policies = {
            InvocationStatePolicy::RestoreBaseline},
        .execution_intents = {ExecutionIntent::Live},
    };
    ProgramModule module{
        .identity = {
            .canonical_id = "test.workset.no_effect/1",
            .revision = 1,
        },
        .ir_version = kCanonicalIrVersionV1,
        .entrypoints = {
            ProgramEntrypoint{
                .name = "run",
                .function = ProgramFunctionId{1},
                .input_type =
                    TypeRef::Builtin(BuiltinType::U32),
                .output_type =
                    TypeRef::Builtin(BuiltinType::U32),
                .domain_outcome_type =
                    TypeRef::Builtin(BuiltinType::Bool),
                .accepted_policies = policies,
            },
        },
        .functions = {
            ProgramFunction{
                .id = ProgramFunctionId{1},
                .name = "run",
                .arguments = {
                    ValueDefinition{
                        .id = ProgramValueId{1},
                        .type =
                            TypeRef::Builtin(BuiltinType::U32),
                    },
                },
                .output_type =
                    TypeRef::Builtin(BuiltinType::U32),
                .domain_outcome_type =
                    TypeRef::Builtin(BuiltinType::Bool),
                .entry_block = ProgramBlockId{1},
                .blocks = {
                    BasicBlock{
                        .id = ProgramBlockId{1},
                        .instructions = {
                            Instruction{
                                .id = ProgramInstructionId{1},
                                .opcode =
                                    InstructionOpcode::Constant,
                                .source_location =
                                    ProgramSourceLocationId{1},
                                .result = ValueDefinition{
                                    .id = ProgramValueId{2},
                                    .type = TypeRef::Builtin(
                                        BuiltinType::Bool),
                                },
                                .literal = LiteralValue{
                                    .type = TypeRef::Builtin(
                                        BuiltinType::Bool),
                                    .payload = true,
                                },
                            },
                        },
                        .terminator = Terminator{
                            .kind = TerminatorKind::Return,
                            .source_location =
                                ProgramSourceLocationId{2},
                            .return_value = ProgramValueId{1},
                            .domain_outcome = ProgramValueId{2},
                        },
                    },
                },
                .exported = true,
            },
        },
        .accepted_policies = policies,
        .budgets = budgets,
        .source_map = {
            .version = 1,
            .entries = {
                SourceMapEntry{
                    .id = ProgramSourceLocationId{1},
                    .function = ProgramFunctionId{1},
                    .block = ProgramBlockId{1},
                    .instruction = ProgramInstructionId{1},
                    .source_name =
                        "process-worker-workset-smoke",
                    .semantic_path = "run.domain",
                },
                SourceMapEntry{
                    .id = ProgramSourceLocationId{2},
                    .function = ProgramFunctionId{1},
                    .block = ProgramBlockId{1},
                    .source_name =
                        "process-worker-workset-smoke",
                    .semantic_path = "run.return",
                },
            },
        },
    };
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    const EncodeResult encoded_module =
        EncodeProgramModuleV1(module);
    if (!encoded_module)
    {
        throw std::runtime_error(
            "Could not encode the development no-effect module: " +
            encoded_module.status.message);
    }

    ProgramInvocation invocation{
        .invocation_id = InvocationId{7001},
        .attempt_id = AttemptId{8001},
        .module = module.identity,
        .entrypoint = "run",
        .dependencies = {
            .ir_version = kCanonicalIrVersionV1,
        },
        .runtime_profile = {
            .profile_id = "soa-usa-jit64-v1",
            .game_id = std::string(
                capabilities::kSupportedGameId),
            .disc_identity = std::string(
                capabilities::kSupportedGameId),
            .executable_identity = std::string(
                capabilities::kSupportedExecutableIdentity),
            .backend = "jit64",
        },
        .state = {
            .policy = InvocationStatePolicy::RestoreBaseline,
            .session_lineage = std::move(baseline_lineage),
        },
        .execution = {
            .intent = ExecutionIntent::Live,
        },
        .input = {
            .root = ProgramValueId{1},
            .values = {
                ProgramValue{
                    .id = ProgramValueId{1},
                    .type =
                        TypeRef::Builtin(BuiltinType::U32),
                    .payload = std::uint32_t{0},
                },
            },
        },
        .limits = budgets,
        .provenance = {
            .requesting_component =
                "ProcessWorkerV1.RealWorksetSmoke",
        },
    };
    const EncodeResult encoded_invocation =
        EncodeProgramInvocationV1(invocation);
    if (!encoded_invocation)
    {
        throw std::runtime_error(
            "Could not encode the development no-effect invocation: " +
            encoded_invocation.status.message);
    }

    return {
        .module = {
            .identity = {
                .canonical_id = module.identity.canonical_id,
                .revision = module.identity.revision,
                .canonical_hash =
                    module.identity.module_hash.ToHex(),
            },
            .format_version = kProgramCodecVersionV1,
            .development_only = true,
            .payload = encoded_module.bytes,
        },
        .invocation = std::move(invocation),
        .encoded_invocation = encoded_invocation.bytes,
    };
}

void ExpectErrorContains(const savor::ProcessWorker& worker, const char* expected)
{
    const auto error = worker.last_error();
    EXPECT_NE(error.find(expected), std::string::npos) << error;
}

} // namespace

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

TEST(ProcessWorkerV1, ConcurrentStopWaitsForTheOwningStopToComplete)
{
    std::latch first_stop_entered{1};
    std::latch release_first_stop{1};
    auto hooks = std::make_shared<savor::ProcessWorkerTestHooks>();
    hooks->stop_acceptance_closed = [&]()
    {
        first_stop_entered.count_down();
        release_first_stop.wait();
    };
    savor::ProcessWorker worker{hooks};

    std::thread first([&]() { worker.stop(); });
    first_stop_entered.wait();

    std::promise<void> second_returned;
    auto second_future = second_returned.get_future();
    std::thread second([&]()
    {
        worker.stop();
        second_returned.set_value();
    });
    EXPECT_EQ(
        second_future.wait_for(std::chrono::milliseconds(25)),
        std::future_status::timeout);

    release_first_stop.count_down();
    first.join();
    second.join();
    EXPECT_EQ(
        second_future.wait_for(std::chrono::seconds(1)),
        std::future_status::ready);
    EXPECT_TRUE(worker.last_stop_snapshot().already_stopping);
}

TEST(
    ProcessWorkerV1,
    CallbackCleanupGraceIsReportedSeparatelyButStillJoinsSafely)
{
    auto hooks = std::make_shared<savor::ProcessWorkerTestHooks>();
    hooks->callback_cleanup_grace = std::chrono::milliseconds(1);
    savor::ProcessWorker worker{hooks};

    std::latch callback_entered{1};
    std::latch release_callback{1};
    worker.set_workset_credits_callback(
        [&](const savor::wrms::WorksetCreditsPayload&)
        {
            callback_entered.count_down();
            release_callback.wait();
        });
    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::WorksetCredits,
        savor::wrms::WorksetCreditsPayload{
            .outbound_sequence = 1}));
    callback_entered.wait();

    std::promise<void> stop_returned;
    auto stop_future = stop_returned.get_future();
    std::thread stopper([&]()
    {
        worker.stop();
        stop_returned.set_value();
    });
    EXPECT_EQ(
        stop_future.wait_for(std::chrono::milliseconds(25)),
        std::future_status::timeout);
    release_callback.count_down();
    stopper.join();

    const auto stopped = worker.last_stop_snapshot();
    EXPECT_TRUE(stopped.callback_cleanup_timed_out);
    EXPECT_TRUE(stopped.callback_dispatcher_joined);
    EXPECT_FALSE(stopped.graceful);
}

TEST(ProcessWorkerV1, ExecutionControlsFailLocallyOutsideVisualDebugMode)
{
    savor::ProcessWorker worker;
    const auto workset = savor::runtime::WorkerWorksetId{91};
    const auto item = savor::runtime::WorkerWorksetItemId{4};

    EXPECT_FALSE(worker.pause_guest_execution(workset, item));
    ExpectErrorContains(worker, "reserved for VisualDebug workers");
    EXPECT_FALSE(worker.resume_guest_execution(workset, item));
    ExpectErrorContains(worker, "reserved for VisualDebug workers");
    EXPECT_FALSE(worker.step_guest_frames(workset, item, 1));
    ExpectErrorContains(worker, "reserved for VisualDebug workers");
}

TEST(ProcessWorkerV1, LivenessProbeUsesTheDirectCommandResultPath)
{
    savor::ProcessWorker* worker_ptr = nullptr;
    auto hooks = std::make_shared<savor::ProcessWorkerTestHooks>();
    hooks->observe_writer_frame =
        [&](savor::wrms::MessageKind kind,
            std::span<const std::uint8_t> bytes)
        {
            if (kind != savor::wrms::MessageKind::LivenessProbe
                || worker_ptr == nullptr)
            {
                return;
            }
            const auto frame =
                savor::wrms::DecodeFrame(bytes, true);
            ASSERT_TRUE(frame);
            ASSERT_TRUE(
                savor::ProcessWorkerTestPeer::DeliverPayload(
                    *worker_ptr,
                    savor::wrms::MessageKind::CommandResult,
                    savor::wrms::CommandResultPayload{
                        .command_sequence = 1,
                        .command_kind =
                            savor::wrms::MessageKind::LivenessProbe,
                        .status =
                            savor::wrms::CommandStatus::Succeeded},
                    frame.frame.header.request_id));
        };

    savor::ProcessWorker worker{hooks};
    worker_ptr = &worker;
    ASSERT_TRUE(
        savor::ProcessWorkerTestPeer::StartNegotiatedVisualTransport(
            worker,
            savor::runtime::SessionId{55},
            savor::runtime::WorksetEpoch{4}));

    savor::wrms::CommandResultPayload result;
    EXPECT_TRUE(worker.probe_liveness(&result, 1000));
    EXPECT_EQ(
        result.command_kind,
        savor::wrms::MessageKind::LivenessProbe);
    EXPECT_EQ(
        result.status,
        savor::wrms::CommandStatus::Succeeded);

    savor::ProcessWorkerTestPeer::StopNegotiatedVisualTransport(
        worker);
}

TEST(
    ProcessWorkerV1,
    WorksetSubmitOutcomeDistinguishesLocalRejectionAcceptedAndAmbiguous)
{
    {
        savor::ProcessWorker worker;
        const auto outcome =
            worker.submit_workset_with_outcome(
                savor::runtime::WorkerWorksetDefinition{});
        EXPECT_EQ(
            outcome.disposition,
            savor::ProcessWorksetSubmitDisposition::DefiniteRejected);
        EXPECT_FALSE(outcome.request_frame_written);
        EXPECT_FALSE(outcome.correlated_result_received);
    }

    {
        savor::ProcessWorker* worker_ptr = nullptr;
        auto hooks = std::make_shared<savor::ProcessWorkerTestHooks>();
        hooks->observe_writer_frame =
            [&](savor::wrms::MessageKind kind,
                std::span<const std::uint8_t> bytes)
            {
                if (kind != savor::wrms::MessageKind::SubmitWorkset ||
                    !worker_ptr)
                {
                    return;
                }
                const auto frame =
                    savor::wrms::DecodeFrame(bytes, true);
                ASSERT_TRUE(frame);
                savor::wrms::SubmitWorksetPayload submit;
                ASSERT_TRUE(savor::wrms::DecodePayload(
                    frame.frame.payload,
                    submit));
                ASSERT_EQ(submit.cancellation_sidecar_version, 1u);
                ASSERT_EQ(
                    submit.initially_cancelled_item_ids,
                    (std::vector<std::uint64_t>{5002}));
                std::vector<std::uint8_t> typed_result;
                ASSERT_TRUE(savor::wrms::EncodePayload(
                    savor::wrms::SubmitWorksetResultPayload{
                        .workset_id = 5001,
                        .sidecar_version = 1,
                        .applied_item_count = 1,
                        .applied_sidecar_sha256 =
                            submit.cancellation_sidecar_sha256,
                        .already_admitted = false,
                    },
                    typed_result));
                ASSERT_TRUE(
                    savor::ProcessWorkerTestPeer::DeliverPayload(
                        *worker_ptr,
                        savor::wrms::MessageKind::CommandResult,
                        savor::wrms::CommandResultPayload{
                            .command_sequence = 1,
                            .command_kind =
                                savor::wrms::MessageKind::SubmitWorkset,
                            .status =
                                savor::wrms::CommandStatus::Succeeded,
                            .result = std::move(typed_result)},
                        frame.frame.header.request_id));
            };
        savor::ProcessWorker worker{hooks};
        worker_ptr = &worker;
        ASSERT_TRUE(
            savor::ProcessWorkerTestPeer::StartNegotiatedVisualTransport(
                worker,
                savor::runtime::SessionId{55},
                savor::runtime::WorksetEpoch{4}));
        const auto workset = MakeTransportTestWorkset();
        const savor::runtime::InitialWorksetCancellationSidecarV1
            sidecar{
                .workset_id = workset.workset_id,
                .item_ids = {
                    savor::runtime::WorkerWorksetItemId{5002}},
            };
        const auto outcome = worker.submit_workset_with_outcome(
            workset,
            sidecar,
            1000);
        EXPECT_EQ(
            outcome.disposition,
            savor::ProcessWorksetSubmitDisposition::Accepted);
        EXPECT_TRUE(outcome.request_frame_written);
        EXPECT_TRUE(outcome.correlated_result_received);
        savor::wrms::SubmitWorksetResultPayload applied;
        ASSERT_TRUE(savor::wrms::DecodePayload(
            outcome.result.result,
            applied));
        EXPECT_EQ(applied.workset_id, 5001u);
        EXPECT_EQ(applied.applied_item_count, 1u);
        EXPECT_EQ(
            applied.applied_sidecar_sha256,
            savor::runtime::
                ComputeInitialWorksetCancellationSidecarSha256(
                    sidecar));
        savor::ProcessWorkerTestPeer::StopNegotiatedVisualTransport(
            worker);
    }

    {
        savor::ProcessWorker* worker_ptr = nullptr;
        auto hooks = std::make_shared<savor::ProcessWorkerTestHooks>();
        hooks->observe_writer_frame =
            [&](savor::wrms::MessageKind kind,
                std::span<const std::uint8_t> bytes)
            {
                if (kind != savor::wrms::MessageKind::SubmitWorkset ||
                    !worker_ptr)
                {
                    return;
                }
                const auto frame =
                    savor::wrms::DecodeFrame(bytes, true);
                ASSERT_TRUE(frame);
                ASSERT_TRUE(
                    savor::ProcessWorkerTestPeer::DeliverPayload(
                        *worker_ptr,
                        savor::wrms::MessageKind::CommandResult,
                        savor::wrms::CommandResultPayload{
                            .command_sequence = 1,
                            .command_kind =
                                savor::wrms::MessageKind::SubmitWorkset,
                            .status =
                                savor::wrms::CommandStatus::Rejected,
                            .rejection_code =
                                savor::wrms::RejectionCode::CapacityExceeded,
                            .error_code = "CapacityExceeded",
                            .message = "resident item capacity exhausted"},
                        frame.frame.header.request_id));
            };
        savor::ProcessWorker worker{hooks};
        worker_ptr = &worker;
        ASSERT_TRUE(
            savor::ProcessWorkerTestPeer::StartNegotiatedVisualTransport(
                worker,
                savor::runtime::SessionId{55},
                savor::runtime::WorksetEpoch{4}));
        const auto outcome =
            worker.submit_workset_with_outcome(
                MakeTransportTestWorkset(),
                1000);
        EXPECT_EQ(
            outcome.disposition,
            savor::ProcessWorksetSubmitDisposition::DefiniteRejected);
        EXPECT_TRUE(outcome.request_frame_written);
        EXPECT_TRUE(outcome.correlated_result_received);
        EXPECT_EQ(
            outcome.result.rejection_code,
            savor::wrms::RejectionCode::CapacityExceeded);
        savor::ProcessWorkerTestPeer::StopNegotiatedVisualTransport(
            worker);
    }

    {
        savor::ProcessWorker* worker_ptr = nullptr;
        auto hooks = std::make_shared<savor::ProcessWorkerTestHooks>();
        hooks->after_writer_write =
            [&](savor::wrms::MessageKind kind, bool succeeded)
            {
                if (kind ==
                        savor::wrms::MessageKind::SubmitWorkset &&
                    succeeded &&
                    worker_ptr)
                {
                    savor::ProcessWorkerTestPeer::FailPendingTransport(
                        *worker_ptr);
                }
            };
        savor::ProcessWorker worker{hooks};
        worker_ptr = &worker;
        ASSERT_TRUE(
            savor::ProcessWorkerTestPeer::StartNegotiatedVisualTransport(
                worker,
                savor::runtime::SessionId{55},
                savor::runtime::WorksetEpoch{4}));
        const auto outcome =
            worker.submit_workset_with_outcome(
                MakeTransportTestWorkset(),
                1000);
        EXPECT_EQ(
            outcome.disposition,
            savor::ProcessWorksetSubmitDisposition::AmbiguousAfterWrite);
        EXPECT_TRUE(outcome.request_frame_written);
        EXPECT_FALSE(outcome.correlated_result_received);
        savor::ProcessWorkerTestPeer::StopNegotiatedVisualTransport(
            worker);
    }
}

TEST(
    ProcessWorkerV1,
    WorksetTerminalCallbackRunsOffReaderPathAndCanSynchronouslyAcknowledge)
{
    savor::ProcessWorker* worker_ptr = nullptr;
    auto hooks = std::make_shared<savor::ProcessWorkerTestHooks>();
    hooks->observe_writer_frame =
        [&](savor::wrms::MessageKind kind,
            std::span<const std::uint8_t> bytes) {
            if (kind !=
                    savor::wrms::MessageKind::AcknowledgeTerminal ||
                !worker_ptr)
                return;
            const auto frame = savor::wrms::DecodeFrame(bytes, true);
            ASSERT_TRUE(frame);
            savor::wrms::AcknowledgeTerminalPayload acknowledgement;
            ASSERT_TRUE(savor::wrms::DecodePayload(
                frame.frame.payload,
                acknowledgement));
            EXPECT_EQ(acknowledgement.terminal_id, 900u);
            EXPECT_EQ(acknowledgement.workset_id, 500u);
            EXPECT_EQ(acknowledgement.item_id, 3u);
            EXPECT_EQ(acknowledgement.item_ordinal, 2u);
            EXPECT_EQ(acknowledgement.invocation_id, 101u);
            EXPECT_EQ(acknowledgement.attempt_id, 7u);
            EXPECT_EQ(acknowledgement.terminal_order, 44u);
            ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
                *worker_ptr,
                savor::wrms::MessageKind::CommandResult,
                savor::wrms::CommandResultPayload{
                    .command_sequence = 77,
                    .command_kind =
                        savor::wrms::MessageKind::AcknowledgeTerminal,
                    .status =
                        savor::wrms::CommandStatus::Succeeded,
                },
                frame.frame.header.request_id));
        };

    savor::ProcessWorker worker{hooks};
    worker_ptr = &worker;
    ASSERT_TRUE(
        savor::ProcessWorkerTestPeer::StartNegotiatedVisualTransport(
            worker,
            savor::runtime::SessionId{55},
            savor::runtime::WorksetEpoch{4}));

    const std::thread::id delivery_thread = std::this_thread::get_id();
    auto callback_result =
        std::make_shared<std::promise<std::pair<bool, bool>>>();
    auto callback_future = callback_result->get_future();
    worker.set_workset_item_terminal_callback(
        [&](const savor::wrms::WorksetItemTerminalPayload& terminal) {
            savor::runtime::WorkerItemTerminalCorrelation correlation{
                savor::runtime::WorkerWorksetId{terminal.workset_id},
                savor::runtime::WorkerWorksetItemId{terminal.item_id},
                terminal.item_ordinal,
                savor::runtime::InvocationId{terminal.invocation_id},
                savor::runtime::AttemptId{terminal.attempt_id},
                savor::runtime::WorkerTerminalId{terminal.terminal_id},
                savor::runtime::WorkerTerminalOrder{
                    terminal.terminal_order},
            };
            const bool off_delivery_thread =
                std::this_thread::get_id() != delivery_thread;
            const bool acknowledged =
                worker.acknowledge_terminal(correlation, nullptr, 1000);
            callback_result->set_value(
                {off_delivery_thread, acknowledged});
        });

    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::WorksetItemTerminal,
        savor::wrms::WorksetItemTerminalPayload{
            .outbound_sequence = 1,
            .workset_id = 500,
            .item_id = 3,
            .item_ordinal = 2,
            .invocation_id = 101,
            .attempt_id = 7,
            .terminal_id = 900,
            .terminal_order = 44,
            .status =
                savor::wrms::InvocationTerminalStatus::Succeeded,
            .session_disposition =
                savor::wrms::SessionDispositionCode::Clean,
            .workset_epoch = 4,
        }));
    ASSERT_EQ(
        callback_future.wait_for(std::chrono::seconds(2)),
        std::future_status::ready);
    const auto [off_delivery_thread, acknowledged] =
        callback_future.get();
    EXPECT_TRUE(off_delivery_thread);
    EXPECT_TRUE(acknowledged);

    savor::ProcessWorkerTestPeer::StopNegotiatedVisualTransport(worker);
}

TEST(
    ProcessWorkerV1,
    WorksetEventsRequireExactMonotonicOutboundSequence)
{
    savor::ProcessWorker worker;
    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::WorksetCredits,
        savor::wrms::WorksetCreditsPayload{
            .outbound_sequence = 1,
            .available_item_credits = 4,
        }));
    EXPECT_EQ(worker.latest_snapshot().last_outbound_sequence, 1u);

    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::WorksetCredits,
        savor::wrms::WorksetCreditsPayload{
            .outbound_sequence = 1,
            .available_item_credits = 3,
        }));
    EXPECT_TRUE(worker.is_failed());
    ExpectErrorContains(worker, "duplicated");
}

TEST(
    ProcessWorkerV1,
    TaintedSessionEventIsPublishedToTheAuthoritativeCallback)
{
    savor::ProcessWorker worker;
    std::optional<savor::wrms::SessionEventPayload> observed;
    worker.set_session_event_callback(
        [&](const savor::wrms::SessionEventPayload& payload)
        {
            observed = payload;
        });

    const savor::wrms::SessionEventPayload event{
        .event_type = savor::wrms::SessionEventType::Tainted,
        .session_id = 41,
        .workset_epoch = 7,
        .worker_state = savor::wrms::WorkerStateCode::Tainted,
        .session_disposition =
            savor::wrms::SessionDispositionCode::Tainted,
        .rejection_code = savor::wrms::RejectionCode::SessionTainted,
        .message = "exact runtime taint",
    };
    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::SessionEvent,
        event));
    savor::ProcessWorkerTestPeer::DrainCallbacks(worker);

    ASSERT_TRUE(observed.has_value());
    EXPECT_EQ(*observed, event);
    const auto snapshot = worker.latest_snapshot();
    EXPECT_EQ(snapshot.worker_state, savor::runtime::WorkerState::Tainted);
    EXPECT_EQ(
        snapshot.session_disposition,
        savor::runtime::SessionDisposition::Tainted);
    EXPECT_EQ(snapshot.last_error, "exact runtime taint");
}

TEST(ProcessWorkerV1, DuplicateCorrelatedCommandResultFailsProtocol)
{
    savor::ProcessWorker worker;
    EXPECT_TRUE(
        savor::ProcessWorkerTestPeer::DeliverDuplicateCommandResult(
            worker,
            77,
            savor::wrms::MessageKind::SubmitWorkset));
    ExpectErrorContains(worker, "exact pending request");
}

TEST(ProcessWorkerV1, NegotiatedButExitedTransportIsNotReady)
{
    savor::ProcessWorker worker;
    savor::ProcessWorkerTestPeer::MarkNegotiatedTransportStopped(
        worker);
    EXPECT_FALSE(worker.is_ready());
    EXPECT_TRUE(worker.is_failed());
}

TEST(
    ProcessWorkerV1,
    WorksetEventsRejectZeroGapAndRegression)
{
    {
        savor::ProcessWorker worker;
        ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
            worker,
            savor::wrms::MessageKind::WorksetCredits,
            savor::wrms::WorksetCreditsPayload{
                .outbound_sequence = 0}));
        EXPECT_TRUE(worker.is_failed());
        ExpectErrorContains(worker, "zero");
    }
    {
        savor::ProcessWorker worker;
        ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
            worker,
            savor::wrms::MessageKind::WorksetCredits,
            savor::wrms::WorksetCreditsPayload{
                .outbound_sequence = 2}));
        EXPECT_TRUE(worker.is_failed());
        ExpectErrorContains(worker, "gap");
    }
    {
        savor::ProcessWorker worker;
        ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
            worker,
            savor::wrms::MessageKind::WorksetCredits,
            savor::wrms::WorksetCreditsPayload{
                .outbound_sequence = 1}));
        ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
            worker,
            savor::wrms::MessageKind::WorksetCredits,
            savor::wrms::WorksetCreditsPayload{
                .outbound_sequence = 2}));
        ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
            worker,
            savor::wrms::MessageKind::WorksetCredits,
            savor::wrms::WorksetCreditsPayload{
                .outbound_sequence = 1}));
        EXPECT_TRUE(worker.is_failed());
        ExpectErrorContains(worker, "regressed");
    }
}

TEST(
    ProcessWorkerV1,
    AuthoritativeWorksetCallbackPressureFailsInsteadOfDropping)
{
    savor::ProcessWorker worker;
    std::promise<void> callback_entered;
    auto callback_entered_future = callback_entered.get_future();
    std::latch release_callback{1};
    std::atomic<bool> first_callback{true};
    worker.set_workset_credits_callback(
        [&](const savor::wrms::WorksetCreditsPayload&)
        {
            if (first_callback.exchange(false, std::memory_order_acq_rel))
            {
                callback_entered.set_value();
                release_callback.wait();
            }
        });

    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::WorksetCredits,
        savor::wrms::WorksetCreditsPayload{
            .outbound_sequence = 1}));
    const auto entered =
        callback_entered_future.wait_for(std::chrono::seconds(1));
    if (entered != std::future_status::ready)
    {
        release_callback.count_down();
        FAIL() << "callback dispatcher did not start";
    }

    for (std::uint64_t sequence = 2; sequence <= 129; ++sequence)
    {
        ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
            worker,
            savor::wrms::MessageKind::WorksetCredits,
            savor::wrms::WorksetCreditsPayload{
                .outbound_sequence = sequence}));
    }
    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::WorksetCredits,
        savor::wrms::WorksetCreditsPayload{
            .outbound_sequence = 130}));
    EXPECT_TRUE(worker.is_failed());
    ExpectErrorContains(worker, "callback queue is full");
    release_callback.count_down();
}

TEST(ProcessWorkerV1, ThrowingAuthoritativeCallbackFailsProtocol)
{
    savor::ProcessWorker worker;
    worker.set_workset_credits_callback(
        [](const savor::wrms::WorksetCreditsPayload&)
        {
            throw std::runtime_error("authoritative callback failure");
        });
    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::WorksetCredits,
        savor::wrms::WorksetCreditsPayload{
            .outbound_sequence = 1}));
    savor::ProcessWorkerTestPeer::DrainCallbacks(worker);
    EXPECT_TRUE(savor::ProcessWorkerTestPeer::ProtocolFailed(worker));
    ExpectErrorContains(worker, "authoritative worker event callback threw");
}

TEST(ProcessWorkerV1, ThrowingPassiveCallbackDoesNotFailProtocol)
{
    savor::ProcessWorker worker;
    worker.set_host_event_callback(
        [](const savor::wrms::HostEventPayload&)
        {
            throw std::runtime_error("passive callback failure");
        });
    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::HostEvent,
        savor::wrms::HostEventPayload{
            .name = "passive"}));
    savor::ProcessWorkerTestPeer::DrainCallbacks(worker);
    EXPECT_FALSE(savor::ProcessWorkerTestPeer::ProtocolFailed(worker));
    ExpectErrorContains(worker, "passive worker event callback threw");
}

TEST(
    ProcessWorkerV1,
    OlderWorksetSummaryDoesNotReleaseSlotWhileSuccessorIsResident)
{
    savor::ProcessWorker worker;
    savor::ProcessWorkerTestPeer::SetBusyWorksetResidency(
        worker,
        savor::runtime::WorkerWorksetId{10},
        savor::runtime::WorkerWorksetId{20});

    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::WorksetSummary,
        savor::wrms::WorksetSummaryPayload{
            .outbound_sequence = 1,
            .workset_id = 10,
            .item_count = 1,
            .completed_count = 1}));
    EXPECT_TRUE(savor::ProcessWorkerTestPeer::Busy(worker));
    EXPECT_EQ(
        worker.latest_snapshot().staged_workset,
        savor::runtime::WorkerWorksetId{20});

    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::WorksetSummary,
        savor::wrms::WorksetSummaryPayload{
            .outbound_sequence = 2,
            .workset_id = 20,
            .item_count = 1,
            .completed_count = 1}));
    EXPECT_FALSE(savor::ProcessWorkerTestPeer::Busy(worker));
}

TEST(ProcessWorkerV1, PinsOneProcessHelloToTheLaunchedIdentity)
{
    std::vector<std::uint8_t> encoded_contract;
    ASSERT_TRUE(savor::runtime::EncodeWorkerRuntimeContractV1(
        savor::runtime::BuildProductionWorkerRuntimeContractV1(),
        encoded_contract));
    {
        savor::ProcessWorker worker;
        savor::ProcessWorkerTestPeer::ConfigureExpectedHello(
            worker,
            17,
            4242);
        const savor::wrms::ProcessHelloPayload hello{
            .worker_id = 17,
            .process_id = 4242,
            .encoded_runtime_contract = encoded_contract};
        ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
            worker,
            savor::wrms::MessageKind::ProcessHello,
            hello));
        EXPECT_FALSE(
            savor::ProcessWorkerTestPeer::ProtocolFailed(worker))
            << worker.last_error();
        ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
            worker,
            savor::wrms::MessageKind::ProcessHello,
            hello));
        EXPECT_TRUE(worker.is_failed());
        ExpectErrorContains(worker, "more than one");
    }
    {
        savor::ProcessWorker worker;
        savor::ProcessWorkerTestPeer::ConfigureExpectedHello(
            worker,
            17,
            4242);
        ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
            worker,
            savor::wrms::MessageKind::ProcessHello,
            savor::wrms::ProcessHelloPayload{
                .worker_id = 18,
                .process_id = 4242,
                .encoded_runtime_contract = encoded_contract}));
        EXPECT_TRUE(worker.is_failed());
        ExpectErrorContains(worker, "launched worker process");
    }
}

TEST(
    ProcessWorkerV1,
    UnsolicitedSpecialResponseFailsBeforeMutatingSessionSnapshot)
{
    savor::ProcessWorker worker;
    const auto before = worker.latest_snapshot();
    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::OpenSessionResult,
        savor::wrms::OpenSessionResultPayload{
            .success = true,
            .session_id = 77,
            .workset_epoch = 9,
            .worker_state = savor::wrms::WorkerStateCode::Ready,
            .session_disposition =
                savor::wrms::SessionDispositionCode::Clean},
        991));
    EXPECT_TRUE(worker.is_failed());
    const auto after = worker.latest_snapshot();
    EXPECT_EQ(after.session_open, before.session_open);
    EXPECT_EQ(after.session_id, before.session_id);
    EXPECT_EQ(after.workset_epoch, before.workset_epoch);
    ExpectErrorContains(worker, "unsolicited");
}

TEST(
    ProcessWorkerV1,
    MalformedCorrelatedSpecialResponseFailsBeforeMutatingSessionSnapshot)
{
    savor::ProcessWorker worker;
    constexpr std::uint64_t request_id = 992;
    savor::ProcessWorkerTestPeer::AddPendingRequest(
        worker,
        request_id,
        savor::wrms::MessageKind::OpenSession);
    const auto before = worker.latest_snapshot();
    const std::array<std::uint8_t, 1> malformed{0xff};
    EXPECT_TRUE(savor::ProcessWorkerTestPeer::DeliverRaw(
        worker,
        savor::wrms::MessageKind::OpenSessionResult,
        malformed,
        request_id));
    EXPECT_TRUE(worker.is_failed());
    const auto after = worker.latest_snapshot();
    EXPECT_EQ(after.session_open, before.session_open);
    EXPECT_EQ(after.session_id, before.session_id);
    EXPECT_EQ(after.workset_epoch, before.workset_epoch);
    ExpectErrorContains(worker, "invalid");
}

TEST(ProcessWorkerV1, MalformedAuthoritativeWorksetEventFailsClosed)
{
    savor::ProcessWorker worker;
    const std::array<std::uint8_t, 1> malformed{0xff};
    EXPECT_TRUE(savor::ProcessWorkerTestPeer::DeliverRaw(
        worker,
        savor::wrms::MessageKind::WorksetItemTerminal,
        malformed));
    EXPECT_TRUE(worker.is_failed());
    ExpectErrorContains(worker, "WorksetItemTerminal");
}

TEST(ProcessWorkerV1, CommandResultMustMatchExactSubmittedKind)
{
    savor::ProcessWorker worker;
    EXPECT_TRUE(
        savor::ProcessWorkerTestPeer::DeliverMismatchedCommandResult(
            worker));
    EXPECT_TRUE(worker.is_failed());
    ExpectErrorContains(worker, "exact request kind");
}

TEST(
    ProcessWorkerV1,
    StopRequestedFromCallbackIsJoinedByOwningThread)
{
    savor::ProcessWorker worker;
    auto stopped = std::make_shared<std::promise<void>>();
    auto stopped_future = stopped->get_future();
    worker.set_workset_credits_callback(
        [&](const savor::wrms::WorksetCreditsPayload&)
        {
            worker.stop();
            stopped->set_value();
        });
    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::WorksetCredits,
        savor::wrms::WorksetCreditsPayload{
            .outbound_sequence = 1,
            .available_item_credits = 4,
        }));
    ASSERT_EQ(
        stopped_future.wait_for(std::chrono::seconds(2)),
        std::future_status::ready);
    EXPECT_TRUE(
        savor::ProcessWorkerTestPeer::CallbackDispatcherJoinable(
            worker));

    worker.stop();
    EXPECT_FALSE(
        savor::ProcessWorkerTestPeer::CallbackDispatcherJoinable(
            worker));
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
    constexpr auto epoch = savor::runtime::WorksetEpoch{4};
    constexpr auto workset = savor::runtime::WorkerWorksetId{31};
    constexpr auto item = savor::runtime::WorkerWorksetItemId{7};
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
                            .session_id = session.value(),
                            .workset_epoch = epoch.value(),
                            .operation_id = 1000 + it->request_id,
                            .has_execution_state = true,
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
            epoch,
            workset,
            item));

    std::latch ready{2};
    std::latch go{1};
    const auto submit = [&](std::uint32_t count) {
        ready.count_down();
        go.wait();
        savor::wrms::ExecutionResultPayload result;
        const bool succeeded = worker.step_guest_frames(
            workset,
            item,
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
        EXPECT_EQ(request.payload.workset_id, workset.value());
        EXPECT_EQ(request.payload.item_id, item.value());
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
    const auto epoch = savor::runtime::WorksetEpoch{4};
    const ExecutionResultPayload valid_step{
        .control = ExecutionControlKind::StepFrame,
        .status = CommandStatus::Succeeded,
        .session_id = session.value(),
        .workset_epoch = epoch.value(),
        .operation_id = 17,
        .has_execution_state = true,
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
        .workset_epoch = epoch.value(),
        .operation_id = 18,
        .has_execution_state = true,
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
        .workset_epoch = epoch.value(),
        .operation_id = 19,
        .has_execution_state = true,
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

TEST(
    ProcessWorkerV1,
    UnlaunchedCancellationIsTypedAsTransportCanceledBeforeWrite)
{
    savor::ProcessWorker worker;
    const auto outcome = worker.cancel_workset_item_with_outcome(
        savor::runtime::WorkerWorksetId{1},
        savor::runtime::WorkerWorksetItemId{1},
        "test cancellation",
        1);
    EXPECT_EQ(
        outcome.disposition,
        savor::ProcessWorkerCommandDisposition::
            TransportCanceledBeforeWrite);
    EXPECT_FALSE(outcome.request_frame_written);
    EXPECT_FALSE(outcome.correlated_result_received);
    EXPECT_EQ(
        outcome.result.error_code,
        "WorkerCommandNotWritten");
}

TEST(ProcessWorkerV1, RuntimeDiagnosticPreservesSessionAndProgressCarriesAttemptId)
{
    savor::ProcessWorker worker;
    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverOpenSessionResult(
        worker,
        savor::wrms::OpenSessionResultPayload{
            .success = true,
            .session_id = 72,
            .workset_epoch = 10,
            .worker_mode = savor::wrms::WorkerModeCode::Headless,
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
    EXPECT_EQ(diagnosed.workset_epoch, authoritative.workset_epoch);
    EXPECT_EQ(diagnosed.worker_mode, authoritative.worker_mode);
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
    worker.set_invocation_progress_callback(
        [&](const auto& progress) { observed_progress = progress; });

    ASSERT_TRUE(savor::ProcessWorkerTestPeer::DeliverPayload(
        worker,
        savor::wrms::MessageKind::InvocationProgress,
        savor::wrms::InvocationProgressPayload{
            .invocation_id = 701,
            .attempt_id = 801,
            .ordinal = 1,
            .durable_job_id = "job-701",
            .library_id = "soa.progress.runtime.vi/1",
            .library_revision = 1,
            .progress_point_id = "vi.sample",
            .schema_id = "soa.progress.runtime.vi/1.Sample",
            .schema_revision = 1,
            .schema_sha256 = std::string(64, 'c'),
            .typed_payload = {1, 2},
            .display_text = "VI 513",
        }));
    savor::ProcessWorkerTestPeer::DrainCallbacks(worker);

    ASSERT_TRUE(observed_progress.has_value());
    EXPECT_EQ(observed_progress->invocation_id, 701u);
    EXPECT_EQ(observed_progress->attempt_id, 801u);
}

TEST(ProcessWorkerV1, ExecutionStateUpdatesSnapshotAndUsesDedicatedCallback)
{
    savor::ProcessWorker worker;
    std::optional<savor::wrms::ExecutionStatePayload> observed;
    worker.set_execution_state_callback(
        [&](const auto& state) { observed = state; });

    const savor::wrms::ExecutionStatePayload running{
        .session_id = 81,
        .workset_epoch = 12,
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
    savor::ProcessWorkerTestPeer::DrainCallbacks(worker);

    ASSERT_TRUE(observed.has_value());
    EXPECT_EQ(*observed, running);
    const auto snapshot = worker.latest_snapshot();
    EXPECT_EQ(snapshot.session_id, savor::runtime::SessionId{81});
    EXPECT_EQ(snapshot.workset_epoch, savor::runtime::WorksetEpoch{12});
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
