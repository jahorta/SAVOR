#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <variant>

#include "Authoring/AuthoringRecipe.h"

namespace {
namespace authoring = savor::db::authoring;
namespace catalog = savor::db::authoring::catalog;

TEST(AuthoringRecipe, PredicateLiteralBelongsToBindingNotDefinition)
{
    authoring::AuthoringRecipeBuilder builder("test.predicate-boundary");
    const auto parameter = catalog::predicate::expression::Parameter(
        "item_id", catalog::types::U16);
    const auto definition = builder.PredicateDefinition({
        .symbol = "definition", .name = "Item count",
        .rule = catalog::predicate::expression::GreaterEqual(
            catalog::predicate::expression::Value(
                catalog::predicate::values::battle::DropCount, {parameter}),
            catalog::predicate::expression::Value(
                catalog::predicate::values::battle::CurrentTurn)),
    });
    builder.PredicateBinding({
        .symbol = "binding", .name = "Electribox count",
        .definition = definition,
        .literals = {{
            .witness_name = "item_id",
            .value = savor::runtime::program::LiteralValue{
                .type = catalog::types::U16,
                .payload = static_cast<std::uint16_t>(273)},
        }},
    });
    const auto recipe = std::move(builder).Build();

    ASSERT_EQ(recipe.predicate_definitions.size(), 1u);
    ASSERT_EQ(recipe.predicate_bindings.size(), 1u);
    const auto& drop_count = recipe.predicate_definitions.front().rule.children.front();
    ASSERT_EQ(drop_count.children.size(), 1u);
    EXPECT_EQ(drop_count.children.front().kind,
        savor::runtime::predicates::PredicateGuidedNodeKindV1::Parameter);
    EXPECT_FALSE(drop_count.children.front().literal.has_value());
    ASSERT_EQ(recipe.predicate_bindings.front().literals.size(), 1u);
    EXPECT_EQ(std::get<std::uint16_t>(
        recipe.predicate_bindings.front().literals.front().value.payload), 273);
}

TEST(AuthoringRecipe, CatalogTokensRemainSemanticAndDiscoverable)
{
    EXPECT_EQ(catalog::predicate::values::battle::DropCount.recipe_id,
        "battle.rewards.drop_count");
    EXPECT_EQ(catalog::predicate::inputs::battle::Rewards.role_id,
        "battle.rewards");
    EXPECT_EQ(catalog::workflow::units::SeedProbe.unit_kind, "seed_probe");
    EXPECT_EQ(catalog::workflow::ports::seed_probe::Run.key,
        "seed_probe_run");
}

TEST(AuthoringRecipe, BattlePlanDescriptionIsMetadata)
{
    authoring::AuthoringRecipeBuilder builder("test.battle-plan-description");
    builder.BattlePlan({
        .symbol = "plan",
        .name = "Plan",
        .description = "Developer-facing plan description",
    });
    const auto recipe = std::move(builder).Build();

    ASSERT_EQ(recipe.battle_plans.size(), 1u);
    EXPECT_EQ(recipe.battle_plans.front().description,
        "Developer-facing plan description");
}
} // namespace
