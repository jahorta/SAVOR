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
        .apply_input_state = CanonicalActionIdentity(
            CanonicalAction::InputApplyState),
        .continue_until = CanonicalActionIdentity(
            CanonicalAction::ExecutionContinueUntil),
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
        .point_receipt_type = CanonicalActionOutputType(
            CanonicalAction::ExecutionContinueUntil),
        .input_execution_binding_type =
            CanonicalActionOutputType(
                CanonicalAction::InputApplyState),
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
                .held_through_successor = Point(
                    "soa.battle.point.BattleMacroInputReadyGate",
                    0x8007cec4u),
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
                .input_kind = InteractionInputKind::Held,
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
    ASSERT_EQ(module.functions.size(), 2u);
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
        0);

    const auto apply = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "select/apply-state-before-departure");
        });
    const auto wait = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "select/exact-stop-and-input-epoch");
        });
    const auto successor = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "select/held-through-semantic-successor");
        });
    const auto neutral = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "select/release-to-neutral");
        });
    const auto release = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "select/release/observed");
        });
    ASSERT_NE(apply, instructions.end());
    ASSERT_NE(wait, instructions.end());
    ASSERT_NE(successor, instructions.end());
    ASSERT_NE(neutral, instructions.end());
    ASSERT_NE(release, instructions.end());
    EXPECT_LT(apply - instructions.begin(), wait - instructions.begin());
    EXPECT_LT(wait - instructions.begin(), successor - instructions.begin());
    EXPECT_LT(successor - instructions.begin(), neutral - instructions.begin());
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
                (*bytes)[1] == 'P' &&
                (*bytes)[2] == 'S' &&
                (*bytes)[3] == '1';
        });
    ASSERT_NE(stop_config, instructions.end());
    const auto* stop_group_bytes = std::get_if<std::vector<Byte>>(
        &(*stop_config)->literal->payload);
    ASSERT_NE(stop_group_bytes, nullptr);
    const auto decoded_points =
        DecodeSemanticPointSetV1(*stop_group_bytes);
    ASSERT_TRUE(decoded_points) << decoded_points.diagnostic;
    EXPECT_EQ(
        decoded_points.value->program_counters,
        (std::vector<std::uint32_t>{0x80071740u}));
    EXPECT_TRUE(
        decoded_points.value->hit_time_sample_descriptor_ids.empty());
    const auto continue_config = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            if (!instruction->literal ||
                !instruction->selector.contains(
                    "select/continue/static-config"))
            {
                return false;
            }
            const auto* bytes =
                std::get_if<std::vector<Byte>>(
                    &instruction->literal->payload);
            return bytes && bytes->size() >= 6 &&
                (*bytes)[0] == 'C' &&
                (*bytes)[1] == 'U' &&
                (*bytes)[2] == 'C' &&
                (*bytes)[3] == '2' &&
                (*bytes)[4] == 1u &&
                (*bytes)[5] == 1u;
        });
    EXPECT_NE(continue_config, instructions.end());
}

TEST(InteractionComposition, MemoryPollingIsAdaptiveBoundedAndHeld)
{
    ProgramModule module;
    ASSERT_TRUE(LowerInteraction(Definition(), module));
    const auto main_function = std::ranges::find_if(
        module.functions,
        [](const ProgramFunction& function) {
            return function.name.starts_with("interact.");
        });
    const auto poll_function = std::ranges::find_if(
        module.functions,
        [](const ProgramFunction& function) {
            return function.name.starts_with(
                "interaction.memory-change.");
        });
    ASSERT_NE(main_function, module.functions.end());
    ASSERT_NE(poll_function, module.functions.end());
    const auto function_instructions = [](const ProgramFunction& function) {
        std::vector<const Instruction*> output;
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions)
                output.push_back(&instruction);
        return output;
    };
    const auto instructions = function_instructions(*main_function);
    const auto poll_instructions = function_instructions(*poll_function);
    const auto baseline = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "memory-baseline-before-input");
        });
    const auto neutral = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "confirm/release-to-neutral");
        });
    const auto synchronization = std::ranges::find_if(
        instructions,
        [](const Instruction* instruction)
        {
            return instruction->selector.contains(
                "bounded-memory-change-while-state-held");
        });
    const auto frame = std::ranges::find_if(
        poll_instructions,
        [](const Instruction* instruction) {
            return instruction->selector.contains(
                "advance-one-held-frame");
        });
    const auto after = std::ranges::find_if(
        poll_instructions,
        [](const Instruction* instruction) {
            return instruction->selector.contains(
                "observe-after-frame");
        });
    ASSERT_NE(baseline, instructions.end());
    ASSERT_NE(neutral, instructions.end());
    ASSERT_NE(synchronization, instructions.end());
    ASSERT_NE(frame, poll_instructions.end());
    ASSERT_NE(after, poll_instructions.end());
    EXPECT_LT(baseline - instructions.begin(), synchronization - instructions.begin());
    EXPECT_LT(synchronization - instructions.begin(), neutral - instructions.begin());
    EXPECT_LT(
        frame - poll_instructions.begin(),
        after - poll_instructions.begin());
    EXPECT_TRUE(std::ranges::any_of(
        poll_function->blocks,
        [&](const BasicBlock& block) {
            return std::ranges::any_of(
                block.terminator.edges,
                [&](const BlockEdge& edge) {
                    return edge.target == poll_function->blocks[1].id;
                });
        }));
    EXPECT_TRUE(std::ranges::any_of(
        poll_function->blocks,
        [](const BasicBlock& block) {
            return block.terminator.kind ==
                    TerminatorKind::StructuredFail &&
                block.terminator.failure &&
                block.terminator.failure->code ==
                    "interaction_memory_change_timeout";
        }));
    const auto advance_kind =
        [&](std::string_view label)
            -> std::optional<Byte>
    {
        const auto found = std::ranges::find_if(
            poll_instructions,
            [&](const Instruction* instruction)
            {
                return instruction->literal &&
                    instruction->selector.contains(label);
            });
        if (found == poll_instructions.end())
            return std::nullopt;
        const auto* bytes =
            std::get_if<std::vector<Byte>>(
                &(*found)->literal->payload);
        return bytes && bytes->size() > 4
            ? std::optional<Byte>((*bytes)[4])
            : std::nullopt;
    };
    const auto frame_kind =
        advance_kind("memory-change/frame/static-config");
    ASSERT_TRUE(frame_kind);
    EXPECT_EQ(*frame_kind, 2u);
    EXPECT_FALSE(std::ranges::any_of(
        poll_instructions,
        [](const Instruction* instruction)
        {
            if (!instruction->literal)
                return false;
            const auto* bytes =
                std::get_if<std::vector<Byte>>(
                    &instruction->literal->payload);
            return bytes && bytes->size() > 4 &&
                (*bytes)[0] == 'E' &&
                (*bytes)[1] == 'A' &&
                (*bytes)[2] == 'C' &&
                (*bytes)[3] == '1' &&
                (*bytes)[4] == 1u;
        }));
}

TEST(InteractionComposition, AdaptiveReducerCanSelectOnlyKnownSegmentsOrComplete)
{
    auto definition = Definition();
    for (auto& segment : definition.segments)
        segment.static_next_segment.reset();
    definition.advance_reducer = Reducer("test.interaction.advance");
    definition.adaptive_state_field = "next_state";
    definition.adaptive_segment_field = "next_segment";
    definition.adaptive_selection = InteractionAdaptiveSelectionMap{
        .segments = {
            {41, "select"},
            {7, "confirm"},
        },
        .complete = 99,
    };
    std::ranges::reverse(definition.segments);

    ProgramModule module;
    ASSERT_TRUE(LowerInteraction(definition, module));
    const auto found_function = std::ranges::find_if(
        module.functions,
        [](const ProgramFunction& function) {
            return function.name.starts_with("interact.");
        });
    ASSERT_NE(found_function, module.functions.end());
    const auto& function = *found_function;
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
        const auto mapped_case = [&](const std::int64_t value) {
            return std::ranges::find(
                block->terminator.enum_cases,
                value,
                &EnumSwitchCase::enum_value);
        };
        const auto select_case = mapped_case(41);
        const auto confirm_case = mapped_case(7);
        const auto complete_case = mapped_case(99);
        ASSERT_NE(select_case, block->terminator.enum_cases.end());
        ASSERT_NE(confirm_case, block->terminator.enum_cases.end());
        ASSERT_NE(complete_case, block->terminator.enum_cases.end());
        EXPECT_NE(select_case->edge.target, confirm_case->edge.target);
        EXPECT_NE(select_case->edge.target, complete_case->edge.target);
        EXPECT_NE(confirm_case->edge.target, complete_case->edge.target);
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

TEST(InteractionComposition, AdaptiveSelectionMapMustCoverSegmentsExactly)
{
    auto definition = Definition();
    for (auto& segment : definition.segments)
        segment.static_next_segment.reset();
    definition.advance_reducer = Reducer("test.interaction.advance");
    definition.adaptive_selection = InteractionAdaptiveSelectionMap{
        .segments = {{4, "select"}},
        .complete = 9,
    };

    ProgramModule module;
    const auto result = LowerInteraction(definition, module);
    ASSERT_FALSE(result);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(
        result.diagnostics.front().code,
        "interaction.invalid_adaptive_selection");
}

TEST(InteractionComposition, AdaptiveSelectionMapRejectsDuplicateTargets)
{
    auto definition = Definition();
    for (auto& segment : definition.segments)
        segment.static_next_segment.reset();
    definition.advance_reducer = Reducer("test.interaction.advance");
    definition.adaptive_selection = InteractionAdaptiveSelectionMap{
        .segments = {
            {4, "select"},
            {8, "select"},
        },
        .complete = 9,
    };

    ProgramModule module;
    const auto result = LowerInteraction(definition, module);
    ASSERT_FALSE(result);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(
        result.diagnostics.front().code,
        "interaction.invalid_adaptive_selection");
}

TEST(InteractionComposition, AdaptiveSelectionMapSeparatesCompletion)
{
    auto definition = Definition();
    for (auto& segment : definition.segments)
        segment.static_next_segment.reset();
    definition.advance_reducer = Reducer("test.interaction.advance");
    definition.adaptive_selection = InteractionAdaptiveSelectionMap{
        .segments = {
            {4, "select"},
            {8, "confirm"},
        },
        .complete = 8,
    };

    ProgramModule module;
    const auto result = LowerInteraction(definition, module);
    ASSERT_FALSE(result);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(
        result.diagnostics.front().code,
        "interaction.invalid_adaptive_selection");
}

TEST(InteractionComposition, PostReleaseGateRequiresHeldInput)
{
    auto definition = Definition();
    definition.segments.front().input_kind = InteractionInputKind::Neutral;
    definition.segments.front().held_through_successor.reset();
    definition.segments.front().post_release_gate = Point(
        "soa.battle.point.BattleMacroInputReadyGate",
        0x8007cec4u);
    ProgramModule module;
    module.ir_version = 99;
    const auto before = module;
    const auto result = LowerInteraction(definition, module);
    EXPECT_FALSE(result);
    EXPECT_EQ(module, before);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(
        result.diagnostics.front().code,
        "interaction.invalid_post_release_gate");
}

TEST(InteractionComposition, HeldThroughBehaviorRequiresSemanticSuccessor)
{
    auto definition = Definition();
    ASSERT_TRUE(
        definition.segments.front().held_through_successor.has_value());
    definition.segments.front()
        .held_through_successor->physical_pc = 0;
    ProgramModule module;
    module.ir_version = 99;
    const auto before = module;
    const auto result = LowerInteraction(definition, module);
    EXPECT_FALSE(result);
    EXPECT_EQ(module, before);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(
        result.diagnostics.front().code,
        "interaction.invalid_held_successor");
}

} // namespace
