#include <gtest/gtest.h>

#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "Runner/Runtime/ProgramRuntime/Composition/SemanticObservationComposition.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "Runner/Runtime/ProgramRuntime/Store/ProgramDefinitionStore.h"
#include "Runner/Runtime/ProgramRuntime/Verify/ProgramVerifier.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

using namespace savor::runtime::program;
using namespace savor::runtime::program::capabilities;
using namespace savor::runtime::program::composition;

ExactDependencyIdentity Reducer(std::string id)
{
    return ExactDependency(
        id,
        1,
        id + "/1:(ContinueUntilResult)->typed");
}

SemanticObservationComposition Definition()
{
    const auto u32 = TypeRef::Builtin(BuiltinType::U32);
    return {
        .canonical_id = "test.seed-observation",
        .revision = 1,
        .source_name = "test.semantic",
        .await = {
            .alternatives = {{
                .capability_pack = FieldPackIdentity(),
                .canonical_id =
                    "soa.field.point.prebattle.BeforeRandSeedSet",
                .kind = SemanticPointKind::ProgramCounter,
                .physical_pc = 0x80101e48u,
            }},
            .hit_time_samples = {{
                .canonical_id = "soa.field.sample.RngSeed",
                .result_type = u32,
                .maximum_bytes = 4,
                .required = true,
                .projection_reducer =
                    Reducer("test.project.rng-seed"),
            }},
            .current_point = CurrentPointPolicy::FutureOnly,
            .continue_until_action = CanonicalActionIdentity(
                CanonicalAction::ExecutionContinueUntil),
            .receipt_type = CanonicalActionOutputType(
                CanonicalAction::
                    ExecutionContinueUntil),
        },
        .address_expressions = {
            {
                .canonical_id = "rng",
                .kind = AddressExpressionKind::RegisteredSymbol,
                .result_type = TypeRef::Builtin(BuiltinType::U64),
                .symbol_or_field = "soa.field.address.RNG_SEED",
                .absolute_address = 0x803469A8u,
            },
            {
                .canonical_id = "rng_plus_four",
                .kind = AddressExpressionKind::CheckedOffset,
                .result_type = TypeRef::Builtin(BuiltinType::U64),
                .base_expression = "rng",
                .checked_offset = 4,
            },
        },
        .observations = {
            {
                .canonical_id = "sampled_rng",
                .result_type = u32,
                .permitted_modes = {
                    ObservationAcquisitionMode::HitTimeSample,
                },
                .hit_time_sample = "soa.field.sample.RngSeed",
            },
            {
                .canonical_id = "paused_rng",
                .result_type = u32,
                .address_expression = "rng",
                .permitted_modes = {
                    ObservationAcquisitionMode::PausedAtPoint,
                },
                .paused_action = CanonicalActionIdentity(
                    CanonicalAction::GuestReadU32),
            },
        },
        .ordered_uses = {
            {
                .canonical_id = "at_hit",
                .definition_id = "sampled_rng",
                .mode = ObservationAcquisitionMode::HitTimeSample,
                .requirement = ObservationRequirement::Required,
                .baseline_name = "seed",
                .baseline = BaselineUpdatePolicy::Latest,
                .publication =
                    ObservationPublicationPolicy::AuthoritativeEmission,
            },
            {
                .canonical_id = "after_frame",
                .definition_id = "paused_rng",
                .mode = ObservationAcquisitionMode::PausedAtPoint,
                .requirement = ObservationRequirement::Required,
                .advance_before_observation =
                    ObservationAdvanceKind::StepFrame,
                .advance_action = CanonicalActionIdentity(
                    CanonicalAction::ExecutionStepFrames),
                .advance_count = 1,
            },
        },
        .output_type = u32,
    };
}

std::vector<const Instruction*> Instructions(const ProgramModule& module)
{
    std::vector<const Instruction*> output;
    for (const auto& function : module.functions)
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions)
                output.push_back(&instruction);
    return output;
}

ProgramVerificationResult CompleteAndVerify(
    ProgramModule module,
    ProgramFunctionId lowered)
{
    const auto u64 = TypeRef::Builtin(BuiltinType::U64);
    const auto boolean = TypeRef::Builtin(BuiltinType::Bool);
    const auto output = module.functions.front().output_type;
    module.functions.push_back({
        .id = ProgramFunctionId(10000),
        .name = "run",
        .arguments = {{ProgramValueId(10000), u64}},
        .output_type = output,
        .domain_outcome_type = boolean,
        .entry_block = ProgramBlockId(10000),
        .blocks = {{
            .id = ProgramBlockId(10000),
            .instructions = {
                {
                    .id = ProgramInstructionId(10000),
                    .opcode = InstructionOpcode::CallLocal,
                    .source_location =
                        ProgramSourceLocationId(10000),
                    .result = ValueDefinition{
                        ProgramValueId(10001), output},
                    .target = {
                        .kind =
                            InstructionTargetKind::
                                LocalFunction,
                        .local_function = lowered,
                    },
                },
                {
                    .id = ProgramInstructionId(10001),
                    .opcode = InstructionOpcode::Constant,
                    .source_location =
                        ProgramSourceLocationId(10001),
                    .result = ValueDefinition{
                        ProgramValueId(10002), boolean},
                    .literal = LiteralValue{
                        .type = boolean,
                        .payload = true,
                    },
                    .ordinal = 1,
                },
            },
            .terminator = {
                .kind = TerminatorKind::Return,
                .source_location =
                    ProgramSourceLocationId(10002),
                .return_value = ProgramValueId(10001),
                .domain_outcome = ProgramValueId(10002),
            },
        }},
        .exported = true,
    });
    module.entrypoints = {{
        .name = "run",
        .function = ProgramFunctionId(10000),
        .input_type = u64,
        .output_type = output,
        .domain_outcome_type = boolean,
        .required_capability_packs =
            module.required_capability_packs,
        .accepted_policies = {
            .state_policies = {
                InvocationStatePolicy::RestoreBaseline},
            .execution_intents = {ExecutionIntent::Live},
        },
    }};
    module.accepted_policies =
        module.entrypoints.front().accepted_policies;
    module.budgets = {
        .maximum_instructions = 10000,
        .maximum_calls = 100,
        .maximum_call_depth = 16,
        .maximum_action_requests = 100,
        .maximum_emissions = 100,
        .maximum_artifacts = 10,
        .maximum_values = 10000,
        .maximum_value_bytes = 4 * 1024 * 1024,
        .maximum_trace_events = 10000,
    };
    module.source_map.entries.push_back({
        .id = ProgramSourceLocationId(10000),
        .function = ProgramFunctionId(10000),
        .block = ProgramBlockId(10000),
        .instruction = ProgramInstructionId(10000),
        .source_name = "test.semantic.wrapper",
        .semantic_path = "call",
    });
    module.source_map.entries.push_back({
        .id = ProgramSourceLocationId(10001),
        .function = ProgramFunctionId(10000),
        .block = ProgramBlockId(10000),
        .instruction = ProgramInstructionId(10001),
        .source_name = "test.semantic.wrapper",
        .semantic_path = "domain",
    });
    module.source_map.entries.push_back({
        .id = ProgramSourceLocationId(10002),
        .function = ProgramFunctionId(10000),
        .block = ProgramBlockId(10000),
        .source_name = "test.semantic.wrapper",
        .semantic_path = "return",
    });
    module.identity = {
        .canonical_id = "test.semantic.verified",
        .revision = 1,
    };
    module.identity.module_hash =
        ComputeProgramModuleHashV1(module);

    ProgramDefinitionStore modules;
    TypeSchemaRegistry schemas;
    ActionRegistry actions(&schemas);
    CapabilityPackRegistry packs(&schemas, &actions);
    if (!RegisterSourceCapabilityPacks(
             schemas,
             actions,
             packs)
             .success)
    {
        return {};
    }
    const auto stored = modules.RegisterCompiled(
        std::move(module));
    if (!stored.success)
        return {};
    ProgramVerifier verifier(
        modules,
        schemas,
        actions,
        packs);
    return verifier.Verify(
        stored.module->identity,
        SupportedSoaUsaCompatibility());
}

TEST(SemanticObservationComposition, LowersToOrdinaryScopedActionsAndValues)
{
    ProgramModule module;
    const auto lowered = LowerSemanticObservation(Definition(), module);
    ASSERT_TRUE(lowered) << lowered.diagnostics.front().message;
    ASSERT_EQ(module.functions.size(), 1u);

    const auto instructions = Instructions(module);
    const auto stop_group_config = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction) {
            return instruction->selector.contains(
                "semantic-points/static-config");
        });
    ASSERT_NE(stop_group_config, instructions.end());
    ASSERT_TRUE((*stop_group_config)->literal.has_value());
    const auto* stop_group_bytes = std::get_if<std::vector<Byte>>(
        &(*stop_group_config)->literal->payload);
    ASSERT_NE(stop_group_bytes, nullptr);
    const auto decoded_points =
        DecodeSemanticPointSetV1(*stop_group_bytes);
    ASSERT_TRUE(decoded_points) << decoded_points.diagnostic;
    EXPECT_EQ(decoded_points.value->program_counters.size(), 1u);
    EXPECT_EQ(
        decoded_points.value->hit_time_sample_descriptor_ids.size(),
        1u);
    ASSERT_FALSE(instructions.empty());
    EXPECT_EQ(instructions.front()->opcode, InstructionOpcode::EnterScope);
    EXPECT_EQ(instructions.back()->opcode, InstructionOpcode::ExitScope);
    EXPECT_EQ(
        std::ranges::count(
            instructions,
            InstructionOpcode::AwaitAction,
            [](const Instruction* instruction)
            {
                return instruction->opcode;
            }),
        3);
    EXPECT_NE(std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->opcode == InstructionOpcode::CallReducer &&
                instruction->selector.contains("hit-time-sample");
        }), instructions.end());
    EXPECT_NE(std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->opcode == InstructionOpcode::Copy &&
                instruction->selector.contains("baseline/latest/seed");
        }), instructions.end());

    const auto step = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "explicit-observation-advance/frames");
        });
    const auto paused_read = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "paused-observation/after_frame");
        });
    ASSERT_NE(step, instructions.end());
    ASSERT_NE(paused_read, instructions.end());
    EXPECT_LT(
        std::distance(instructions.begin(), step),
        std::distance(instructions.begin(), paused_read));

    EXPECT_NE(std::ranges::find_if(
        module.action_imports,
        [](const ExactDependencyIdentity& action)
        {
            return action.canonical_id ==
                "runtime.execution.step_frames";
        }), module.action_imports.end());
    EXPECT_NE(
        std::ranges::find(
            module.required_capability_packs,
            CanonicalRuntimePackIdentity()),
        module.required_capability_packs.end());
    ASSERT_EQ(module.reducer_imports.size(), 1u);
    EXPECT_EQ(
        module.reducer_imports.front().canonical_id,
        "test.project.rng-seed");
    for (const auto* instruction : instructions)
    {
        if (instruction->opcode == InstructionOpcode::AwaitAction)
            EXPECT_EQ(instruction->operands.size(), 1u);
        EXPECT_FALSE(instruction->selector.contains("current="));
        EXPECT_FALSE(instruction->selector.contains("suppress="));
        EXPECT_FALSE(instruction->selector.contains("movie="));
        EXPECT_FALSE(instruction->selector.contains("stall="));
    }
}

TEST(SemanticObservationComposition, PreservesDeclaredObservationOrder)
{
    ProgramModule module;
    ASSERT_TRUE(LowerSemanticObservation(Definition(), module));
    const auto instructions = Instructions(module);
    const auto hit = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains("hit-time-sample");
        });
    const auto paused = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains("paused-observation");
        });
    ASSERT_NE(hit, instructions.end());
    ASSERT_NE(paused, instructions.end());
    EXPECT_LT(
        std::distance(instructions.begin(), hit),
        std::distance(instructions.begin(), paused));
    const auto stop_config = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            if (!instruction->literal ||
                !instruction->selector.contains(
                    "semantic-points/static-config"))
            {
                return false;
            }
            const auto* bytes =
                std::get_if<std::vector<Byte>>(
                    &instruction->literal->payload);
            return bytes && bytes->size() >= 4 &&
                (*bytes)[0] == 'S' &&
                (*bytes)[1] == 'P' &&
                (*bytes)[2] == 'S' &&
                (*bytes)[3] == '1';
        });
    EXPECT_NE(stop_config, instructions.end());
    EXPECT_EQ(std::ranges::count(
        instructions,
        InstructionOpcode::RecordConstruct,
        [](const Instruction* instruction)
        {
            return instruction->opcode;
        }), 4);
}

TEST(SemanticObservationComposition, InvalidDefinitionIsAtomic)
{
    ProgramModule module;
    module.ir_version = 77;
    auto definition = Definition();
    definition.await.alternatives.front().physical_pc = 0;
    const auto before = module;
    const auto result = LowerSemanticObservation(definition, module);
    EXPECT_FALSE(result);
    EXPECT_EQ(module, before);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.front().code, "semantic.zero_pc");
}

TEST(SemanticObservationComposition, RequiresExplicitPostFrameAction)
{
    ProgramModule module;
    auto definition = Definition();
    definition.ordered_uses.back().advance_action.reset();
    const auto result = LowerSemanticObservation(definition, module);
    EXPECT_FALSE(result);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(
        result.diagnostics.front().code,
        "semantic.invalid_explicit_advance");
}

TEST(SemanticObservationComposition, Sps1CodecIsDeterministicAndLinksStaticSamplers)
{
    const auto definition = Definition();
    const auto first = EncodeSemanticPointSetV1(
        definition.await.alternatives,
        definition.await.hit_time_samples);
    const auto second = EncodeSemanticPointSetV1(
        definition.await.alternatives,
        definition.await.hit_time_samples);
    EXPECT_EQ(first, second);
    ASSERT_GE(first.size(), 4u);
    EXPECT_EQ(
        std::string(first.begin(), first.begin() + 4),
        "SPS1");

    const auto decoded = DecodeSemanticPointSetV1(first);
    ASSERT_TRUE(decoded) << decoded.diagnostic;
    EXPECT_EQ(
        decoded.value->program_counters,
        (std::vector<std::uint32_t>{0x80101e48u}));
    ASSERT_EQ(
        decoded.value->hit_time_sample_descriptor_ids.size(),
        1u);
    EXPECT_NE(
        decoded.value->hit_time_sample_descriptor_ids.front(),
        0u);

    auto incompatible = definition.await.hit_time_samples;
    incompatible.front().result_type =
        TypeRef::Builtin(BuiltinType::U16);
    const auto rejected = DecodeSemanticPointSetV1(
        EncodeSemanticPointSetV1(
            definition.await.alternatives,
            incompatible));
    EXPECT_FALSE(rejected);
    EXPECT_NE(
        rejected.diagnostic.find("incompatible"),
        std::string::npos);

    auto non_pc = definition.await.alternatives;
    non_pc.front().kind = SemanticPointKind::Memory;
    EXPECT_FALSE(DecodeSemanticPointSetV1(
        EncodeSemanticPointSetV1(non_pc)));

    auto unknown = definition.await.alternatives;
    unknown.front().kind = static_cast<SemanticPointKind>(0xff);
    EXPECT_FALSE(DecodeSemanticPointSetV1(
        EncodeSemanticPointSetV1(unknown)));
}

TEST(SemanticObservationComposition, CompletedLoweringPassesProgramVerifier)
{
    auto definition = Definition();
    definition.await.hit_time_samples.clear();
    definition.observations.erase(
        definition.observations.begin());
    definition.ordered_uses = {{
        .canonical_id = "paused",
        .definition_id = "paused_rng",
        .mode =
            ObservationAcquisitionMode::PausedAtPoint,
        .requirement = ObservationRequirement::Required,
    }};
    ProgramModule module;
    const auto lowered =
        LowerSemanticObservation(definition, module);
    ASSERT_TRUE(lowered)
        << lowered.diagnostics.front().message;
    const auto verified =
        CompleteAndVerify(module, *lowered.function);
    ASSERT_TRUE(verified.success)
        << (verified.diagnostics.empty()
                ? ""
                : verified.diagnostics.front().message);
}

TEST(SemanticObservationComposition, ProgramVerifierRejectsMalformedSps1Constant)
{
    auto definition = Definition();
    definition.await.hit_time_samples.clear();
    definition.observations.erase(definition.observations.begin());
    definition.ordered_uses = {{
        .canonical_id = "paused",
        .definition_id = "paused_rng",
        .mode = ObservationAcquisitionMode::PausedAtPoint,
        .requirement = ObservationRequirement::Required,
    }};
    ProgramModule module;
    const auto lowered = LowerSemanticObservation(definition, module);
    ASSERT_TRUE(lowered) << lowered.diagnostics.front().message;
    bool mutated = false;
    for (auto& function : module.functions)
    {
        for (auto& block : function.blocks)
        {
            for (auto& instruction : block.instructions)
            {
                if (!instruction.literal || instruction.literal->type !=
                        CanonicalRuntimeType(
                            CanonicalRuntimeSchema::SemanticPointSet))
                    continue;
                auto* bytes = std::get_if<std::vector<Byte>>(
                    &instruction.literal->payload);
                ASSERT_NE(bytes, nullptr);
                bytes->push_back(0xff);
                mutated = true;
            }
        }
    }
    ASSERT_TRUE(mutated);
    const auto verified = CompleteAndVerify(module, *lowered.function);
    EXPECT_FALSE(verified.success);
    EXPECT_TRUE(std::ranges::any_of(
        verified.diagnostics,
        [](const VerificationDiagnostic& diagnostic) {
            return diagnostic.code ==
                    VerificationErrorCode::InvalidInstruction &&
                diagnostic.message.find("SPS1") != std::string::npos;
        }));
}

TEST(SemanticObservationComposition, PackQueryBuildsExactProjectedRequest)
{
    const auto catalog = BuildSourceCapabilityPackCatalog();
    const auto schema = [&](std::string_view id)
        -> SchemaIdentity
    {
        const auto found = std::ranges::find(
            catalog.schemas,
            id,
            [](const TypeSchemaDefinition& value)
            {
                return std::string_view(
                    value.identity.canonical_id);
            });
        EXPECT_NE(found, catalog.schemas.end());
        return found == catalog.schemas.end()
            ? SchemaIdentity{}
            : found->identity;
    };
    const TypeRef request = TypeRef::Named(
        schema("soa.battle.CaptureContextRequest"));
    const TypeRef context = TypeRef::Named(
        schema("soa.battle.BattleContext"));

    auto definition = Definition();
    definition.await.hit_time_samples.clear();
    definition.observations = {{
        .canonical_id = "battle_context",
        .result_type = context,
        .permitted_modes = {
            ObservationAcquisitionMode::PausedAtPoint},
        .paused_action =
            BattleCaptureContextActionIdentity(),
        .paused_action_pack = BattlePackIdentity(),
        .paused_request_type = request,
        .paused_request_fields = {
            {
                .field_name = "workset_epoch",
                .field_type =
                    TypeRef::Builtin(BuiltinType::U64),
                .source = ObservationDefinition::
                    RequestValueSource::StopReceiptField,
                .source_field = "workset_epoch",
            },
            {
                .field_name = "expected_pc",
                .field_type =
                    TypeRef::Builtin(BuiltinType::U32),
                .source = ObservationDefinition::
                    RequestValueSource::StopReceiptField,
                .source_field = "pc",
            },
        },
        .coherent_query = true,
    }};
    definition.ordered_uses = {{
        .canonical_id = "battle",
        .definition_id = "battle_context",
        .mode =
            ObservationAcquisitionMode::PausedAtPoint,
    }};
    definition.output_type = context;

    ProgramModule module;
    const auto lowered =
        LowerSemanticObservation(definition, module);
    ASSERT_TRUE(lowered)
        << lowered.diagnostics.front().message;
    const auto instructions = Instructions(module);
    const auto request_record = std::ranges::find_if(
        instructions,
        [&](const Instruction* instruction)
        {
            return instruction->opcode ==
                    InstructionOpcode::RecordConstruct &&
                instruction->result &&
                instruction->result->type == request;
    });
    ASSERT_NE(request_record, instructions.end());
    EXPECT_EQ((*request_record)->operands.size(), 2u);

    const auto verified =
        CompleteAndVerify(module, *lowered.function);
    ASSERT_TRUE(verified.success)
        << (verified.diagnostics.empty()
                ? ""
                : verified.diagnostics.front().message);
}

} // namespace
