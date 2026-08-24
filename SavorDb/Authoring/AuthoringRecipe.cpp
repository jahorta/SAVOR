#include "AuthoringRecipe.h"

namespace savor::db::authoring {

RecipeRef<SeedProbeSpecTag> AuthoringRecipeBuilder::SeedProbe(
    SeedProbeSpecDefinition value)
{
    const auto symbol = value.symbol;
    recipe_.seed_probe_specs.push_back(std::move(value));
    return {symbol};
}
RecipeRef<PredicateDefinitionTag> AuthoringRecipeBuilder::PredicateDefinition(
    PredicateDefinitionDefinition value)
{
    const auto symbol = value.symbol;
    recipe_.predicate_definitions.push_back(std::move(value));
    return {symbol};
}
RecipeRef<PredicateBindingTag> AuthoringRecipeBuilder::PredicateBinding(
    PredicateBindingDefinition value)
{
    const auto symbol = value.symbol;
    recipe_.predicate_bindings.push_back(std::move(value));
    return {symbol};
}
RecipeRef<PredicateGroupTag> AuthoringRecipeBuilder::PredicateGroup(
    PredicateGroupDefinition value)
{
    const auto symbol = value.symbol;
    recipe_.predicate_groups.push_back(std::move(value));
    return {symbol};
}
RecipeRef<BattleActionPresetTag> AuthoringRecipeBuilder::ActionPreset(
    BattleActionPresetDefinition value)
{
    const auto symbol = value.symbol;
    recipe_.battle_action_presets.push_back(std::move(value));
    return {symbol};
}
RecipeRef<BattlePlanTag> AuthoringRecipeBuilder::BattlePlan(
    BattlePlanDefinition value)
{
    const auto symbol = value.symbol;
    recipe_.battle_plans.push_back(std::move(value));
    return {symbol};
}
RecipeRef<WorkflowGraphTag> AuthoringRecipeBuilder::Workflow(
    WorkflowGraphDefinition value)
{
    const auto symbol = value.symbol;
    recipe_.workflow_graphs.push_back(std::move(value));
    return {symbol};
}

} // namespace savor::db::authoring
