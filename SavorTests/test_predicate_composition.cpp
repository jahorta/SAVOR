#include <gtest/gtest.h>

#include "Runner/Runtime/ProgramRuntime/Composition/PredicateComposition.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

using namespace savor::runtime::program;
using namespace savor::runtime::program::composition;

PredicateDefinition Definition()
{
    const auto u32 = TypeRef::Builtin(BuiltinType::U32);
    const auto boolean = TypeRef::Builtin(BuiltinType::Bool);
    return {
        .canonical_id = "test.seed-greater-than",
        .revision = 1,
        .source_name = "test.predicate",
        .witnesses = {{.name = "seed", .value_type = u32}},
        .expression = {
            {.kind = PredicateExpressionKind::Witness, .witness_index = 0,
             .result_type = u32, .source_label = "seed"},
            {.kind = PredicateExpressionKind::Literal,
             .literal = LiteralValue{.type = u32, .payload = std::uint32_t{10}},
             .result_type = u32, .source_label = "threshold"},
            {.kind = PredicateExpressionKind::Greater, .operands = {0, 1},
             .result_type = boolean, .source_label = "seed-above-threshold"},
        },
        .root_expression = 2,
    };
}

PredicateCheckUse Check()
{
    return {
        .canonical_id = "seed-check",
        .semantic_point_id = "soa.field.point.prebattle.BeforeRandSeedSet",
        .reaction = PredicateReaction::RecordAndContinue,
        .emit_evidence = true,
        .participates_in_aggregation = true,
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

TEST(PredicateComposition, LowersRequiredWitnessAndDeclaredEvidence)
{
    ProgramModule module;
    const auto result = LowerPredicate(Definition(), Check(), module);
    ASSERT_TRUE(result) << result.diagnostics.front().message;
    const auto instructions = Instructions(module);
    EXPECT_EQ(std::ranges::count(instructions, InstructionOpcode::OptionalIsPresent,
        [](const Instruction* value) { return value->opcode; }), 0);
    EXPECT_NE(std::ranges::find(instructions, InstructionOpcode::Greater,
        [](const Instruction* value) { return value->opcode; }), instructions.end());
    EXPECT_EQ(std::ranges::count(instructions, InstructionOpcode::EmitRecord,
        [](const Instruction* value) { return value->opcode; }), 2);
}

TEST(PredicateComposition, ExposesOnlyPassedAndFailedEvaluations)
{
    ProgramModule module;
    ASSERT_TRUE(LowerPredicate(Definition(), Check(), module));
    std::vector<std::int64_t> statuses;
    for (const auto* instruction : Instructions(module)) {
        if (!instruction->literal) continue;
        const auto* value = std::get_if<EnumValue>(&instruction->literal->payload);
        if (value && value->schema.canonical_id == "test.seed-greater-than.Evaluation")
            statuses.push_back(value->value);
    }
    std::ranges::sort(statuses);
    EXPECT_EQ(statuses, (std::vector<std::int64_t>{0, 1}));
}

TEST(PredicateComposition, AbortOnFailRemainsPureForCallerOwnedRejection)
{
    auto check = Check();
    check.reaction = PredicateReaction::AbortOnFail;
    ProgramModule module;
    ASSERT_TRUE(LowerPredicate(Definition(), check, module));
    EXPECT_EQ(std::ranges::count_if(module.functions.front().blocks,
        [](const BasicBlock& block) {
            return block.terminator.kind == TerminatorKind::Return
                && block.terminator.domain_outcome.has_value();
        }), 0);
}

TEST(PredicateComposition, SupportsCheckedArithmetic)
{
    auto definition = Definition();
    definition.expression.insert(definition.expression.begin() + 2, {
        .kind = PredicateExpressionKind::Add,
        .operands = {0, 1},
        .result_type = TypeRef::Builtin(BuiltinType::U32),
        .source_label = "sum",
    });
    definition.expression[3].operands = {2, 1};
    definition.root_expression = 3;
    ProgramModule module;
    ASSERT_TRUE(LowerPredicate(definition, Check(), module));
    const auto instructions = Instructions(module);
    EXPECT_NE(std::ranges::find(instructions, InstructionOpcode::AddChecked,
        [](const Instruction* value) { return value->opcode; }), instructions.end());
}

TEST(PredicateComposition, NoncanonicalExpressionIsRejectedAtomically)
{
    auto definition = Definition();
    definition.expression[2].operands = {0, 3};
    ProgramModule module;
    const auto before = module;
    const auto result = LowerPredicate(definition, Check(), module);
    EXPECT_FALSE(result);
    EXPECT_EQ(module, before);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.front().code, "predicate.noncanonical_expression");
}

} // namespace
