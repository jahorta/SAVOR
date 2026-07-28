#include <gtest/gtest.h>

#include "Runner/Runtime/ProgramRuntime/Composition/PredicateComposition.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

using namespace savor::runtime::program;
using namespace savor::runtime::program::composition;

SchemaIdentity OptionalU32Identity()
{
    return ExactSchema(
        "test.OptionalU32",
        1,
        "optional<u32>");
}

ProgramModule ModuleWithOptionalU32()
{
    ProgramModule module;
    module.local_types.push_back({
        .identity = OptionalU32Identity(),
        .kind = TypeSchemaKind::Optional,
        .element_type = TypeRef::Builtin(BuiltinType::U32),
    });
    return module;
}

PredicateDefinition Definition(
    PredicateWitnessRequirement requirement =
        PredicateWitnessRequirement::OptionalUnavailable)
{
    const auto u32 = TypeRef::Builtin(BuiltinType::U32);
    const auto boolean = TypeRef::Builtin(BuiltinType::Bool);
    return {
        .canonical_id = "test.seed-greater-than",
        .revision = 1,
        .source_name = "test.predicate",
        .witnesses = {{
            .name = "seed",
            .parameter_type = TypeRef::Named(OptionalU32Identity()),
            .value_type = u32,
            .requirement = requirement,
        }},
        .expression = {
            {
                .kind = PredicateExpressionKind::Witness,
                .witness_index = 0,
                .result_type = u32,
                .source_label = "seed",
            },
            {
                .kind = PredicateExpressionKind::Literal,
                .literal = LiteralValue{
                    .type = u32,
                    .payload = std::uint32_t{10},
                },
                .result_type = u32,
                .source_label = "threshold",
            },
            {
                .kind = PredicateExpressionKind::Greater,
                .operands = {0, 1},
                .result_type = boolean,
                .source_label = "seed-above-threshold",
            },
        },
        .root_expression = 2,
        .check = {
            .canonical_id = "seed-check",
            .semantic_point_id =
                "soa.field.point.prebattle.BeforeRandSeedSet",
            .use_policy = PredicateUsePolicy::EmitRecord,
            .emit_condition_observation = true,
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

TEST(PredicateComposition, LowersToValuesBranchesAndDeclaredEmission)
{
    auto module = ModuleWithOptionalU32();
    const auto result = LowerPredicate(Definition(), module);
    ASSERT_TRUE(result) << result.diagnostics.front().message;

    const auto instructions = Instructions(module);
    EXPECT_NE(std::ranges::find(
        instructions,
        InstructionOpcode::OptionalIsPresent,
        [](const Instruction* instruction)
        {
            return instruction->opcode;
        }), instructions.end());
    EXPECT_NE(std::ranges::find(
        instructions,
        InstructionOpcode::OptionalExtract,
        [](const Instruction* instruction)
        {
            return instruction->opcode;
        }), instructions.end());
    EXPECT_NE(std::ranges::find(
        instructions,
        InstructionOpcode::Greater,
        [](const Instruction* instruction)
        {
            return instruction->opcode;
        }), instructions.end());
    EXPECT_GE(
        std::ranges::count(
            instructions,
            InstructionOpcode::EmitRecord,
            [](const Instruction* instruction)
            {
                return instruction->opcode;
            }),
        4);
    EXPECT_TRUE(module.action_imports.empty());
    EXPECT_TRUE(module.reducer_imports.empty());
}

TEST(PredicateComposition, PreservesFourDistinctEvaluationStatuses)
{
    auto module = ModuleWithOptionalU32();
    ASSERT_TRUE(LowerPredicate(Definition(), module));
    std::vector<std::int64_t> statuses;
    for (const auto* instruction : Instructions(module))
    {
        if (!instruction->literal)
            continue;
        const auto* value = std::get_if<EnumValue>(
            &instruction->literal->payload);
        if (value != nullptr &&
            value->schema.canonical_id ==
                "test.seed-greater-than.Evaluation")
        {
            statuses.push_back(value->value);
        }
    }
    std::ranges::sort(statuses);
    EXPECT_EQ(statuses, (std::vector<std::int64_t>{0, 1, 2, 3}));
}

TEST(PredicateComposition, RequiredMissingEvidenceFailsStructurally)
{
    auto module = ModuleWithOptionalU32();
    ASSERT_TRUE(LowerPredicate(
        Definition(PredicateWitnessRequirement::Required),
        module));
    const auto& function = module.functions.front();
    const auto missing = std::ranges::find_if(
        function.blocks,
        [](const BasicBlock& block)
        {
            return block.terminator.failure &&
                block.terminator.failure->code ==
                    "predicate.required_evidence_unavailable";
        });
    ASSERT_NE(missing, function.blocks.end());
    EXPECT_EQ(
        missing->terminator.kind,
        TerminatorKind::StructuredFail);
}

TEST(PredicateComposition, ExplicitFailPolicyDoesNotBecomeRouterGuard)
{
    auto definition = Definition();
    definition.check.use_policy = PredicateUsePolicy::StructuredFail;
    definition.check.structured_failure_code =
        "domain.seed_too_small";
    auto module = ModuleWithOptionalU32();
    ASSERT_TRUE(LowerPredicate(definition, module));
    const auto& function = module.functions.front();
    EXPECT_NE(std::ranges::find_if(
        function.blocks,
        [](const BasicBlock& block)
        {
            return block.terminator.failure &&
                block.terminator.failure->code ==
                    "domain.seed_too_small";
        }), function.blocks.end());
    EXPECT_TRUE(module.action_imports.empty());
}

TEST(PredicateComposition, DomainRejectionDefinesEveryNormalDomainOutcome)
{
    auto definition = Definition();
    definition.check.use_policy =
        PredicateUsePolicy::ReturnDomainRejection;
    definition.check.domain_rejection_code =
        "domain.seed_too_small";
    definition.check.domain_rejection = LiteralValue{
        .type = TypeRef::Builtin(BuiltinType::Bool),
        .payload = false,
    };
    definition.domain_outcome_type =
        TypeRef::Builtin(BuiltinType::Bool);
    definition.domain_success = LiteralValue{
        .type = TypeRef::Builtin(BuiltinType::Bool),
        .payload = true,
    };

    auto module = ModuleWithOptionalU32();
    ASSERT_TRUE(LowerPredicate(definition, module));
    const auto& function = module.functions.front();
    EXPECT_EQ(
        std::ranges::count_if(
            function.blocks,
            [](const BasicBlock& block)
            {
                return block.terminator.kind ==
                        TerminatorKind::Return &&
                    block.terminator.domain_outcome.has_value();
            }),
        4);
}

TEST(PredicateComposition, NoncanonicalExpressionIsRejectedAtomically)
{
    auto definition = Definition();
    definition.expression[2].operands = {0, 3};
    auto module = ModuleWithOptionalU32();
    const auto before = module;
    const auto result = LowerPredicate(definition, module);
    EXPECT_FALSE(result);
    EXPECT_EQ(module, before);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(
        result.diagnostics.front().code,
        "predicate.noncanonical_expression");
}

} // namespace
