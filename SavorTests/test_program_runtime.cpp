#include <gtest/gtest.h>

#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "Runner/Runtime/ProgramRuntime/ProgramRuntime.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace savor::runtime;
using namespace savor::runtime::program;

ProgramBudgets Budgets()
{
    return {
        .maximum_instructions = 100,
        .maximum_calls = 16,
        .maximum_call_depth = 8,
        .maximum_action_requests = 16,
        .maximum_emissions = 16,
        .maximum_artifacts = 16,
        .maximum_values = 100,
        .maximum_value_bytes = 4096,
        .maximum_trace_events = 100,
    };
}

ProgramPolicySet Policies()
{
    return {
        .state_policies = {
            InvocationStatePolicy::RestoreBaseline,
        },
        .execution_intents = {ExecutionIntent::Live},
    };
}

ProgramValueGraph U32Graph(std::uint32_t value)
{
    return {
        ProgramValueId(1),
        {
            ProgramValue{
                ProgramValueId(1),
                TypeRef::Builtin(BuiltinType::U32),
                value,
            },
        },
    };
}

ProgramValueGraph UnitGraph()
{
    return {
        ProgramValueId(1),
        {
            ProgramValue{
                ProgramValueId(1),
                TypeRef::Builtin(BuiltinType::Unit),
                UnitValue{},
            },
        },
    };
}

ProgramValueGraph BytesGraph(
    TypeRef type,
    std::vector<Byte> bytes = {})
{
    return {
        ProgramValueId(1),
        {
            ProgramValue{
                ProgramValueId(1),
                std::move(type),
                std::move(bytes),
            },
        },
    };
}

ProgramValueGraph StepFramesGraph(std::uint64_t count)
{
    const ProgramValueId count_id(1);
    const ProgramValueId neutral_witness_id(2);
    const ProgramValueId static_config_id(3);
    const ProgramValueId root_id(4);
    return {
        root_id,
        {
            ProgramValue{
                count_id,
                TypeRef::Builtin(BuiltinType::U64),
                count,
            },
            ProgramValue{
                neutral_witness_id,
                CanonicalRuntimeType(
                    CanonicalRuntimeSchema::
                        OptionalInputNeutralWitness),
                OptionalValue{},
            },
            ProgramValue{
                static_config_id,
                CanonicalRuntimeType(
                    CanonicalRuntimeSchema::
                        ExecutionAdvanceStaticConfig),
                std::vector<Byte>{},
            },
            ProgramValue{
                root_id,
                CanonicalActionInputType(
                    CanonicalAction::ExecutionStepFrames),
                RecordValue{{
                    count_id,
                    neutral_witness_id,
                    static_config_id,
                }},
            },
        },
    };
}

const std::uint32_t* U32Value(const ProgramValueGraph& graph)
{
    const auto found = std::ranges::find_if(
        graph.values,
        [&graph](const ProgramValue& value) {
            return value.id == graph.root;
        });
    return found == graph.values.end()
        ? nullptr
        : std::get_if<std::uint32_t>(&found->payload);
}

ProgramModule Module(std::string canonical_id)
{
    ProgramModule module{
        .identity = {
            .canonical_id = std::move(canonical_id),
            .revision = 1,
        },
        .entrypoints = {
            ProgramEntrypoint{
                .name = "run",
                .function = ProgramFunctionId(1),
                .input_type =
                    TypeRef::Builtin(BuiltinType::U32),
                .output_type =
                    TypeRef::Builtin(BuiltinType::U32),
                .domain_outcome_type =
                    TypeRef::Builtin(BuiltinType::Bool),
                .accepted_policies = Policies(),
            },
        },
        .functions = {
            ProgramFunction{
                .id = ProgramFunctionId(1),
                .name = "run",
                .arguments = {
                    {
                        ProgramValueId(1),
                        TypeRef::Builtin(BuiltinType::U32),
                    },
                },
                .output_type =
                    TypeRef::Builtin(BuiltinType::U32),
                .domain_outcome_type =
                    TypeRef::Builtin(BuiltinType::Bool),
                .entry_block = ProgramBlockId(1),
                .blocks = {
                    BasicBlock{
                        .id = ProgramBlockId(1),
                        .instructions = {
                            Instruction{
                                .id = ProgramInstructionId(1),
                                .opcode =
                                    InstructionOpcode::Constant,
                                .source_location =
                                    ProgramSourceLocationId(1),
                                .result = ValueDefinition{
                                    ProgramValueId(2),
                                    TypeRef::Builtin(
                                        BuiltinType::Bool),
                                },
                                .literal = LiteralValue{
                                    TypeRef::Builtin(
                                        BuiltinType::Bool),
                                    true,
                                },
                            },
                        },
                        .terminator = Terminator{
                            .kind = TerminatorKind::Return,
                            .source_location =
                                ProgramSourceLocationId(2),
                            .return_value = ProgramValueId(1),
                            .domain_outcome = ProgramValueId(2),
                        },
                    },
                },
                .exported = true,
            },
        },
        .accepted_policies = Policies(),
        .budgets = Budgets(),
        .source_map = {
            .version = 1,
            .entries = {
                SourceMapEntry{
                    .id = ProgramSourceLocationId(1),
                    .function = ProgramFunctionId(1),
                    .block = ProgramBlockId(1),
                    .instruction = ProgramInstructionId(1),
                    .source_name = "program-runtime.test",
                    .semantic_path = "run.domain",
                },
                SourceMapEntry{
                    .id = ProgramSourceLocationId(2),
                    .function = ProgramFunctionId(1),
                    .block = ProgramBlockId(1),
                    .source_name = "program-runtime.test",
                    .semantic_path = "run.return",
                },
            },
        },
    };
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    return module;
}

ProgramModule StatePolicyModule(std::string canonical_id)
{
    ProgramModule module = Module(std::move(canonical_id));
    const std::vector<InvocationStatePolicy> policies{
        InvocationStatePolicy::RestoreBaseline,
    };
    module.accepted_policies.state_policies = policies;
    module.entrypoints.front()
        .accepted_policies.state_policies = policies;
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    return module;
}

ProgramModule TimingActionModule(
    std::string canonical_id,
    CanonicalAction action = CanonicalAction::TelemetryEmit)
{
    const ExactDependencyIdentity identity =
        CanonicalActionIdentity(action);
    const TypeRef input = CanonicalActionInputType(action);
    const TypeRef output = CanonicalActionOutputType(action);
    ProgramModule module{
        .identity = {
            .canonical_id = std::move(canonical_id),
            .revision = 1,
        },
        .entrypoints = {
            ProgramEntrypoint{
                .name = "run",
                .function = ProgramFunctionId(1),
                .input_type = input,
                .output_type = output,
                .domain_outcome_type =
                    TypeRef::Builtin(BuiltinType::Bool),
                .required_capability_packs = {
                    CanonicalRuntimePackIdentity()},
                .accepted_policies = Policies(),
            },
        },
        .functions = {
            ProgramFunction{
                .id = ProgramFunctionId(1),
                .name = "run",
                .arguments = {
                    {ProgramValueId(1), input},
                },
                .output_type = output,
                .domain_outcome_type =
                    TypeRef::Builtin(BuiltinType::Bool),
                .entry_block = ProgramBlockId(1),
                .blocks = {
                    BasicBlock{
                        .id = ProgramBlockId(1),
                        .instructions = {
                            Instruction{
                                .id = ProgramInstructionId(1),
                                .opcode =
                                    InstructionOpcode::AwaitAction,
                                .source_location =
                                    ProgramSourceLocationId(1),
                                .result = ValueDefinition{
                                    ProgramValueId(2),
                                    output,
                                },
                                .operands = {ProgramValueId(1)},
                                .target = {
                                    .kind =
                                        InstructionTargetKind::Action,
                                    .dependency = identity,
                                },
                            },
                            Instruction{
                                .id = ProgramInstructionId(2),
                                .opcode =
                                    InstructionOpcode::Constant,
                                .source_location =
                                    ProgramSourceLocationId(2),
                                .result = ValueDefinition{
                                    ProgramValueId(3),
                                    TypeRef::Builtin(
                                        BuiltinType::Bool),
                                },
                                .literal = LiteralValue{
                                    TypeRef::Builtin(
                                        BuiltinType::Bool),
                                    true,
                                },
                            },
                        },
                        .terminator = Terminator{
                            .kind = TerminatorKind::Return,
                            .source_location =
                                ProgramSourceLocationId(3),
                            .return_value = ProgramValueId(2),
                            .domain_outcome = ProgramValueId(3),
                        },
                    },
                },
                .exported = true,
            },
        },
        .action_imports = {identity},
        .type_imports = {*input.named, *output.named},
        .required_capability_packs = {
            CanonicalRuntimePackIdentity()},
        .accepted_policies = Policies(),
        .budgets = Budgets(),
        .source_map = {
            .version = 1,
            .entries = {
                SourceMapEntry{
                    .id = ProgramSourceLocationId(1),
                    .function = ProgramFunctionId(1),
                    .block = ProgramBlockId(1),
                    .instruction = ProgramInstructionId(1),
                    .source_name = "program-runtime.test",
                    .semantic_path = "run.read",
                },
                SourceMapEntry{
                    .id = ProgramSourceLocationId(2),
                    .function = ProgramFunctionId(1),
                    .block = ProgramBlockId(1),
                    .instruction = ProgramInstructionId(2),
                    .source_name = "program-runtime.test",
                    .semantic_path = "run.domain",
                },
                SourceMapEntry{
                    .id = ProgramSourceLocationId(3),
                    .function = ProgramFunctionId(1),
                    .block = ProgramBlockId(1),
                    .source_name = "program-runtime.test",
                    .semantic_path = "run.return",
                },
            },
        },
    };
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);
    return module;
}

ProgramModuleIdentity EnvelopeIdentity(
    const ModuleIdentity& identity)
{
    return {
        identity.canonical_id,
        identity.revision,
        identity.module_hash.ToHex(),
    };
}

RuntimeProfile TestRuntimeProfile()
{
    const RuntimeCompatibility compatibility =
        capabilities::SupportedSoaUsaCompatibility();
    return {
        .profile_id = "program-runtime-test",
        .game_id = compatibility.game_id,
        .disc_identity = "test-disc",
        .executable_identity =
            compatibility.executable_identity,
        .backend = "fake",
    };
}

ProgramRuntimeConfig TestRuntimeConfig()
{
    return {
        .compatibility =
            capabilities::SupportedSoaUsaCompatibility(),
        .runtime_profile = TestRuntimeProfile(),
    };
}

ProgramInvocation Invocation(
    const ProgramModule& module,
    InvocationId invocation_id = InvocationId(71))
{
    return {
        .invocation_id = invocation_id,
        .attempt_id = AttemptId(4),
        .module = module.identity,
        .entrypoint = "run",
        .dependencies = {
            .ir_version = kCanonicalIrVersionV1,
        },
        .runtime_profile = TestRuntimeProfile(),
        .state = {
            .policy = InvocationStatePolicy::RestoreBaseline,
            .session_lineage = "program-runtime-test",
            .expected_session = SessionId(9),
            .expected_epoch = WorksetEpoch(5),
        },
        .execution = {
            .intent = ExecutionIntent::Live,
            .record_trace = true,
            .record_progress = false,
        },
        .input = U32Graph(123),
        .limits = Budgets(),
        .provenance = {
            .requesting_component = "ProgramRuntimeTest",
        },
    };
}

class RecordingEventSink final : public IProgramRuntimeEventSink
{
public:
    void Publish(ProgramRuntimeEvent event) override
    {
        events.push_back(std::move(event));
    }

    template <typename T>
    const T* Last() const
    {
        for (auto item = events.rbegin();
             item != events.rend();
             ++item)
        {
            if (const auto* value = std::get_if<T>(&*item))
                return value;
        }
        return nullptr;
    }

    std::vector<ProgramRuntimeEvent> events;
};

class RecordingActionSink final
    : public IProgramActionRequestSink
{
public:
    void Publish(ProgramActionRequest request) override
    {
        requests.push_back(std::move(request));
    }

    std::vector<ProgramActionRequest> requests;
};

ModulePreparationRequest PreparationRequest(
    const ProgramModule& module,
    WorkerCommandSequence sequence = WorkerCommandSequence(1))
{
    const EncodeResult encoded = EncodeProgramModuleV1(module);
    EXPECT_TRUE(encoded)
        << encoded.status.message;
    return {
        .command_sequence = sequence,
        .module = {
            .identity = EnvelopeIdentity(module.identity),
            .format_version = kProgramCodecVersionV1,
            .payload = encoded.bytes,
        },
    };
}

EncodedInvocationEnvelope InvocationRequest(
    const ProgramInvocation& invocation,
    WorkerCommandSequence sequence = WorkerCommandSequence(2))
{
    const EncodeResult encoded =
        EncodeProgramInvocationV1(invocation);
    EXPECT_TRUE(encoded)
        << encoded.status.message;
    (void)sequence;
    return {
        .invocation_id = invocation.invocation_id,
        .attempt_id = invocation.attempt_id,
        .module = EnvelopeIdentity(invocation.module),
        .entrypoint = invocation.entrypoint,
        .expected_workset_epoch = invocation.state.expected_epoch,
        .input_payload = encoded.bytes,
    };
}

ProgramRuntimeSubmission StartInvocation(
    ProgramRuntime& runtime,
    EncodedInvocationEnvelope invocation,
    CancellationToken cancellation,
    const std::shared_ptr<IProgramRuntimeEventSink>& events,
    bool state_already_prepared = true)
{
    const WorksetEpoch workset_epoch = invocation.expected_workset_epoch;
    DecodeResult<ProgramInvocation> decoded =
        DecodeProgramInvocationV1(invocation.input_payload);
    if (!decoded)
    {
        return ProgramRuntimeSubmission::Rejected(
            WorkerRejectionCode::InvalidArgument,
            decoded.status.message.empty()
                ? "test invocation payload is invalid"
                : decoded.status.message);
    }
    const std::string baseline_lineage =
        decoded.value->state.session_lineage;
    ProgramInvocation unbound = *decoded.value;
    unbound.state.expected_session = {};
    unbound.state.expected_epoch = {};
    EncodedInvocationEnvelope unbound_envelope =
        InvocationRequest(unbound);
    PreparedInvocationTemplateReceipt receipt;
    const ProgramRuntimeSubmission prepared =
        runtime.PrepareInvocationTemplate(
            {
                WorkerCommandSequence(2),
                std::move(unbound_envelope),
            },
            receipt);
    if (!prepared.accepted)
        return prepared;
    return runtime.StartPreparedInvocation(
        {
            WorkerCommandSequence(3),
            receipt.template_id,
            SessionId(1),
            workset_epoch,
            std::string(64, 'b'),
            baseline_lineage,
            state_already_prepared,
        },
        std::move(cancellation),
        events);
}

ProgramActionResolution Completion(
    const ProgramActionRequest& request,
    WorksetEpoch workset_epoch,
    ProgramActionResolutionStatus status =
        ProgramActionResolutionStatus::Completed)
{
    return {
        .request_id = request.request_id,
        .invocation_id = request.invocation_id,
        .attempt_id = request.attempt_id,
        .operation = request.operation,
        .status = status,
        .workset_epoch = workset_epoch,
        .output = UnitGraph(),
        .cleanup = ProgramCleanupStatus::Clean,
        .session_disposition = SessionDisposition::Clean,
    };
}

TEST(ProgramRuntime, PreparesSprmBeforeAcceptingSpri)
{
    ProgramRuntime runtime(TestRuntimeConfig());
    ASSERT_TRUE(runtime.initialized())
        << runtime.initialization_diagnostic();
    EXPECT_TRUE(HasCapability(
        runtime.capabilities(),
        WorkerCapability::WorksetDispatch));

    auto events = std::make_shared<RecordingEventSink>();
    const ProgramModule module =
        Module("test.runtime.prepare-start");
    const ProgramRuntimeSubmission prepared =
        runtime.PrepareModule(
            PreparationRequest(module),
            events);
    ASSERT_TRUE(prepared.accepted)
        << prepared.error.message;
    const ModulePreparationEvent* preparation =
        events->Last<ModulePreparationEvent>();
    ASSERT_NE(preparation, nullptr);
    EXPECT_TRUE(preparation->prepared)
        << preparation->error.message;
    EXPECT_EQ(
        preparation->module,
        EnvelopeIdentity(module.identity));
    EXPECT_EQ(runtime.definitions().size(), 1u);
    const ProgramRuntimeCatalogSnapshot prepared_catalog =
        runtime.catalog();
    ASSERT_EQ(prepared_catalog.modules.size(), 1u);
    EXPECT_EQ(
        prepared_catalog.modules.front().dependency_lock_sha256,
        ComputeProgramDependencyLockHashV1(
            ProgramDependencyLock{
                .ir_version = kCanonicalIrVersionV1})
            .ToHex());
    EXPECT_NE(
        prepared_catalog.modules.front().dependency_lock_sha256,
        prepared_catalog.dependency_manifest_sha256);

    const ProgramInvocation invocation = Invocation(module);
    CancellationSource cancellation(invocation.invocation_id);
    const ProgramRuntimeSubmission no_sink =
        StartInvocation(runtime,
            InvocationRequest(invocation),
            cancellation.token(),
            events);
    EXPECT_FALSE(no_sink.accepted);
    EXPECT_EQ(
        no_sink.error.code,
        WorkerRejectionCode::ProgramRuntimeUnavailable);
}

TEST(
    ProgramRuntime,
    CompleteExactRequiresEveryConfiguredModuleAndNoExtras)
{
    std::vector<ProgramModule> modules;
    std::vector<ProgramRuntimeCatalogModule> expected;
    for (std::uint32_t index = 0; index < 9; ++index)
    {
        modules.push_back(Module(
            "test.runtime.catalog." +
            std::to_string(index)));
        expected.push_back(ProgramRuntimeCatalogModule{
            EnvelopeIdentity(modules.back().identity),
            {"run"},
            ComputeProgramDependencyLockHashV1(
                ProgramDependencyLock{
                    .ir_version = kCanonicalIrVersionV1})
                .ToHex(),
            false});
    }
    ProgramRuntimeConfig config = TestRuntimeConfig();
    config.expected_exact_catalog = expected;
    ProgramRuntimeConfig invalid_config = config;
    invalid_config.expected_exact_catalog->pop_back();
    ProgramRuntime invalid_runtime(std::move(invalid_config));
    EXPECT_FALSE(invalid_runtime.initialized());

    ProgramRuntime runtime(config);
    ASSERT_TRUE(runtime.initialized())
        << runtime.initialization_diagnostic();
    EXPECT_FALSE(runtime.catalog().complete_exact);

    auto events = std::make_shared<RecordingEventSink>();
    for (std::size_t index = 0; index < modules.size(); ++index)
    {
        const ProgramRuntimeSubmission prepared =
            runtime.PrepareModule(
                PreparationRequest(
                    modules[index],
                    WorkerCommandSequence(index + 1)),
                events);
        ASSERT_TRUE(prepared.accepted)
            << prepared.error.message;
        EXPECT_EQ(
            runtime.catalog().complete_exact,
            index + 1 == modules.size());
    }

    const ProgramModule extra =
        Module("test.runtime.catalog.extra");
    ASSERT_TRUE(
        runtime.PrepareModule(
            PreparationRequest(
                extra,
                WorkerCommandSequence(10)),
            events)
            .accepted);
    EXPECT_FALSE(runtime.catalog().complete_exact);

    ProgramRuntime development_runtime(config);
    ASSERT_TRUE(development_runtime.initialized())
        << development_runtime.initialization_diagnostic();
    for (std::size_t index = 0; index < modules.size(); ++index)
    {
        ModulePreparationRequest request =
            PreparationRequest(
                modules[index],
                WorkerCommandSequence(index + 1));
        request.module.development_only =
            index + 1 == modules.size();
        ASSERT_TRUE(
            development_runtime.PrepareModule(
                std::move(request),
                events)
                .accepted);
    }
    EXPECT_FALSE(development_runtime.catalog().complete_exact);
}

TEST(
    ProgramRuntime,
    VerifiesUnboundWorksetTemplateBeforeExactBaselineBinding)
{
    ProgramRuntime runtime(TestRuntimeConfig());
    auto events = std::make_shared<RecordingEventSink>();
    auto actions = std::make_shared<RecordingActionSink>();
    runtime.BindActionSink(actions);
    const ProgramModule module =
        Module("test.runtime.workset-template");
    ASSERT_TRUE(
        runtime.PrepareModule(
            PreparationRequest(module),
            events)
            .accepted);

    ProgramInvocation invocation = Invocation(module);
    invocation.state.expected_session = {};
    invocation.state.expected_epoch = {};
    const EncodeResult encoded =
        EncodeProgramInvocationV1(invocation);
    ASSERT_TRUE(encoded) << encoded.status.message;
    InvocationTemplatePreparationRequest preparation;
    preparation.command_sequence = WorkerCommandSequence(2);
    preparation.invocation_template = {
        invocation.invocation_id,
        invocation.attempt_id,
        EnvelopeIdentity(invocation.module),
        invocation.entrypoint,
        {},
        encoded.bytes};
    PreparedInvocationTemplateReceipt prepared;
    const ProgramRuntimeSubmission admitted =
        runtime.PrepareInvocationTemplate(
            std::move(preparation),
            prepared);
    ASSERT_TRUE(admitted.accepted) << admitted.error.message;
    ASSERT_TRUE(prepared);

    CancellationSource cancellation(invocation.invocation_id);
    const ProgramRuntimeSubmission started =
        runtime.StartPreparedInvocation(
            {
                WorkerCommandSequence(3),
                prepared.template_id,
                SessionId(9),
                WorksetEpoch(5),
                std::string(64, 'a'),
                invocation.state.session_lineage,
                true,
            },
            cancellation.token(),
            events);
    ASSERT_TRUE(started.accepted) << started.error.message;
    ASSERT_EQ(actions->requests.size(), 1u);
    const ProgramActionRequest& state = actions->requests.back();
    EXPECT_TRUE(state.state_already_prepared);
    EXPECT_EQ(
        state.prepared_baseline_sha256,
        std::string(64, 'a'));
    ASSERT_TRUE(state.state_request);
    EXPECT_EQ(
        state.state_request->expected_session,
        SessionId(9));
    EXPECT_EQ(
        state.state_request->expected_epoch,
        WorksetEpoch(5));

    ASSERT_TRUE(
        runtime.DeliverActionResolution(
            Completion(state, WorksetEpoch(5)))
            .accepted);
    EXPECT_FALSE(runtime.Pump());
    ASSERT_EQ(actions->requests.size(), 2u);
    const ProgramActionRequest finish = actions->requests.back();
    ASSERT_EQ(
        finish.operation,
        ProgramHostOperation::FinishInvocation);
    ASSERT_TRUE(
        runtime.DeliverActionResolution(
            Completion(finish, WorksetEpoch(5)))
            .accepted);
    EXPECT_FALSE(runtime.Pump());
    const auto taken = runtime.TakeFinishedExecution(
        invocation.invocation_id,
        invocation.attempt_id);
    const auto* terminal = taken.finished
        ? &*taken.finished
        : nullptr;
    ASSERT_NE(terminal, nullptr);
    EXPECT_EQ(
        terminal->status,
        InvocationTerminalStatus::Completed);
    EXPECT_EQ(terminal->workset_epoch, WorksetEpoch(5));
}

TEST(ProgramRuntime, RequiresAnExactConfiguredRuntimeProfile)
{
    ProgramRuntimeConfig invalid = TestRuntimeConfig();
    invalid.runtime_profile.backend.clear();
    ProgramRuntime unavailable(std::move(invalid));
    EXPECT_FALSE(unavailable.initialized());
    EXPECT_FALSE(HasCapability(
        unavailable.capabilities(),
        WorkerCapability::WorksetDispatch));

    ProgramRuntime runtime(TestRuntimeConfig());
    ASSERT_TRUE(runtime.initialized())
        << runtime.initialization_diagnostic();
    auto events = std::make_shared<RecordingEventSink>();
    auto actions = std::make_shared<RecordingActionSink>();
    runtime.BindActionSink(actions);
    const ProgramModule module =
        Module("test.runtime.profile");
    ASSERT_TRUE(runtime.PrepareModule(
        PreparationRequest(module), events).accepted);
    ASSERT_TRUE(events->Last<ModulePreparationEvent>()->prepared);

    const auto rejected = [&](ProgramInvocation invocation) {
        CancellationSource cancellation(invocation.invocation_id);
        return StartInvocation(runtime,
            InvocationRequest(invocation),
            cancellation.token(),
            events);
    };

    ProgramInvocation mismatch = Invocation(module);
    mismatch.runtime_profile.profile_id = "another-profile";
    EXPECT_FALSE(rejected(mismatch).accepted);
    mismatch = Invocation(module);
    mismatch.runtime_profile.disc_identity = "another-disc";
    EXPECT_FALSE(rejected(mismatch).accepted);
    mismatch = Invocation(module);
    mismatch.runtime_profile.backend = "jit64";
    EXPECT_FALSE(rejected(mismatch).accepted);
    mismatch = Invocation(module);
    mismatch.runtime_profile.game_id = "OTHER";
    EXPECT_FALSE(rejected(mismatch).accepted);
    mismatch = Invocation(module);
    mismatch.runtime_profile.executable_identity = "other-executable";
    EXPECT_FALSE(rejected(mismatch).accepted);
    mismatch = Invocation(module);
    mismatch.runtime_profile.capability_packs = {
        CanonicalRuntimePackIdentity()};
    EXPECT_FALSE(rejected(mismatch).accepted);
    EXPECT_TRUE(actions->requests.empty());
}

TEST(ProgramRuntime, EnforcesInvocationStatePolicyShapes)
{
    ProgramRuntime runtime(TestRuntimeConfig());
    ASSERT_TRUE(runtime.initialized())
        << runtime.initialization_diagnostic();
    auto events = std::make_shared<RecordingEventSink>();
    auto actions = std::make_shared<RecordingActionSink>();
    runtime.BindActionSink(actions);
    const ProgramModule module =
        StatePolicyModule("test.runtime.state-shapes");
    ASSERT_TRUE(runtime.PrepareModule(
        PreparationRequest(module), events).accepted);
    ASSERT_TRUE(events->Last<ModulePreparationEvent>()->prepared);

    std::uint64_t next_id = 80;
    ProgramInvocation valid =
        Invocation(module, InvocationId(next_id++));
    valid.state.policy = InvocationStatePolicy::RestoreBaseline;
    CancellationSource cancellation(valid.invocation_id);
    ASSERT_TRUE(StartInvocation(runtime,
        InvocationRequest(valid),
        cancellation.token(),
        events).accepted);
    ASSERT_EQ(actions->requests.size(), 1u);
    ASSERT_TRUE(actions->requests.front().state_request);
    EXPECT_EQ(
        actions->requests.front().state_request->policy,
        InvocationStatePolicy::RestoreBaseline);

    ProgramRuntime baseline_runtime(TestRuntimeConfig());
    ASSERT_TRUE(baseline_runtime.initialized())
        << baseline_runtime.initialization_diagnostic();
    auto baseline_events =
        std::make_shared<RecordingEventSink>();
    auto baseline_actions =
        std::make_shared<RecordingActionSink>();
    baseline_runtime.BindActionSink(baseline_actions);
    ASSERT_TRUE(baseline_runtime.PrepareModule(
        PreparationRequest(module), baseline_events).accepted);
    ASSERT_TRUE(
        baseline_events->Last<ModulePreparationEvent>()->prepared);
    ProgramInvocation baseline =
        Invocation(module, InvocationId(next_id++));
    baseline.state.policy =
        InvocationStatePolicy::RestoreBaseline;
    CancellationSource baseline_cancellation(
        baseline.invocation_id);
    ASSERT_TRUE(StartInvocation(baseline_runtime,
        InvocationRequest(baseline),
        baseline_cancellation.token(),
        baseline_events).accepted);
    ASSERT_EQ(baseline_actions->requests.size(), 1u);
    ASSERT_TRUE(
        baseline_actions->requests.front().state_request);
    EXPECT_EQ(
        baseline_actions->requests.front()
            .state_request->policy,
        InvocationStatePolicy::RestoreBaseline);
}

TEST(ProgramRuntime, RunsOnlyThroughTheActorActionSinkAndPublishesSprr)
{
    ProgramRuntime runtime(TestRuntimeConfig());
    ASSERT_TRUE(runtime.initialized())
        << runtime.initialization_diagnostic();
    auto events = std::make_shared<RecordingEventSink>();
    auto actions = std::make_shared<RecordingActionSink>();
    runtime.BindActionSink(actions);

    const ProgramModule module =
        Module("test.runtime.action-sink-only");
    ASSERT_TRUE(
        runtime.PrepareModule(
            PreparationRequest(module),
            events)
            .accepted);
    ASSERT_NE(events->Last<ModulePreparationEvent>(), nullptr);
    ASSERT_TRUE(events->Last<ModulePreparationEvent>()->prepared)
        << events->Last<ModulePreparationEvent>()
               ->error.message;

    ProgramInvocation invocation = Invocation(module);
    CancellationSource cancellation(invocation.invocation_id);
    const ProgramRuntimeSubmission started =
        StartInvocation(runtime,
            InvocationRequest(invocation),
            cancellation.token(),
            events);
    ASSERT_TRUE(started.accepted)
        << started.error.message;
    ASSERT_EQ(actions->requests.size(), 1u);
    const ProgramActionRequest state_request =
        actions->requests.front();
    EXPECT_EQ(
        state_request.operation,
        ProgramHostOperation::PrepareInvocationState);
    ASSERT_TRUE(state_request.state_request);
    EXPECT_EQ(
        state_request.state_request->policy,
        invocation.state.policy);
    EXPECT_EQ(
        state_request.state_request->session_lineage,
        invocation.state.session_lineage);
    EXPECT_EQ(
        state_request.state_request->expected_session,
        SessionId(1));
    EXPECT_EQ(
        state_request.state_request->expected_epoch,
        invocation.state.expected_epoch);

    // Entry execution cannot begin before the actor completes state
    // preparation.
    EXPECT_FALSE(runtime.Pump());
    EXPECT_EQ(actions->requests.size(), 1u);
    EXPECT_FALSE(runtime.TakeFinishedExecution(
        invocation.invocation_id,
        invocation.attempt_id).taken);

    const WorksetEpoch prepared_epoch =
        invocation.state.expected_epoch;
    ProgramActionResolution prepared_completion =
        Completion(state_request, prepared_epoch);
    prepared_completion.cleanup_receipts.push_back(
        CleanupReceipt{
            ProgramResourceHandleId(11),
            ProgramCleanupStatus::Clean,
            "state preparation cleanup"});
    ASSERT_TRUE(
        runtime.DeliverActionResolution(
            std::move(prepared_completion))
            .accepted);
    EXPECT_EQ(actions->requests.size(), 1u);

    // This module immediately returns, but FinishInvocation is still an
    // actor-marshalled host operation rather than an inline session call.
    EXPECT_FALSE(runtime.Pump());
    ASSERT_EQ(actions->requests.size(), 2u);
    const ProgramActionRequest finish_request =
        actions->requests.back();
    EXPECT_EQ(
        finish_request.operation,
        ProgramHostOperation::FinishInvocation);
    EXPECT_EQ(
        finish_request.expected_epoch,
        prepared_epoch);
    EXPECT_TRUE(finish_request.cleanup_only);

    ProgramActionResolution finish_completion =
        Completion(finish_request, prepared_epoch);
    finish_completion.cleanup_receipts.push_back(
        CleanupReceipt{
            ProgramResourceHandleId(12),
            ProgramCleanupStatus::CleanWithDiagnostics,
            "finish cleanup"});
    finish_completion.cleanup =
        ProgramCleanupStatus::CleanWithDiagnostics;
    ASSERT_TRUE(runtime.DeliverActionResolution(
        std::move(finish_completion)).accepted);
    EXPECT_FALSE(runtime.Pump());

    const auto taken = runtime.TakeFinishedExecution(
        invocation.invocation_id,
        invocation.attempt_id);
    const ProgramExecutionFinished* terminal = taken.finished
        ? &*taken.finished
        : nullptr;
    ASSERT_NE(terminal, nullptr);
    EXPECT_EQ(
        terminal->status,
        InvocationTerminalStatus::Completed);
    EXPECT_EQ(
        terminal->cleanup,
        CleanupStatus::CleanWithDiagnostics);
    EXPECT_EQ(
        terminal->session_disposition,
        SessionDisposition::Clean);
    EXPECT_EQ(
        terminal->workset_epoch,
        invocation.state.expected_epoch);
    ASSERT_GE(terminal->output_payload.size(), 4u);
    EXPECT_EQ(terminal->output_payload[0], Byte('S'));
    EXPECT_EQ(terminal->output_payload[1], Byte('P'));
    EXPECT_EQ(terminal->output_payload[2], Byte('R'));
    EXPECT_EQ(terminal->output_payload[3], Byte('R'));

    const DecodeResult<ProgramResult> decoded =
        DecodeProgramResultV1(terminal->output_payload);
    ASSERT_TRUE(decoded)
        << decoded.status.message;
    ASSERT_TRUE(decoded.value->output);
    ASSERT_NE(U32Value(*decoded.value->output), nullptr);
    EXPECT_EQ(*U32Value(*decoded.value->output), 123u);
    EXPECT_EQ(
        decoded.value->infrastructure,
        ProgramInfrastructureStatus::Completed);
    EXPECT_EQ(
        decoded.value->cleanup,
        ProgramCleanupStatus::CleanWithDiagnostics);
    EXPECT_EQ(
        decoded.value->cleanup_receipts,
        (std::vector<CleanupReceipt>{
            {
                ProgramResourceHandleId(11),
                ProgramCleanupStatus::Clean,
                "state preparation cleanup",
            },
            {
                ProgramResourceHandleId(12),
                ProgramCleanupStatus::CleanWithDiagnostics,
                "finish cleanup",
            },
        }));
}

TEST(ProgramRuntime, ProjectsOnlyVerifiedBoundedHostOperationDeadlines)
{
    ProgramRuntime runtime(TestRuntimeConfig());
    ASSERT_TRUE(runtime.initialized())
        << runtime.initialization_diagnostic();
    auto events = std::make_shared<RecordingEventSink>();
    auto actions = std::make_shared<RecordingActionSink>();
    runtime.BindActionSink(actions);

    const ProgramModule module =
        TimingActionModule("test.runtime.action-timing");
    ASSERT_TRUE(runtime.PrepareModule(
        PreparationRequest(module), events).accepted);
    const ModulePreparationEvent* prepared =
        events->Last<ModulePreparationEvent>();
    ASSERT_NE(prepared, nullptr);
    ASSERT_TRUE(prepared->prepared) << prepared->error.message;

    const CanonicalAction action = CanonicalAction::TelemetryEmit;
    const ExactDependencyIdentity identity =
        CanonicalActionIdentity(action);
    const TypeRef input_type = CanonicalActionInputType(action);
    ProgramInvocation invocation = Invocation(module);
    invocation.dependencies.action_imports = {identity};
    invocation.dependencies.type_imports =
        CanonicalActionTypeSchemaClosure(action);
    invocation.dependencies.capability_packs = {
        CanonicalRuntimePackIdentity()};
    invocation.runtime_profile.capability_packs =
        invocation.dependencies.capability_packs;
    invocation.input = BytesGraph(input_type);

    CancellationSource cancellation(invocation.invocation_id);
    ASSERT_TRUE(StartInvocation(runtime,
        InvocationRequest(invocation),
        cancellation.token(),
        events).accepted);
    ASSERT_EQ(actions->requests.size(), 1u);
    EXPECT_EQ(
        actions->requests.front().timing,
        ActionTimingClass::BoundedHostOperation);
    EXPECT_TRUE(actions->requests.front().bounded_host_deadline);
    EXPECT_FALSE(runtime.next_wake().has_value());

    ASSERT_TRUE(runtime.DeliverActionResolution(
        Completion(actions->requests.front(), WorksetEpoch(5))).accepted);
    EXPECT_FALSE(runtime.Pump());
    ASSERT_EQ(actions->requests.size(), 2u);
    const ProgramActionRequest& request = actions->requests.back();
    ASSERT_EQ(request.action, identity);
    EXPECT_EQ(
        request.timing,
        ActionTimingClass::BoundedHostOperation);
    ASSERT_TRUE(request.bounded_host_deadline);

    const ActionDescriptor* descriptor =
        runtime.actions().ResolveAction(identity);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(
        descriptor->timing,
        ActionTimingClass::BoundedHostOperation);
    const auto descriptor_remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            *request.bounded_host_deadline -
            std::chrono::steady_clock::now());
    EXPECT_GT(descriptor_remaining.count(), 0);
    EXPECT_LE(
        descriptor_remaining.count(),
        static_cast<std::int64_t>(
            descriptor->default_host_timeout_milliseconds));
}

TEST(ProgramRuntime, CancellationDrivenActionsCarryNoElapsedDeadline)
{
    ProgramRuntime runtime(TestRuntimeConfig());
    ASSERT_TRUE(runtime.initialized())
        << runtime.initialization_diagnostic();
    auto events = std::make_shared<RecordingEventSink>();
    auto actions = std::make_shared<RecordingActionSink>();
    runtime.BindActionSink(actions);

    const CanonicalAction action =
        CanonicalAction::ExecutionStepFrames;
    const ProgramModule module =
        TimingActionModule(
            "test.runtime.cancellation-driven-action",
            action);
    ASSERT_TRUE(runtime.PrepareModule(
        PreparationRequest(module), events).accepted);
    ASSERT_TRUE(events->Last<ModulePreparationEvent>()->prepared);

    ProgramInvocation invocation = Invocation(module);
    const ExactDependencyIdentity identity =
        CanonicalActionIdentity(action);
    const TypeRef input_type = CanonicalActionInputType(action);
    invocation.dependencies.action_imports = {identity};
    invocation.dependencies.type_imports =
        CanonicalActionTypeSchemaClosure(action);
    invocation.dependencies.capability_packs = {
        CanonicalRuntimePackIdentity()};
    invocation.runtime_profile.capability_packs =
        invocation.dependencies.capability_packs;
    invocation.input = StepFramesGraph(1);

    CancellationSource cancellation(invocation.invocation_id);
    ASSERT_TRUE(StartInvocation(runtime,
        InvocationRequest(invocation),
        cancellation.token(),
        events).accepted);
    ASSERT_EQ(actions->requests.size(), 1u);
    EXPECT_EQ(
        actions->requests.front().timing,
        ActionTimingClass::BoundedHostOperation);
    EXPECT_TRUE(actions->requests.front().bounded_host_deadline);
    EXPECT_FALSE(runtime.next_wake());

    ASSERT_TRUE(runtime.DeliverActionResolution(
        Completion(actions->requests.front(), WorksetEpoch(5))).accepted);
    EXPECT_FALSE(runtime.Pump());
    ASSERT_EQ(actions->requests.size(), 2u);
    const ProgramActionRequest& request = actions->requests.back();
    ASSERT_EQ(request.action, identity);
    EXPECT_EQ(
        request.timing,
        ActionTimingClass::CancellationDriven);
    EXPECT_FALSE(request.bounded_host_deadline);
    EXPECT_FALSE(runtime.next_wake());
}

TEST(ProgramRuntime, RetainsFinishedExecutionUntilExactActorTake)
{
    ProgramRuntime runtime(TestRuntimeConfig());
    ASSERT_TRUE(runtime.initialized())
        << runtime.initialization_diagnostic();
    auto events = std::make_shared<RecordingEventSink>();
    auto actions = std::make_shared<RecordingActionSink>();
    runtime.BindActionSink(actions);

    const ProgramModule module =
        Module("test.runtime.execution-take");
    ASSERT_TRUE(runtime.PrepareModule(
        PreparationRequest(module), events).accepted);
    ASSERT_TRUE(events->Last<ModulePreparationEvent>()->prepared);

    const ProgramInvocation invocation = Invocation(module);
    CancellationSource cancellation(invocation.invocation_id);
    ASSERT_TRUE(StartInvocation(runtime,
        InvocationRequest(invocation),
        cancellation.token(),
        events).accepted);
    ASSERT_TRUE(runtime.DeliverActionResolution(
        Completion(
            actions->requests.front(),
            invocation.state.expected_epoch)).accepted);
    EXPECT_FALSE(runtime.Pump());
    ASSERT_TRUE(runtime.DeliverActionResolution(
        Completion(
            actions->requests.back(),
            invocation.state.expected_epoch)).accepted);
    EXPECT_FALSE(runtime.Pump());
    const ProgramRuntimeSubmission cancellation_lost =
        runtime.RequestCancellation(invocation.invocation_id);
    EXPECT_TRUE(cancellation_lost.accepted);
    EXPECT_TRUE(cancellation_lost.execution_already_finished);
    const auto wrong_take = runtime.TakeFinishedExecution(
        invocation.invocation_id,
        AttemptId(999));
    EXPECT_FALSE(wrong_take.taken);
    EXPECT_EQ(
        wrong_take.error.code,
        WorkerRejectionCode::InvocationMismatch);
    EXPECT_TRUE(runtime.TakeFinishedExecution(
        invocation.invocation_id,
        invocation.attempt_id).taken);
    const auto repeated_take = runtime.TakeFinishedExecution(
        invocation.invocation_id,
        invocation.attempt_id);
    EXPECT_FALSE(repeated_take.taken);
    EXPECT_EQ(
        repeated_take.error.code,
        WorkerRejectionCode::InvocationNotActive);

    ProgramInvocation next =
        Invocation(module, InvocationId(72));
    CancellationSource next_cancellation(next.invocation_id);
    EXPECT_TRUE(StartInvocation(runtime,
        InvocationRequest(next),
        next_cancellation.token(),
        events).accepted);
}

TEST(ProgramRuntime, RemembersCancellationDuringBaselinePreparation)
{
    ProgramRuntime runtime(TestRuntimeConfig());
    ASSERT_TRUE(runtime.initialized())
        << runtime.initialization_diagnostic();
    auto events = std::make_shared<RecordingEventSink>();
    auto actions = std::make_shared<RecordingActionSink>();
    runtime.BindActionSink(actions);

    const ProgramModule module =
        Module("test.runtime.cancel-during-state");
    ASSERT_TRUE(
        runtime.PrepareModule(
            PreparationRequest(module),
            events)
            .accepted);
    ASSERT_TRUE(events->Last<ModulePreparationEvent>()->prepared);
    const ProgramInvocation invocation = Invocation(module);
    CancellationSource cancellation(invocation.invocation_id);
    ASSERT_TRUE(
        StartInvocation(runtime,
            InvocationRequest(invocation),
            cancellation.token(),
            events)
            .accepted);
    ASSERT_EQ(actions->requests.size(), 1u);
    const ProgramActionRequest state_request =
        actions->requests.front();

    const ProgramRuntimeSubmission cancelled =
        runtime.RequestCancellation(invocation.invocation_id);
    ASSERT_TRUE(cancelled.accepted)
        << cancelled.error.message;
    ASSERT_TRUE(
        runtime.DeliverActionResolution(
            Completion(state_request, WorksetEpoch(5)))
            .accepted);

    EXPECT_FALSE(runtime.Pump());
    ASSERT_EQ(actions->requests.size(), 2u);
    const ProgramActionRequest finish_request =
        actions->requests.back();
    EXPECT_EQ(
        finish_request.operation,
        ProgramHostOperation::FinishInvocation);
    EXPECT_TRUE(finish_request.cleanup_only);
    ASSERT_TRUE(
        runtime.DeliverActionResolution(
            Completion(finish_request, WorksetEpoch(5)))
            .accepted);
    EXPECT_FALSE(runtime.Pump());

    const auto taken = runtime.TakeFinishedExecution(
        invocation.invocation_id,
        invocation.attempt_id);
    const ProgramExecutionFinished* terminal = taken.finished
        ? &*taken.finished
        : nullptr;
    ASSERT_NE(terminal, nullptr);
    EXPECT_EQ(
        terminal->status,
        InvocationTerminalStatus::Cancelled);
    const DecodeResult<ProgramResult> decoded =
        DecodeProgramResultV1(terminal->output_payload);
    ASSERT_TRUE(decoded)
        << decoded.status.message;
    EXPECT_EQ(
        decoded.value->infrastructure,
        ProgramInfrastructureStatus::Cancelled);
    EXPECT_FALSE(decoded.value->output);
}

TEST(ProgramRuntime, RejectsMalformedSpriWithoutPublishingAnAction)
{
    ProgramRuntime runtime(TestRuntimeConfig());
    ASSERT_TRUE(runtime.initialized())
        << runtime.initialization_diagnostic();
    auto events = std::make_shared<RecordingEventSink>();
    auto actions = std::make_shared<RecordingActionSink>();
    runtime.BindActionSink(actions);

    const ProgramModule module =
        Module("test.runtime.malformed-spri");
    ASSERT_TRUE(
        runtime.PrepareModule(
            PreparationRequest(module),
            events)
            .accepted);
    ASSERT_TRUE(events->Last<ModulePreparationEvent>()->prepared);

    const ProgramInvocation invocation = Invocation(module);
    EncodedInvocationEnvelope malformed =
        InvocationRequest(invocation);
    ASSERT_FALSE(malformed.input_payload.empty());
    malformed.input_payload[0] = Byte('X');
    CancellationSource cancellation(invocation.invocation_id);
    const ProgramRuntimeSubmission rejected =
        StartInvocation(runtime,
            std::move(malformed),
            cancellation.token(),
            events);
    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(
        rejected.error.code,
        WorkerRejectionCode::InvalidArgument);
    EXPECT_TRUE(actions->requests.empty());
}

TEST(ProgramRuntime, ReportsMalformedSprmThroughPreparationEvent)
{
    ProgramRuntime runtime(TestRuntimeConfig());
    ASSERT_TRUE(runtime.initialized())
        << runtime.initialization_diagnostic();
    auto events = std::make_shared<RecordingEventSink>();
    const ProgramModule module =
        Module("test.runtime.malformed-sprm");
    ModulePreparationRequest request =
        PreparationRequest(module);
    ASSERT_FALSE(request.module.payload.empty());
    request.module.payload[0] = Byte('X');

    const ProgramRuntimeSubmission accepted =
        runtime.PrepareModule(std::move(request), events);
    ASSERT_TRUE(accepted.accepted);
    const ModulePreparationEvent* event =
        events->Last<ModulePreparationEvent>();
    ASSERT_NE(event, nullptr);
    EXPECT_FALSE(event->prepared);
    EXPECT_EQ(event->error.code, WorkerRejectionCode::InvalidArgument);
    EXPECT_EQ(runtime.definitions().size(), 0u);
}

TEST(ProgramRuntime, RejectedPreparationDoesNotBlockCorrectedRetry)
{
    ProgramRuntime runtime(TestRuntimeConfig());
    ASSERT_TRUE(runtime.initialized())
        << runtime.initialization_diagnostic();
    auto events = std::make_shared<RecordingEventSink>();

    ProgramModule invalid =
        Module("test.runtime.atomic-prepare");
    invalid.budgets.maximum_instructions = 0;
    invalid.identity.module_hash =
        ComputeProgramModuleHashV1(invalid);
    ASSERT_TRUE(
        runtime.PrepareModule(
            PreparationRequest(invalid, WorkerCommandSequence(10)),
            events)
            .accepted);
    const ModulePreparationEvent* rejected =
        events->Last<ModulePreparationEvent>();
    ASSERT_NE(rejected, nullptr);
    EXPECT_FALSE(rejected->prepared);
    EXPECT_EQ(runtime.definitions().size(), 0u);
    EXPECT_EQ(runtime.definitions().verified_cache_size(), 0u);

    const ProgramModule corrected =
        Module("test.runtime.atomic-prepare");
    ASSERT_NE(
        invalid.identity.module_hash,
        corrected.identity.module_hash);
    ASSERT_TRUE(
        runtime.PrepareModule(
            PreparationRequest(
                corrected,
                WorkerCommandSequence(11)),
            events)
            .accepted);
    const ModulePreparationEvent* prepared =
        events->Last<ModulePreparationEvent>();
    ASSERT_NE(prepared, nullptr);
    EXPECT_TRUE(prepared->prepared)
        << prepared->error.message;
    EXPECT_EQ(runtime.definitions().size(), 1u);
    EXPECT_EQ(runtime.definitions().verified_cache_size(), 1u);
    EXPECT_NE(
        runtime.definitions().Resolve(corrected.identity),
        nullptr);
}

TEST(ProgramRuntime, RejectsMismatchedAndTerminatesStaleCompletion)
{
    ProgramRuntime runtime(TestRuntimeConfig());
    ASSERT_TRUE(runtime.initialized())
        << runtime.initialization_diagnostic();
    auto events = std::make_shared<RecordingEventSink>();
    auto actions = std::make_shared<RecordingActionSink>();
    runtime.BindActionSink(actions);

    const ProgramModule module =
        Module("test.runtime.stale-state");
    ASSERT_TRUE(
        runtime.PrepareModule(
            PreparationRequest(module),
            events)
            .accepted);
    ASSERT_TRUE(events->Last<ModulePreparationEvent>()->prepared);
    const ProgramInvocation invocation = Invocation(module);
    CancellationSource cancellation(invocation.invocation_id);
    ASSERT_TRUE(
        StartInvocation(runtime,
            InvocationRequest(invocation),
            cancellation.token(),
            events)
            .accepted);
    ASSERT_EQ(actions->requests.size(), 1u);
    const ProgramActionRequest request = actions->requests.front();

    ProgramActionResolution wrong =
        Completion(request, WorksetEpoch(5));
    wrong.request_id =
        ProgramActionRequestId(request.request_id.value() + 1);
    EXPECT_FALSE(
        runtime.DeliverActionResolution(wrong).accepted);
    wrong = Completion(request, WorksetEpoch(5));
    wrong.operation = ProgramHostOperation::InvokeAction;
    EXPECT_FALSE(
        runtime.DeliverActionResolution(wrong).accepted);
    wrong = Completion(request, WorksetEpoch(5));
    wrong.attempt_id = AttemptId(999);
    EXPECT_FALSE(
        runtime.DeliverActionResolution(wrong).accepted);
    wrong = Completion(request, WorksetEpoch(5));
    wrong.workset_epoch = WorksetEpoch(4);
    const ProgramRuntimeSubmission stale_origin =
        runtime.DeliverActionResolution(wrong);
    EXPECT_FALSE(stale_origin.accepted);
    EXPECT_EQ(
        stale_origin.error.code,
        WorkerRejectionCode::WorksetEpochMismatch);
    EXPECT_FALSE(runtime.TakeFinishedExecution(
        invocation.invocation_id,
        invocation.attempt_id).taken);

    ProgramActionResolution stale = Completion(
        request,
        request.expected_epoch,
        ProgramActionResolutionStatus::StaleEpoch);
    stale.code = "stale_epoch";
    stale.message = "state preparation observed an obsolete epoch";
    ASSERT_TRUE(
        runtime.DeliverActionResolution(std::move(stale))
            .accepted);
    const auto taken = runtime.TakeFinishedExecution(
        invocation.invocation_id,
        invocation.attempt_id);
    const ProgramExecutionFinished* terminal = taken.finished
        ? &*taken.finished
        : nullptr;
    ASSERT_NE(terminal, nullptr);
    EXPECT_EQ(
        terminal->status,
        InvocationTerminalStatus::Failed);
    EXPECT_EQ(
        terminal->error.code,
        WorkerRejectionCode::InvalidArgument);
    const DecodeResult<ProgramResult> decoded =
        DecodeProgramResultV1(terminal->output_payload);
    ASSERT_TRUE(decoded)
        << decoded.status.message;
    EXPECT_EQ(
        decoded.value->infrastructure,
        ProgramInfrastructureStatus::ContractFailed);
    ASSERT_FALSE(decoded.value->diagnostics.empty());
    EXPECT_EQ(
        decoded.value->diagnostics.back().code,
        "stale_epoch");
    EXPECT_EQ(actions->requests.size(), 1u);
}

TEST(ProgramRuntime, RejectsMalformedCompletionAfterBaselinePreparation)
{
    ProgramRuntime runtime(TestRuntimeConfig());
    ASSERT_TRUE(runtime.initialized())
        << runtime.initialization_diagnostic();
    auto events = std::make_shared<RecordingEventSink>();
    auto actions = std::make_shared<RecordingActionSink>();
    runtime.BindActionSink(actions);

    const ProgramModule module =
        Module("test.runtime.malformed-completion");
    ASSERT_TRUE(
        runtime.PrepareModule(
            PreparationRequest(module),
            events)
            .accepted);
    ASSERT_TRUE(events->Last<ModulePreparationEvent>()->prepared);
    const ProgramInvocation invocation = Invocation(module);
    CancellationSource cancellation(invocation.invocation_id);
    ASSERT_TRUE(
        StartInvocation(runtime,
            InvocationRequest(invocation),
            cancellation.token(),
            events)
            .accepted);
    ASSERT_EQ(actions->requests.size(), 1u);
    ASSERT_TRUE(
        runtime.DeliverActionResolution(
            Completion(actions->requests.front(), WorksetEpoch(5)))
            .accepted);
    EXPECT_FALSE(runtime.Pump());
    ASSERT_EQ(actions->requests.size(), 2u);
    const ProgramActionRequest finish = actions->requests.back();

    ProgramActionResolution malformed =
        Completion(finish, WorksetEpoch(5));
    malformed.invocation_id = InvocationId(999);
    const ProgramRuntimeSubmission rejected =
        runtime.DeliverActionResolution(std::move(malformed));
    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(
        rejected.error.code,
        WorkerRejectionCode::InvocationMismatch);
    EXPECT_FALSE(runtime.TakeFinishedExecution(
        invocation.invocation_id,
        invocation.attempt_id).taken);

    ASSERT_TRUE(
        runtime.DeliverActionResolution(
            Completion(finish, WorksetEpoch(5)))
            .accepted);
    EXPECT_FALSE(runtime.Pump());
    EXPECT_TRUE(runtime.TakeFinishedExecution(
        invocation.invocation_id,
        invocation.attempt_id).taken);
}

} // namespace
