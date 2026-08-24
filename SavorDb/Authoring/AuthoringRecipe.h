#pragma once

#include "AuthoringCatalog.h"
#include "IAuthoringDb.h"
#include "../Execution/Workflow/WorkflowComposition.h"

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace savor::db::authoring {

struct SeedProbeSpecTag {};
struct PredicateDefinitionTag {};
struct PredicateBindingTag {};
struct PredicateGroupTag {};
struct BattleActionPresetTag {};
struct BattlePlanTag {};
struct WorkflowGraphTag {};

template <typename Tag> struct RecipeRef {
    std::string symbol;
};
template <typename Tag> struct ExistingRef {
    std::int64_t id = 0;
};
template <typename Tag>
using AuthoringRef = std::variant<RecipeRef<Tag>, ExistingRef<Tag>>;

struct SeedProbeSpecDefinition {
    std::string symbol;
    std::string name;
    int priority = 0;
    std::int64_t min_value = -128;
    std::int64_t max_value = 127;
    bool cap_trigger_top = false;
    bool ignore_trigger_minmax = false;
    int combo_attempts_per_target = 3;
    int combo_sampler_tries = 32;
    bool auto_schedule_battle_run = false;
};

struct PredicateDefinitionDefinition {
    std::string symbol;
    std::string name;
    std::string description;
    runtime::predicates::PredicateGuidedNodeV1 rule;
    bool publish = true;
};

struct PredicateLiteralBinding {
    std::string witness_name;
    runtime::program::LiteralValue value;
};

struct PredicateBindingDefinition {
    std::string symbol;
    std::string name;
    std::string description;
    AuthoringRef<PredicateDefinitionTag> definition;
    bool use_recommended_sources = true;
    std::vector<PredicateLiteralBinding> literals;
    bool publish = true;
};

struct PredicateGroupMemberDefinition {
    AuthoringRef<PredicateBindingTag> binding;
    std::vector<catalog::PredicateHookToken> hooks;
    runtime::predicates::PredicateOccurrencePolicyV1 occurrence =
        runtime::predicates::PredicateOccurrencePolicyV1::Every;
    std::optional<std::uint32_t> occurrence_ordinal;
    std::optional<AuthoringRef<PredicateBindingTag>> guard_binding;
    runtime::program::composition::PredicateReaction reaction =
        runtime::program::composition::PredicateReaction::RecordAndContinue;
    bool participates_in_aggregation = true;
    bool emit_evidence = true;
};

struct PredicateGroupDefinition {
    std::string symbol;
    std::string name;
    std::string description;
    std::vector<PredicateGroupMemberDefinition> members;
    bool publish = true;
};

struct BattleActionPresetDefinition {
    std::string symbol;
    std::string name;
    BattlePlanActionMacro macro = BattlePlanActionMacro::Attack;
    BattlePlanTargetKind target_kind = BattlePlanTargetKind::SingleEnemy;
    std::optional<int> target_mask_bits;
    std::optional<int> target_single_slot;
    std::optional<int> target_same_as_actor_slot;
    std::optional<int> item_id;
    int flags = 0;
};

struct BattlePlanActionDefinition {
    int actor_slot = 0;
    AuthoringRef<BattleActionPresetTag> preset;
    int ordinal = 0;
};

struct BattlePlanTurnDefinition {
    int turn_index = 0;
    std::optional<AuthoringRef<PredicateGroupTag>> predicate_group;
    std::vector<BattlePlanActionDefinition> actions;
};

struct BattlePlanDefinition {
    std::string symbol;
    std::string name;
    std::string description;
    std::vector<BattlePlanTurnDefinition> turns;
};

enum class WorkflowAuthoredObjectKind {
    SeedProbeSpec,
    BattlePlan,
};

struct WorkflowAuthoredReferenceDefinition {
    WorkflowAuthoredObjectKind kind = WorkflowAuthoredObjectKind::SeedProbeSpec;
    std::variant<AuthoringRef<SeedProbeSpecTag>, AuthoringRef<BattlePlanTag>> ref;
};

struct WorkflowNodeDefinition {
    std::string node_key;
    catalog::WorkflowUnitToken unit;
    std::optional<std::string> display_name;
    std::optional<WorkflowAuthoredReferenceDefinition> authored_ref;
};

struct WorkflowExternalInputDefinition {
    std::string node_key;
    catalog::WorkflowPortToken input;
};

struct WorkflowEdgeDefinition {
    std::string from_node_key;
    catalog::WorkflowPortToken output;
    std::string to_node_key;
    catalog::WorkflowPortToken input;
    std::optional<std::string> guard_kind;
    std::optional<std::string> guard_value;
};

struct WorkflowGraphDefinition {
    std::string symbol;
    std::optional<std::int64_t> workflow_graph_id;
    std::optional<std::int64_t> parent_revision_id;
    std::string name;
    std::string description;
    bool hidden = false;
    int graph_version = 1;
    bool make_active = true;
    bool standalone_hash = false;
    std::vector<WorkflowNodeDefinition> nodes;
    std::vector<WorkflowExternalInputDefinition> external_inputs;
    std::vector<WorkflowEdgeDefinition> edges;
};

struct AuthoringRecipe {
    std::string key;
    std::vector<SeedProbeSpecDefinition> seed_probe_specs;
    std::vector<PredicateDefinitionDefinition> predicate_definitions;
    std::vector<PredicateBindingDefinition> predicate_bindings;
    std::vector<PredicateGroupDefinition> predicate_groups;
    std::vector<BattleActionPresetDefinition> battle_action_presets;
    std::vector<BattlePlanDefinition> battle_plans;
    std::vector<WorkflowGraphDefinition> workflow_graphs;
};

class AuthoringRecipeBuilder {
public:
    explicit AuthoringRecipeBuilder(std::string key) { recipe_.key = std::move(key); }

    RecipeRef<SeedProbeSpecTag> SeedProbe(SeedProbeSpecDefinition value);
    RecipeRef<PredicateDefinitionTag> PredicateDefinition(
        PredicateDefinitionDefinition value);
    RecipeRef<PredicateBindingTag> PredicateBinding(
        PredicateBindingDefinition value);
    RecipeRef<PredicateGroupTag> PredicateGroup(PredicateGroupDefinition value);
    RecipeRef<BattleActionPresetTag> ActionPreset(
        BattleActionPresetDefinition value);
    RecipeRef<BattlePlanTag> BattlePlan(BattlePlanDefinition value);
    RecipeRef<WorkflowGraphTag> Workflow(WorkflowGraphDefinition value);

    [[nodiscard]] AuthoringRecipe Build() && { return std::move(recipe_); }

private:
    AuthoringRecipe recipe_;
};

struct AuthoringRecipeResult {
    std::map<std::string, std::int64_t> seed_probe_spec_ids;
    std::map<std::string, std::int64_t> predicate_definition_revision_ids;
    std::map<std::string, std::int64_t> predicate_binding_revision_ids;
    std::map<std::string, std::int64_t> predicate_group_revision_ids;
    std::map<std::string, std::int64_t> battle_action_preset_ids;
    std::map<std::string, std::int64_t> battle_plan_ids;
    std::map<std::string, SaveWorkflowGraphResult> workflow_graphs;
};

} // namespace savor::db::authoring
