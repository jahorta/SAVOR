#include <gtest/gtest.h>

#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Composition/InteractionComposition.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

using namespace savor::runtime::program;
using namespace savor::runtime::program::capabilities;
using namespace savor::runtime::program::composition;

ExactDependencyIdentity Reducer(std::string id)
{
    const std::string contract = id + "/1:(typed)->typed";
    return ExactDependency(std::move(id), 1, contract);
}

InteractionActionSet Actions()
{
    return {
        .acquire_input_lease = CanonicalActionIdentity(
            CanonicalAction::InputAcquireLease),
        .publish_held = CanonicalActionIdentity(
            CanonicalAction::InputPublishHeld),
        .publish_pulse = CanonicalActionIdentity(
            CanonicalAction::InputPublishPulse),
        .publish_sequence = CanonicalActionIdentity(
            CanonicalAction::InputPublishSequence),
        .neutralize = CanonicalActionIdentity(
            CanonicalAction::InputNeutralize),
        .await_guest_poll = CanonicalActionIdentity(
            CanonicalAction::InputAwaitGuestPoll),
        .subscribe_group = CanonicalActionIdentity(
            CanonicalAction::StopPointsSubscribeGroup),
        .continue_until = CanonicalActionIdentity(
            CanonicalAction::ExecutionContinueUntil),
        .step_instructions = CanonicalActionIdentity(
            CanonicalAction::ExecutionStepInstructions),
        .step_frames = CanonicalActionIdentity(
            CanonicalAction::ExecutionStepFrames),
    };
}

SemanticPointReference Point(std::string id, std::uint32_t pc)
{
    return {
        .capability_pack = BattlePackIdentity(),
        .canonical_id = std::move(id),
        .kind = SemanticPointKind::ProgramCounter,
        .physical_pc = pc,
    };
}

InteractionDefinition Definition()
{
    const auto u64 = TypeRef::Builtin(BuiltinType::U64);
    const auto input_frame = CanonicalRuntimeType(
        CanonicalRuntimeSchema::InputFramePayload);
    return {
        .canonical_id = "test.two-segment-input",
        .revision = 1,
        .source_name = "test.interaction",
        .parameters = {
            {"first_input", input_frame},
            {"second_input", input_frame},
        },
        .state_type = u64,
        .output_type = u64,
        .lease_type = CanonicalActionOutputType(
            CanonicalAction::InputAcquireLease),
        .subscription_type = CanonicalActionOutputType(
            CanonicalAction::StopPointsSubscribeGroup),
        .point_receipt_type = CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil),
        .input_publication_receipt_type =
            CanonicalActionOutputType(
                CanonicalAction::InputPublishHeld),
        .input_neutral_witness_type =
            CanonicalActionOutputType(
                CanonicalAction::InputNeutralize),
        .input_poll_receipt_type =
            CanonicalActionOutputType(
                CanonicalAction::InputAwaitGuestPoll),
        .segment_result_type = u64,
        .adaptive_transition_type = u64,
        .adaptive_segment_id_type = u64,
        .initialize_reducer = Reducer("test.interaction.initialize"),
        .finalize_reducer = Reducer("test.interaction.finalize"),
        .actions = Actions(),
        .segments = {
            {
                .canonical_id = "select",
                .gate_alternatives = {
                    Point("soa.battle.point.TurnInputs", 0x80071740u),
                },
                .requested_input_parameter = 0,
                .input_kind = InteractionInputKind::Held,
                .acknowledgement =
                    InputAcknowledgementPolicy::RequestAndRelease,
                .step_off_current_source = true,
                .reached_instruction =
                    ReachedInstructionPolicy::ExecuteUnderHeldRequest,
                .deadline_milliseconds = 1000,
                .release_witness_point =
                    "soa.battle.point.BattleMacroInputReadyGate",
                .completion_mapper =
                    Reducer("test.interaction.map.select"),
                .static_next_segment = "confirm",
            },
            {
                .canonical_id = "confirm",
                .gate_alternatives = {
                    Point(
                        "soa.battle.point.BattleMacroInputReadyGate",
                        0x8007cec4u),
                },
                .requested_input_parameter = 1,
                .input_kind = InteractionInputKind::Pulse,
                .acknowledgement =
                    InputAcknowledgementPolicy::RequestAndRelease,
                .step_off_current_source = true,
                .reached_instruction =
                    ReachedInstructionPolicy::LeavePaused,
                .deadline_milliseconds = 1000,
                .release_witness_point =
                    "soa.battle.point.BattleMacroDirectCommandQueued",
                .memory_change_observation = CanonicalActionIdentity(
                    CanonicalAction::GuestReadU32),
                .memory_change_address = 0x803469a8u,
                .memory_change_value_type =
                    TypeRef::Builtin(BuiltinType::U32),
                .maximum_memory_polls = 8,
                .completion_mapper =
                    Reducer("test.interaction.map.confirm"),
            },
        },
        .first_segment = "select",
        .budgets = {
            .maximum_instructions = 10000,
            .maximum_action_requests = 128,
            .maximum_emissions = 16,
            .active_deadline_milliseconds = 5000,
        },
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

TEST(InteractionComposition, LowersTemporalContractToOrdinaryIr)
{
    ProgramModule module;
    const auto result = LowerInteraction(Definition(), module);
    ASSERT_TRUE(result) << result.diagnostics.front().message;
    ASSERT_EQ(module.functions.size(), 1u);
    const auto instructions = Instructions(module);

    EXPECT_EQ(
        std::ranges::count(
            instructions,
            InstructionOpcode::EnterScope,
            [](const Instruction* instruction)
            {
                return instruction->opcode;
            }),
        3);
    EXPECT_EQ(
        std::ranges::count(
            instructions,
            InstructionOpcode::DeferCompensation,
            [](const Instruction* instruction)
            {
                return instruction->opcode;
            }),
        1);

    const auto publish = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "select/publish-before-step");
        });
    const auto step_source = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "select/step-source-with-request");
        });
    const auto wait = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "select/exact-stop-and-input-epoch");
        });
    const auto request_poll = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "select/request-receipt-before-neutral");
        });
    const auto neutral = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "select/publish-neutral");
        });
    const auto release = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "select/release-witness");
        });
    ASSERT_NE(publish, instructions.end());
    ASSERT_NE(step_source, instructions.end());
    ASSERT_NE(wait, instructions.end());
    ASSERT_NE(request_poll, instructions.end());
    ASSERT_NE(neutral, instructions.end());
    ASSERT_NE(release, instructions.end());
    EXPECT_LT(publish - instructions.begin(), step_source - instructions.begin());
    EXPECT_LT(step_source - instructions.begin(), wait - instructions.begin());
    EXPECT_LT(wait - instructions.begin(), request_poll - instructions.begin());
    EXPECT_LT(request_poll - instructions.begin(), neutral - instructions.begin());
    EXPECT_LT(neutral - instructions.begin(), release - instructions.begin());
    for (const auto* instruction : instructions)
    {
        if (instruction->opcode == InstructionOpcode::AwaitAction)
            EXPECT_EQ(instruction->operands.size(), 1u);
        EXPECT_FALSE(instruction->selector.contains("deadline="));
        EXPECT_FALSE(instruction->selector.contains("movie="));
        EXPECT_FALSE(instruction->selector.contains("stall="));
    }
    const auto stop_config = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            if (!instruction->literal ||
                !instruction->selector.contains(
                    "gate/static-config"))
            {
                return false;
            }
            const auto* bytes =
                std::get_if<std::vector<Byte>>(
                    &instruction->literal->payload);
            return bytes && bytes->size() >= 4 &&
                (*bytes)[0] == 'S' &&
                (*bytes)[1] == 'G' &&
                (*bytes)[2] == 'C' &&
                (*bytes)[3] == '1';
        });
    EXPECT_NE(stop_config, instructions.end());
}

TEST(InteractionComposition, MemoryPollingPreservesBaselineThenNeutralFrame)
{
    ProgramModule module;
    ASSERT_TRUE(LowerInteraction(Definition(), module));
    const auto instructions = Instructions(module);
    const auto baseline = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "memory-baseline-before-advance");
        });
    const auto frame = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "one-neutral-frame-between-polls");
        });
    const auto neutral = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "confirm/publish-neutral");
        });
    const auto after = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "memory-observation-after-neutral-frame");
        });
    ASSERT_NE(baseline, instructions.end());
    ASSERT_NE(neutral, instructions.end());
    ASSERT_NE(frame, instructions.end());
    ASSERT_NE(after, instructions.end());
    EXPECT_LT(baseline - instructions.begin(), neutral - instructions.begin());
    EXPECT_LT(neutral - instructions.begin(), frame - instructions.begin());
    EXPECT_LT(frame - instructions.begin(), after - instructions.begin());
    const auto advance_kind =
        [&](std::string_view label)
            -> std::optional<Byte>
    {
        const auto found = std::ranges::find_if(
            instructions,
            [&](const Instruction* instruction)
            {
                return instruction->literal &&
                    instruction->selector.contains(label);
            });
        if (found == instructions.end())
            return std::nullopt;
        const auto* bytes =
            std::get_if<std::vector<Byte>>(
                &(*found)->literal->payload);
        return bytes && bytes->size() > 4
            ? std::optional<Byte>((*bytes)[4])
            : std::nullopt;
    };
    const auto instruction_kind =
        advance_kind("source-step/static-config");
    const auto frame_kind =
        advance_kind("neutral-frame/static-config");
    ASSERT_TRUE(instruction_kind);
    ASSERT_TRUE(frame_kind);
    EXPECT_EQ(*instruction_kind, 1u);
    EXPECT_EQ(*frame_kind, 2u);
}

TEST(InteractionComposition, AdaptiveReducerCanSelectOnlyKnownSegmentsOrComplete)
{
    auto definition = Definition();
    for (auto& segment : definition.segments)
        segment.static_next_segment.reset();
    definition.advance_reducer = Reducer("test.interaction.advance");
    definition.adaptive_state_field = "next_state";
    definition.adaptive_segment_field = "next_segment";

    ProgramModule module;
    ASSERT_TRUE(LowerInteraction(definition, module));
    const auto& function = module.functions.front();
    for (const auto& segment : definition.segments)
    {
        const auto block = std::ranges::find_if(
            function.blocks,
            [&](const BasicBlock& candidate)
            {
                return std::ranges::any_of(
                    candidate.instructions,
                    [&](const Instruction& instruction)
                    {
                        return instruction.selector.contains(
                            "segment/" + segment.canonical_id + "/scope");
                    });
            });
        ASSERT_NE(block, function.blocks.end());
        EXPECT_EQ(block->terminator.kind, TerminatorKind::EnumSwitch);
        EXPECT_EQ(
            block->terminator.enum_cases.size(),
            definition.segments.size() + 1);
        std::vector<std::string> projections;
        for (const Instruction& instruction :
             block->instructions)
        {
            if (instruction.opcode ==
                InstructionOpcode::RecordProject)
            {
                projections.push_back(
                    instruction.selector);
            }
        }
        std::ranges::sort(projections);
        EXPECT_EQ(
            projections,
            (std::vector<std::string>{
                "next_segment",
                "next_state"}));
    }
}

TEST(InteractionComposition, InvalidReleaseWitnessIsRejectedAtomically)
{
    auto definition = Definition();
    definition.segments.front().release_witness_point.reset();
    ProgramModule module;
    module.ir_version = 99;
    const auto before = module;
    const auto result = LowerInteraction(definition, module);
    EXPECT_FALSE(result);
    EXPECT_EQ(module, before);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(
        result.diagnostics.front().code,
        "interaction.missing_release_witness");
}

} // namespace
