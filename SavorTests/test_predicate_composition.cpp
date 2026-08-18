#include <gtest/gtest.h>

#include "Runner/Breakpoints/BpRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Composition/PredicateComposition.h"
#include "Runner/Runtime/Predicates/PredicateExecution.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

using namespace savor::runtime::program;
using namespace savor::runtime::program::composition;
using namespace savor::runtime::predicates;

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

PredicateEvaluationPolicy Check()
{
    return {
        .predicate_group_revision_id = 11,
        .execution_binding_revision_id = 12,
        .member_ordinal = 0,
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

TEST(PredicateAuthoringIdentity, SemanticFingerprintsExcludeOwnIdentityButRetainExecutableContent)
{
    const auto original_definition = Definition();
    auto renamed_definition_identity = original_definition;
    renamed_definition_identity.canonical_id = "predicate.definition/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    renamed_definition_identity.revision = 99;
    renamed_definition_identity.source_name = "different presentation source";
    EXPECT_EQ(ComputePredicateDefinitionSemanticHashV1(original_definition),
              ComputePredicateDefinitionSemanticHashV1(renamed_definition_identity));

    auto changed_definition = renamed_definition_identity;
    changed_definition.expression[1].literal->payload = std::uint32_t{11};
    EXPECT_NE(ComputePredicateDefinitionSemanticHashV1(original_definition),
              ComputePredicateDefinitionSemanticHashV1(changed_definition));

    PredicateExecutionBindingV1 binding{
        .execution_binding_revision_id = 10,
        .canonical_id = "predicate.binding/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        .revision = 4,
        .definition = {
            20,
            ComputePredicateDefinitionHashV1(original_definition),
            original_definition,
        },
        .witnesses = {{
            .witness_ordinal = 0,
            .source_kind = PredicateWitnessSourceKindV1::ConcreteValue,
            .value_type = TypeRef::Builtin(BuiltinType::U32),
            .concrete_value = LiteralValue{
                .type = TypeRef::Builtin(BuiltinType::U32),
                .payload = std::uint32_t{42},
            },
        }},
    };
    auto different_binding_identity = binding;
    different_binding_identity.execution_binding_revision_id = 999;
    different_binding_identity.canonical_id =
        "predicate.binding/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    different_binding_identity.revision = 12;
    different_binding_identity.content_sha256 = std::string(64, 'f');
    different_binding_identity.definition.revision_id = 777;
    EXPECT_EQ(ComputePredicateExecutionBindingSemanticHashV1(binding),
              ComputePredicateExecutionBindingSemanticHashV1(
                  different_binding_identity));

    auto changed_binding = different_binding_identity;
    changed_binding.witnesses.front().concrete_value->payload = std::uint32_t{43};
    EXPECT_NE(ComputePredicateExecutionBindingSemanticHashV1(binding),
              ComputePredicateExecutionBindingSemanticHashV1(changed_binding));

    ResolvedPredicateGroupV1 group{
        .predicate_group_revision_id = 30,
        .canonical_id = "predicate.group/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        .revision = 3,
        .members = {{
            .ordinal = 0,
            .execution_binding_revision_id = 10,
            .semantic_hook_ids = {
                "soa.battle.point.TurnIsReady",
            },
            .occurrence = PredicateOccurrencePolicyV1::First,
            .reaction = PredicateReaction::RecordAndContinue,
            .participates_in_aggregation = true,
            .emit_evidence = true,
        }},
    };
    auto different_group_identity = group;
    different_group_identity.predicate_group_revision_id = 888;
    different_group_identity.canonical_id =
        "predicate.group/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    different_group_identity.revision = 8;
    different_group_identity.content_sha256 = std::string(64, 'e');
    EXPECT_EQ(ComputeResolvedPredicateGroupSemanticHashV1(group),
              ComputeResolvedPredicateGroupSemanticHashV1(
                  different_group_identity));

    auto changed_group = different_group_identity;
    changed_group.members.front().emit_evidence = false;
    EXPECT_NE(ComputeResolvedPredicateGroupSemanticHashV1(group),
              ComputeResolvedPredicateGroupSemanticHashV1(changed_group));
}

PredicateGuidedNodeV1 CatalogValue(
    std::string recipe,
    std::vector<PredicateGuidedNodeV1> arguments = {})
{
    return {
        .kind = PredicateGuidedNodeKindV1::CatalogValue,
        .key = std::move(recipe),
        .children = std::move(arguments),
    };
}

PredicateGuidedNodeV1 Parameter(std::string name, BuiltinType type)
{
    return {
        .kind = PredicateGuidedNodeKindV1::Parameter,
        .key = std::move(name),
        .declared_type = TypeRef::Builtin(type),
    };
}

PredicateGuidedNodeV1 U32Literal(std::uint32_t value)
{
    return {
        .kind = PredicateGuidedNodeKindV1::Literal,
        .declared_type = TypeRef::Builtin(BuiltinType::U32),
        .literal = LiteralValue{
            .type = TypeRef::Builtin(BuiltinType::U32),
            .payload = value,
        },
    };
}

TEST(PredicateGuidedAuthoring, CatalogV2CarriesFriendlyBattleLanguage)
{
    const auto catalog = BattlePredicateAuthoringCatalogV2();
    EXPECT_EQ(catalog.revision, 4u);
    EXPECT_EQ(catalog.content_sha256,
              ComputePredicateAuthoringCatalogHashV2(catalog));
    EXPECT_NE(std::ranges::find(catalog.semantic_inputs,
                  std::string("battle.turn_order"),
                  &PredicateAuthoringSemanticInputV2::role_id),
              catalog.semantic_inputs.end());
    EXPECT_NE(std::ranges::find(catalog.semantic_inputs,
                  std::string("battle.rewards"),
                  &PredicateAuthoringSemanticInputV2::role_id),
              catalog.semantic_inputs.end());
    EXPECT_NE(std::ranges::find(catalog.value_recipes,
                  std::string("battle.turn_order.player_max_position"),
                  &PredicateAuthoringValueRecipeV2::recipe_id),
              catalog.value_recipes.end());
    EXPECT_NE(std::ranges::find(catalog.value_recipes,
                  std::string("battle.rewards.drop_count"),
                  &PredicateAuthoringValueRecipeV2::recipe_id),
              catalog.value_recipes.end());
    EXPECT_EQ(std::ranges::count(catalog.value_recipes,
                  std::string("Current battle turn"),
                  &PredicateAuthoringValueRecipeV2::display_name),
              1u);
    ASSERT_EQ(catalog.comparison_operators.size(), 6u);
    ASSERT_EQ(catalog.calculation_operators.size(), 5u);
    EXPECT_EQ(std::ranges::find(catalog.comparison_operators,
                  std::string("less"),
                  &PredicateAuthoringOperatorV2::operator_id)->display_name,
              "Is before / less than");
}

TEST(PredicateGuidedAuthoring, CompilesFirstBattleTurnOrderRuleDeterministically)
{
    const auto catalog = BattlePredicateAuthoringCatalogV2();
    const PredicateGuidedNodeV1 rule{
        .kind = PredicateGuidedNodeKindV1::Less,
        .children = {
            CatalogValue("battle.turn_order.player_max_position"),
            CatalogValue("battle.turn_order.enemy_min_position"),
        },
    };

    const auto first = CompileGuidedPredicateDefinitionV1(rule, catalog);
    const auto second = CompileGuidedPredicateDefinitionV1(rule, catalog);
    ASSERT_TRUE(first) << first.diagnostics.front().message;
    ASSERT_TRUE(second) << second.diagnostics.front().message;
    EXPECT_EQ(ComputePredicateDefinitionHashV1(first.definition),
              ComputePredicateDefinitionHashV1(second.definition));
    ASSERT_EQ(first.definition.witnesses.size(), 1u);
    EXPECT_EQ(first.definition.witnesses.front().name, "turn_order");
    ASSERT_EQ(first.definition.expression.size(), 4u);
    EXPECT_EQ(first.definition.expression[1].kind,
              PredicateExpressionKind::ImportedReducer);
    EXPECT_EQ(first.definition.expression[1].reducer->canonical_id,
              "soa.battle.derived.player_max_position");
    EXPECT_EQ(first.definition.expression[2].reducer->canonical_id,
              "soa.battle.derived.enemy_min_position");
    EXPECT_EQ(first.definition.expression[3].kind,
              PredicateExpressionKind::Less);
    EXPECT_EQ(first.definition.root_expression, 3u);

    const auto reconstructed =
        ReconstructGuidedPredicateDefinitionV1(first.definition, catalog);
    ASSERT_TRUE(reconstructed) << reconstructed.diagnostics.front().message;
    const auto round_trip =
        CompileGuidedPredicateDefinitionV1(reconstructed.root, catalog);
    ASSERT_TRUE(round_trip) << round_trip.diagnostics.front().message;
    EXPECT_EQ(ComputePredicateDefinitionSemanticHashV1(first.definition),
              ComputePredicateDefinitionSemanticHashV1(round_trip.definition));
}

TEST(PredicateGuidedAuthoring, CompilesFirstBattleCumulativeDropRule)
{
    const auto catalog = BattlePredicateAuthoringCatalogV2();
    const PredicateGuidedNodeV1 rule{
        .kind = PredicateGuidedNodeKindV1::GreaterEqual,
        .children = {
            CatalogValue("battle.rewards.drop_count",
                {Parameter("item_id", BuiltinType::U16)}),
            CatalogValue("battle.current_turn"),
        },
    };

    const auto compiled = CompileGuidedPredicateDefinitionV1(rule, catalog);
    ASSERT_TRUE(compiled) << compiled.diagnostics.front().message;
    ASSERT_EQ(compiled.definition.witnesses.size(), 3u);
    EXPECT_EQ(compiled.definition.witnesses[0].name, "rewards");
    EXPECT_EQ(compiled.definition.witnesses[1].name, "item_id");
    EXPECT_EQ(compiled.definition.witnesses[1].value_type,
              TypeRef::Builtin(BuiltinType::U16));
    EXPECT_EQ(compiled.definition.witnesses[2].name, "turn_inputs");
    ASSERT_EQ(compiled.definition.expression.size(), 6u);
    EXPECT_EQ(compiled.definition.expression[2].reducer->canonical_id,
              "soa.battle.derived.drop_count");
    EXPECT_EQ(compiled.definition.expression[4].reducer->canonical_id,
              "soa.battle.derived.current_turn");
    EXPECT_EQ(compiled.definition.expression[5].kind,
              PredicateExpressionKind::GreaterEqual);
}

TEST(PredicateGuidedAuthoring, PlansSemanticBattleStateWithoutUserSelectedHooks)
{
    const auto catalog = BattlePredicateAuthoringCatalogV2();
    const PredicateWitness witness{
        "turn_inputs",
        TypeRef::Named(
            capabilities::BattleDerivedSnapshotSchemaIdentity()),
    };
    const auto source = PlanPredicateSemanticWitnessSourceV1(
        witness, 0, catalog);
    ASSERT_TRUE(source);
    EXPECT_EQ(source->source_kind,
              PredicateWitnessSourceKindV1::DerivedStateQuery);
    EXPECT_EQ(source->source,
              capabilities::BattleDerivedTurnEntryActionIdentity());

    const auto query = std::ranges::find(
        catalog.query_sources, *source->source,
        &PredicateAuthoringSourceV1::identity);
    ASSERT_NE(query, catalog.query_sources.end());
    ASSERT_EQ(query->capture_hook_ids.size(), 1u);
    EXPECT_EQ(query->capture_hook_ids.front(),
              bp::BpRegistry::FindRuntime(bp::battle::TurnInputs)->stable_id);
    EXPECT_NE(std::ranges::find(
                  query->evaluation_hook_ids,
                  std::string(bp::BpRegistry::FindRuntime(
                      bp::battle::EndBattleVictory)->stable_id)),
              query->evaluation_hook_ids.end());
}

TEST(PredicateGuidedAuthoring, SupportsNestedBooleanAndArithmeticRules)
{
    const auto catalog = BattlePredicateAuthoringCatalogV2();
    const PredicateGuidedNodeV1 arithmetic{
        .kind = PredicateGuidedNodeKindV1::Greater,
        .children = {
            PredicateGuidedNodeV1{
                .kind = PredicateGuidedNodeKindV1::Add,
                .children = {U32Literal(2), U32Literal(3)},
            },
            U32Literal(4),
        },
    };
    const PredicateGuidedNodeV1 rule{
        .kind = PredicateGuidedNodeKindV1::All,
        .children = {
            arithmetic,
            PredicateGuidedNodeV1{
                .kind = PredicateGuidedNodeKindV1::Not,
                .children = {PredicateGuidedNodeV1{
                    .kind = PredicateGuidedNodeKindV1::Equal,
                    .children = {U32Literal(1), U32Literal(0)},
                }},
            },
        },
    };

    const auto compiled = CompileGuidedPredicateDefinitionV1(rule, catalog);
    ASSERT_TRUE(compiled) << compiled.diagnostics.front().message;
    EXPECT_EQ(compiled.definition.expression[compiled.definition.root_expression].kind,
              PredicateExpressionKind::BooleanAnd);
}

TEST(PredicateGuidedAuthoring, ReportsAddressableInvalidRuleDiagnostics)
{
    const auto catalog = BattlePredicateAuthoringCatalogV2();
    auto unavailable = CatalogValue("battle.value.does_not_exist");
    auto result = CompileGuidedPredicateDefinitionV1(unavailable, catalog);
    ASSERT_FALSE(result);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.front().path, "root");
    EXPECT_EQ(result.diagnostics.front().code, "predicate.recipe_unavailable");

    result = CompileGuidedPredicateDefinitionV1(U32Literal(1), catalog);
    ASSERT_FALSE(result);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.front().code, "predicate.non_boolean_root");

    const PredicateGuidedNodeV1 invalid_arity{
        .kind = PredicateGuidedNodeKindV1::Any,
        .children = {PredicateGuidedNodeV1{
            .kind = PredicateGuidedNodeKindV1::Equal,
            .children = {U32Literal(1), U32Literal(1)},
        }},
    };
    result = CompileGuidedPredicateDefinitionV1(invalid_arity, catalog);
    ASSERT_FALSE(result);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.front().code, "predicate.invalid_arity");
}

} // namespace
