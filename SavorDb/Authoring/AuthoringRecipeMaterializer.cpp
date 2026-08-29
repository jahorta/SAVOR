#include "AuthoringRecipeMaterializer.h"

#include "AuthoringContentHash.h"

#include <algorithm>
#include <iomanip>
#include <ranges>
#include <set>
#include <sstream>

namespace savor::db::authoring {
namespace {

void SetError(std::string* out, std::string value)
{
    if (out != nullptr) *out = std::move(value);
}

template <typename Tag>
std::optional<std::int64_t> Resolve(
    const AuthoringRef<Tag>& ref,
    const std::map<std::string, std::int64_t>& symbols,
    std::string_view label,
    std::string* error_out)
{
    if (const auto* existing = std::get_if<ExistingRef<Tag>>(&ref)) {
        if (existing->id > 0) return existing->id;
        SetError(error_out, std::string(label) + " existing ID must be positive");
        return std::nullopt;
    }
    const auto& symbol = std::get<RecipeRef<Tag>>(ref).symbol;
    const auto found = symbols.find(symbol);
    if (found != symbols.end()) return found->second;
    SetError(error_out, std::string(label) + " symbol is unavailable: " + symbol);
    return std::nullopt;
}

bool ValidSymbols(const AuthoringRecipe& recipe, std::string* error_out)
{
    const auto validate = [&](const auto& values, std::string_view kind) {
        std::set<std::string> symbols;
        for (const auto& value : values) {
            if (value.symbol.empty()) {
                SetError(error_out, std::string(kind) + " symbol is required");
                return false;
            }
            if (!symbols.insert(value.symbol).second) {
                SetError(error_out, std::string(kind) + " symbol is duplicated: "
                    + value.symbol);
                return false;
            }
        }
        return true;
    };
    return !recipe.key.empty()
        && validate(recipe.seed_probe_specs, "SeedProbe spec")
        && validate(recipe.predicate_definitions, "Predicate Definition")
        && validate(recipe.predicate_bindings, "Predicate Binding")
        && validate(recipe.predicate_groups, "Predicate Group")
        && validate(recipe.battle_action_presets, "Battle action preset")
        && validate(recipe.battle_plans, "Battle Plan")
        && validate(recipe.workflow_graphs, "workflow graph");
}

std::string RequestKey(
    const AuthoringRecipe& recipe,
    std::string_view kind,
    std::string_view symbol)
{
    const auto content = recipe.key + "." + std::string(kind)
        + "." + std::string(symbol);
    const auto hash = [&](const std::uint64_t basis) {
        std::uint64_t value = basis;
        for (const auto ch : content) {
            value ^= static_cast<unsigned char>(ch);
            value *= 1099511628211ull;
        }
        return value;
    };
    std::ostringstream out;
    out << std::hex << std::setfill('0')
        << std::setw(16) << hash(1469598103934665603ull)
        << std::setw(16) << hash(1099511628211ull);
    return out.str();
}

std::string ArgumentType(
    const execution::workflow::WorkflowLaunchArgumentValueType type)
{
    using Type = execution::workflow::WorkflowLaunchArgumentValueType;
    switch (type) {
    case Type::Integer: return "integer";
    case Type::Text: return "text";
    case Type::Boolean: return "boolean";
    case Type::Json: return "json";
    case Type::Choice: return "choice";
    }
    return {};
}

std::optional<std::int64_t> ResolveWorkflowReference(
    const WorkflowAuthoredReferenceDefinition& authored,
    const AuthoringRecipeResult& resolved,
    std::string* error_out)
{
    if (authored.kind == WorkflowAuthoredObjectKind::SeedProbeSpec) {
        const auto* ref = std::get_if<AuthoringRef<SeedProbeSpecTag>>(
            &authored.ref);
        if (ref == nullptr) {
            SetError(error_out, "workflow authored reference kind does not match SeedProbe");
            return std::nullopt;
        }
        return Resolve(*ref, resolved.seed_probe_spec_ids,
            "workflow SeedProbe reference", error_out);
    }
    const auto* ref = std::get_if<AuthoringRef<BattlePlanTag>>(&authored.ref);
    if (ref == nullptr) {
        SetError(error_out, "workflow authored reference kind does not match Battle Plan");
        return std::nullopt;
    }
    return Resolve(*ref, resolved.battle_plan_ids,
        "workflow Battle Plan reference", error_out);
}

} // namespace

bool AuthoringRecipeMaterializer::SaveSeedProbeSpec(
    const SeedProbeSpecDefinition& definition,
    std::int64_t* id_out,
    std::string* error_out) const
{
    if (db_ == nullptr || id_out == nullptr || definition.name.empty()
        || definition.min_value > definition.max_value
        || definition.combo_attempts_per_target <= 0
        || definition.combo_sampler_tries <= 0) {
        SetError(error_out, "invalid SeedProbe authoring definition");
        return false;
    }
    const auto now = types::UtcNow();
    return db_->SaveSeedProbeSpec({
        .name = definition.name,
        .priority = definition.priority,
        .min_value = definition.min_value,
        .max_value = definition.max_value,
        .cap_trigger_top = definition.cap_trigger_top,
        .ignore_trigger_minmax = definition.ignore_trigger_minmax,
        .combo_attempts_per_target = definition.combo_attempts_per_target,
        .combo_sampler_tries = definition.combo_sampler_tries,
        .auto_schedule_battle_run = definition.auto_schedule_battle_run,
        .created_at_utc = now,
        .correlation_id = "authoring.recipe.seed_probe",
        .causation_id = "authoring.recipe",
    }, id_out, error_out);
}

bool AuthoringRecipeMaterializer::SaveWorkflowGraph(
    const WorkflowGraphDefinition& definition,
    const AuthoringRecipeResult& resolved,
    SaveWorkflowGraphResult* result_out,
    std::string* error_out) const
{
    if (db_ == nullptr || result_out == nullptr || definition.name.empty()
        || definition.nodes.empty()) {
        SetError(error_out, "invalid workflow graph authoring definition");
        return false;
    }
    const auto registry = execution::workflow::BuildDefaultWorkflowUnitRegistry();
    execution::workflow::WorkflowCompositionSpec composition{};
    SaveWorkflowGraphCommand command{
        .workflow_graph_id = definition.workflow_graph_id,
        .parent_revision_id = definition.parent_revision_id,
        .name = definition.name,
        .description = definition.description,
        .hidden = definition.hidden,
        .graph_version = definition.graph_version,
        .execution_shape = definition.execution_shape,
        .expansion_kind = definition.expansion_kind,
        .make_active = definition.make_active,
        .created_at_utc = types::UtcNow(),
        .correlation_id = "authoring.recipe.workflow",
        .causation_id = "authoring.recipe",
    };
    std::set<std::string> node_keys;
    for (const auto& node : definition.nodes) {
        if (node.node_key.empty() || !node_keys.insert(node.node_key).second) {
            SetError(error_out, "workflow node keys must be nonempty and unique");
            return false;
        }
        const auto* unit = registry.Find(node.unit.unit_kind);
        if (unit == nullptr) {
            SetError(error_out, "workflow unit is unavailable: "
                + std::string(node.unit.unit_kind));
            return false;
        }
        SaveWorkflowGraphNodeCommand saved{
            .node_key = node.node_key,
            .unit_kind = unit->unit_kind,
            .display_name = node.display_name.value_or(unit->display_name),
        };
        if (node.authored_ref.has_value()) {
            if (unit->authored_refs.size() != 1) {
                SetError(error_out, "workflow authored reference requires exactly one unit contract reference");
                return false;
            }
            const auto id = ResolveWorkflowReference(
                *node.authored_ref, resolved, error_out);
            if (!id.has_value()) return false;
            saved.authored_ref_kind = unit->authored_refs.front().ref_kind;
            saved.authored_ref_id = *id;
        } else if (std::ranges::any_of(
                       unit->authored_refs, &execution::workflow::WorkflowAuthoredRefRequirement::required)) {
            SetError(error_out, "workflow unit requires an authored reference: " + unit->unit_kind);
            return false;
        }
        for (const auto& input : unit->required_inputs) {
            saved.inputs.push_back({input.key, input.data_kind, input.ref_kind,
                                    input.display_name, input.required});
        }
        for (const auto& output : unit->possible_outputs) {
            saved.possible_outputs.push_back({output.key, output.data_kind,
                                              output.ref_kind, output.display_name});
        }
        for (const auto& argument : unit->launch_arguments) {
            SaveWorkflowGraphNodeArgumentCommand saved_argument{
                .argument_key = argument.key,
                .display_name = argument.display_name,
                .value_type = ArgumentType(argument.value_type),
                .required = argument.required,
                .default_value = argument.default_value,
                .minimum_integer = argument.minimum_integer,
                .maximum_integer = argument.maximum_integer,
            };
            for (const auto& choice : argument.choices) {
                saved_argument.choices.push_back({choice.value, choice.display_name});
            }
            const auto constant = std::ranges::find(
                node.constant_arguments, argument.key,
                &WorkflowNodeDefinition::ConstantArgument::argument_key);
            if (constant != node.constant_arguments.end()) {
                saved_argument.binding_mode =
                    SaveWorkflowGraphNodeArgumentCommand::BindingMode::Constant;
                saved_argument.constant_value = constant->canonical_value;
            }
            saved.arguments.push_back(std::move(saved_argument));
        }
        for (const auto& constant : node.constant_arguments) {
            if (std::ranges::count(node.constant_arguments, constant.argument_key,
                    &WorkflowNodeDefinition::ConstantArgument::argument_key) != 1
                || std::ranges::none_of(unit->launch_arguments,
                    [&](const auto& argument) {
                        return argument.key == constant.argument_key;
                    })) {
                SetError(error_out, "workflow constant references an unknown or duplicate argument: "
                    + node.node_key + "." + constant.argument_key);
                return false;
            }
        }
        for (const auto& constraint : unit->launch_argument_constraints) {
            saved.argument_constraints.push_back({
                constraint.lesser_or_equal_key,
                constraint.greater_or_equal_key,
                constraint.message});
        }
        command.nodes.push_back(std::move(saved));
        composition.nodes.push_back({node.node_key, unit->unit_kind});
    }
    for (const auto& external : definition.external_inputs) {
        const auto node = std::ranges::find(
            definition.nodes, external.node_key, &WorkflowNodeDefinition::node_key);
        if (node == definition.nodes.end()) {
            SetError(error_out, "workflow external input references an unknown node");
            return false;
        }
        const auto* unit = registry.Find(node->unit.unit_kind);
        const auto input = std::ranges::find(
            unit->required_inputs, external.input.key,
            &execution::workflow::WorkflowPortDefinition::key);
        if (input == unit->required_inputs.end()) {
            SetError(error_out, "workflow external input is unavailable on the selected unit");
            return false;
        }
        composition.external_inputs.push_back({
            external.node_key, input->key, input->data_kind, input->ref_kind,
            std::nullopt});
    }
    for (const auto& edge : definition.edges) {
        command.edges.push_back({
            edge.from_node_key, std::string(edge.output.key), edge.to_node_key,
            std::string(edge.input.key), "DATA", edge.guard_kind, edge.guard_value});
        composition.output_bindings.push_back({
            edge.from_node_key, std::string(edge.output.key), edge.to_node_key,
            std::string(edge.input.key), edge.guard_kind, edge.guard_value});
    }
    for (const auto& dependency : definition.control_dependencies) {
        if (!node_keys.contains(dependency.from_node_key)
            || !node_keys.contains(dependency.to_node_key)
            || dependency.from_node_key == dependency.to_node_key) {
            SetError(error_out, "workflow control dependency references an invalid node");
            return false;
        }
        command.edges.push_back({
            dependency.from_node_key, {}, dependency.to_node_key, {}, "CONTROL",
            dependency.guard_kind, dependency.guard_value});
    }
    const execution::workflow::WorkflowCompositionService composer(&registry);
    const auto preview = composer.Preview(composition);
    if (!preview.valid) {
        SetError(error_out, preview.issues.empty()
            ? "workflow composition is invalid"
            : preview.issues.front().message);
        return false;
    }
    std::vector<WorkflowGraphHashNode> hash_nodes;
    std::vector<WorkflowGraphHashArgument> hash_arguments;
    std::vector<WorkflowGraphHashEdge> hash_edges;
    for (const auto& node : command.nodes) {
        hash_nodes.push_back({node.node_key, node.unit_kind});
        for (const auto& argument : node.arguments) {
            if (argument.binding_mode ==
                SaveWorkflowGraphNodeArgumentCommand::BindingMode::Constant) {
                hash_arguments.push_back({node.node_key, argument.argument_key,
                    "CONSTANT", argument.constant_value.value_or("")});
            }
        }
    }
    for (const auto& edge : command.edges) {
        hash_edges.push_back({edge.from_node_key, edge.output_key,
                              edge.to_node_key, edge.input_key, edge.edge_kind});
    }
    if (definition.standalone_hash) {
        if (command.nodes.size() != 1) {
            SetError(error_out, "standalone workflow graph must contain one unit");
            return false;
        }
        command.graph_hash = ComputeStandaloneWorkflowGraphHash(
            command.nodes.front().unit_kind,
            command.nodes.front().authored_ref_kind,
            command.nodes.front().authored_ref_id);
    } else {
        command.graph_hash = ComputeWorkflowGraphHash(
            command.name, command.description, command.execution_shape,
            command.expansion_kind, hash_nodes, hash_arguments, hash_edges);
    }
    if (!db_->SaveWorkflowGraph(command, result_out, error_out)) {
        if (error_out) {
            *error_out = "workflow graph persistence failed for '" + command.name
                + "': " + *error_out;
        }
        return false;
    }
    const auto snapshot = db_->GetWorkflowGraph(result_out->workflow_graph_id);
    if (!snapshot.has_value()
        || snapshot->workflow_graph_revision_id
            != result_out->workflow_graph_revision_id
        || snapshot->graph_hash != command.graph_hash) {
        SetError(error_out, "workflow graph verification failed: " + command.name);
        return false;
    }
    return true;
}

bool AuthoringRecipeMaterializer::Apply(
    const AuthoringRecipe& recipe,
    AuthoringRecipeResult* result_out,
    std::string* error_out) const
{
    if (db_ == nullptr || result_out == nullptr || !ValidSymbols(recipe, error_out)) {
        if (db_ == nullptr) SetError(error_out, "authoring database is unavailable");
        else if (result_out == nullptr) SetError(error_out, "authoring result is required");
        else if (recipe.key.empty()) SetError(error_out, "authoring recipe key is required");
        return false;
    }
    AuthoringRecipeResult result{};
    for (const auto& spec : recipe.seed_probe_specs) {
        std::int64_t id = 0;
        if (!SaveSeedProbeSpec(spec, &id, error_out)) return false;
        const auto saved = db_->GetSeedProbeSpec(id);
        if (!saved.has_value() || saved->name != spec.name) {
            SetError(error_out, "SeedProbe spec verification failed: " + spec.symbol);
            return false;
        }
        result.seed_probe_spec_ids.emplace(spec.symbol, id);
    }
    const auto predicate_catalog = runtime::predicates::BattlePredicateAuthoringCatalogV2();
    for (const auto& definition : recipe.predicate_definitions) {
        const auto compiled = runtime::predicates::CompileGuidedPredicateDefinitionV1(
            definition.rule, predicate_catalog);
        if (!compiled) {
            SetError(error_out, compiled.diagnostics.empty()
                ? "Predicate Definition compilation failed"
                : compiled.diagnostics.front().message);
            return false;
        }
        PredicateAuthoringRevisionReceipt receipt{};
        if (!db_->CreatePredicateDefinitionDraft({
                .creation_request_key = RequestKey(
                    recipe, "predicate-definition", definition.symbol),
                .name = definition.name,
                .description = definition.description,
                .body = {compiled.definition.witnesses,
                         compiled.definition.expression,
                         compiled.definition.root_expression},
                .created_at_utc = types::UtcNow(),
            }, &receipt, error_out)) return false;
        if (definition.publish
            && !db_->PublishPredicateDefinitionRevisionV2(
                receipt.revision_id, types::UtcNow(), &receipt, error_out)) {
            return false;
        }
        result.predicate_definition_revision_ids.emplace(
            definition.symbol, receipt.revision_id);
    }
    for (const auto& binding : recipe.predicate_bindings) {
        const auto definition_id = Resolve(
            binding.definition, result.predicate_definition_revision_ids,
            "Predicate Definition", error_out);
        if (!definition_id.has_value()) return false;
        const auto definition = db_->GetPredicateDefinitionRevisionV2(*definition_id);
        if (!definition.has_value()) {
            SetError(error_out, "Predicate Definition snapshot is unavailable");
            return false;
        }
        std::vector<runtime::predicates::PredicateWitnessSourceBindingV1> sources;
        std::set<std::string> consumed_literals;
        for (std::size_t ordinal = 0;
             ordinal < definition->definition.witnesses.size(); ++ordinal) {
            const auto& witness = definition->definition.witnesses[ordinal];
            const auto literal = std::ranges::find(
                binding.literals, witness.name,
                &PredicateLiteralBinding::witness_name);
            if (literal != binding.literals.end()) {
                if (literal->value.type != witness.value_type
                    || !consumed_literals.insert(witness.name).second) {
                    SetError(error_out, "Predicate literal binding type or identity mismatch: "
                        + witness.name);
                    return false;
                }
                sources.push_back({
                    .witness_ordinal = static_cast<std::uint32_t>(ordinal),
                    .source_kind = runtime::predicates::PredicateWitnessSourceKindV1::ConcreteValue,
                    .value_type = witness.value_type,
                    .concrete_value = literal->value,
                });
                continue;
            }
            if (binding.use_recommended_sources) {
                auto planned = runtime::predicates::PlanPredicateSemanticWitnessSourceV1(
                    witness, static_cast<std::uint32_t>(ordinal), predicate_catalog);
                if (planned.has_value()) {
                    sources.push_back(std::move(*planned));
                    continue;
                }
            }
            SetError(error_out, "Predicate witness has no binding: " + witness.name);
            return false;
        }
        if (consumed_literals.size() != binding.literals.size()) {
            SetError(error_out, "Predicate Binding contains an unknown or duplicate literal witness");
            return false;
        }
        PredicateAuthoringRevisionReceipt receipt{};
        if (!db_->CreatePredicateExecutionBindingDraft({
                .creation_request_key = RequestKey(
                    recipe, "predicate-binding", binding.symbol),
                .name = binding.name,
                .description = binding.description,
                .body = {*definition_id, std::move(sources)},
                .created_at_utc = types::UtcNow(),
            }, &receipt, error_out)) return false;
        if (binding.publish
            && !db_->PublishPredicateExecutionBindingRevision(
                receipt.revision_id, types::UtcNow(), &receipt, error_out)) {
            return false;
        }
        result.predicate_binding_revision_ids.emplace(
            binding.symbol, receipt.revision_id);
    }
    for (const auto& group : recipe.predicate_groups) {
        PredicateGroupDraftBody body{};
        for (std::size_t ordinal = 0; ordinal < group.members.size(); ++ordinal) {
            const auto& member = group.members[ordinal];
            const auto binding_id = Resolve(
                member.binding, result.predicate_binding_revision_ids,
                "Predicate Binding", error_out);
            if (!binding_id.has_value()) return false;
            runtime::predicates::PredicateGroupMemberV1 saved{
                .ordinal = static_cast<std::uint32_t>(ordinal),
                .execution_binding_revision_id = *binding_id,
                .occurrence = member.occurrence,
                .occurrence_ordinal = member.occurrence_ordinal,
                .reaction = member.reaction,
                .participates_in_aggregation = member.participates_in_aggregation,
                .emit_evidence = member.emit_evidence,
            };
            for (const auto hook : member.hooks) {
                saved.semantic_hook_ids.emplace_back(hook.canonical_id);
            }
            std::ranges::sort(saved.semantic_hook_ids);
            if (member.guard_binding.has_value()) {
                const auto guard = Resolve(
                    *member.guard_binding, result.predicate_binding_revision_ids,
                    "Predicate guard Binding", error_out);
                if (!guard.has_value()) return false;
                saved.guard_execution_binding_revision_id = *guard;
            }
            body.members.push_back(std::move(saved));
        }
        PredicateAuthoringRevisionReceipt receipt{};
        if (!db_->CreatePredicateGroupDraft({
                .creation_request_key = RequestKey(
                    recipe, "predicate-group", group.symbol),
                .name = group.name,
                .description = group.description,
                .body = std::move(body),
                .created_at_utc = types::UtcNow(),
            }, &receipt, error_out)) return false;
        if (group.publish
            && !db_->PublishPredicateGroupRevision(
                receipt.revision_id, types::UtcNow(), &receipt, error_out)) {
            return false;
        }
        result.predicate_group_revision_ids.emplace(group.symbol, receipt.revision_id);
    }
    for (const auto& preset : recipe.battle_action_presets) {
        std::int64_t id = 0;
        if (!db_->SaveBattlePlanActionPreset({
                .name = preset.name,
                .macro = preset.macro,
                .target_kind = preset.target_kind,
                .target_mask_bits = preset.target_mask_bits,
                .target_single_slot = preset.target_single_slot,
                .target_same_as_actor_slot = preset.target_same_as_actor_slot,
                .item_id = preset.item_id,
                .flags = preset.flags,
                .created_at_utc = types::UtcNow(),
                .correlation_id = RequestKey(recipe, "action-preset", preset.symbol),
                .causation_id = "authoring-recipe." + recipe.key,
            }, &id, error_out)) return false;
        result.battle_action_preset_ids.emplace(preset.symbol, id);
    }
    for (const auto& plan : recipe.battle_plans) {
        std::vector<std::vector<BattlePlanFingerprintAction>> fingerprint_actions;
        std::vector<BattlePlanFingerprintTurn> fingerprint_turns;
        fingerprint_actions.reserve(plan.turns.size());
        fingerprint_turns.reserve(plan.turns.size());
        std::vector<std::optional<std::int64_t>> group_ids;
        group_ids.reserve(plan.turns.size());
        for (const auto& turn : plan.turns) {
            std::optional<std::int64_t> group_id;
            if (turn.predicate_group.has_value()) {
                group_id = Resolve(*turn.predicate_group,
                    result.predicate_group_revision_ids,
                    "Battle Plan Predicate Group", error_out);
                if (!group_id.has_value()) return false;
            }
            group_ids.push_back(group_id);
            auto& actions = fingerprint_actions.emplace_back();
            for (const auto& action : turn.actions) {
                const auto preset_id = Resolve(action.preset,
                    result.battle_action_preset_ids,
                    "Battle Plan action preset", error_out);
                if (!preset_id.has_value()) return false;
                actions.push_back({action.actor_slot, *preset_id, action.ordinal});
            }
            fingerprint_turns.push_back({turn.turn_index, group_id, actions});
        }
        std::int64_t plan_id = 0;
        if (!db_->SavePlan({
                .name = plan.name,
                .description = plan.description,
                .fingerprint = ComputeBattlePlanFingerprint(
                    plan.name, fingerprint_turns),
                .created_at_utc = types::UtcNow(),
                .correlation_id = RequestKey(recipe, "battle-plan", plan.symbol),
                .causation_id = "authoring-recipe." + recipe.key,
            }, &plan_id, error_out)) return false;
        for (std::size_t i = 0; i < plan.turns.size(); ++i) {
            SaveBattlePlanTurnCommand command{
                .plan_id = plan_id,
                .turn_index = plan.turns[i].turn_index,
                .default_predicate_group_revision_id = group_ids[i],
                .created_at_utc = types::UtcNow(),
                .correlation_id = RequestKey(recipe, "battle-turn", plan.symbol),
                .causation_id = "authoring-recipe." + recipe.key,
            };
            for (const auto& action : fingerprint_actions[i]) {
                command.actions.push_back({
                    action.actor_slot, action.action_preset_id, action.ordinal});
            }
            std::int64_t turn_id = 0;
            if (!db_->SaveBattlePlanTurn(command, &turn_id, error_out)) return false;
        }
        result.battle_plan_ids.emplace(plan.symbol, plan_id);
    }
    for (const auto& graph : recipe.workflow_graphs) {
        SaveWorkflowGraphResult saved{};
        if (!SaveWorkflowGraph(graph, result, &saved, error_out)) return false;
        result.workflow_graphs.emplace(graph.symbol, saved);
    }
    *result_out = std::move(result);
    return true;
}

bool MaterializeWorkflowGraph(
    IAuthoringDb* db,
    const SaveWorkflowGraphCommand& draft,
    SaveWorkflowGraphResult* result_out,
    std::string* error_out)
{
    WorkflowGraphDefinition definition{
        .symbol = "single.workflow",
        .workflow_graph_id = draft.workflow_graph_id,
        .parent_revision_id = draft.parent_revision_id,
        .name = draft.name,
        .description = draft.description,
        .hidden = draft.hidden.value_or(false),
        .graph_version = draft.graph_version,
        .make_active = draft.make_active,
        .standalone_hash = draft.graph_hash.starts_with("standalone-"),
        .execution_shape = draft.execution_shape,
        .expansion_kind = draft.expansion_kind,
    };
    for (const auto& node : draft.nodes) {
        WorkflowNodeDefinition saved{
            .node_key = node.node_key,
            .unit = {node.unit_kind},
            .display_name = node.display_name,
        };
        for (const auto& argument : node.arguments) {
            if (argument.binding_mode ==
                    SaveWorkflowGraphNodeArgumentCommand::BindingMode::Constant
                && argument.constant_value) {
                saved.constant_arguments.push_back({
                    argument.argument_key, *argument.constant_value});
            }
        }
        if (node.authored_ref_kind.has_value()
            && node.authored_ref_id.has_value()) {
            if (*node.authored_ref_kind == "seed_probe_spec") {
                saved.authored_ref = WorkflowAuthoredReferenceDefinition{
                    .kind = WorkflowAuthoredObjectKind::SeedProbeSpec,
                    .ref = AuthoringRef<SeedProbeSpecTag>{
                        ExistingRef<SeedProbeSpecTag>{*node.authored_ref_id}},
                };
            } else if (*node.authored_ref_kind == "authoring.battle_plan") {
                saved.authored_ref = WorkflowAuthoredReferenceDefinition{
                    .kind = WorkflowAuthoredObjectKind::BattlePlan,
                    .ref = AuthoringRef<BattlePlanTag>{
                        ExistingRef<BattlePlanTag>{*node.authored_ref_id}},
                };
            } else {
                SetError(error_out, "unsupported workflow authored reference kind: "
                    + *node.authored_ref_kind);
                return false;
            }
        }
        definition.nodes.push_back(std::move(saved));
    }
    for (const auto& edge : draft.edges) {
        if (edge.edge_kind == "CONTROL") {
            definition.control_dependencies.push_back({
                edge.from_node_key, edge.to_node_key,
                edge.guard_kind, edge.guard_value});
            continue;
        }
        definition.edges.push_back({
            edge.from_node_key, {edge.output_key}, edge.to_node_key,
            {edge.input_key}, edge.guard_kind, edge.guard_value});
    }
    for (const auto& node : draft.nodes) {
        for (const auto& input : node.inputs) {
            if (!input.required) continue;
            const bool connected = std::ranges::any_of(
                draft.edges, [&](const auto& edge) {
                    return edge.to_node_key == node.node_key
                        && edge.input_key == input.input_key;
                });
            if (!connected) {
                definition.external_inputs.push_back({
                    node.node_key, {input.input_key}});
            }
        }
    }
    const AuthoringRecipeMaterializer materializer(db);
    return materializer.SaveWorkflowGraph(
        definition, {}, result_out, error_out);
}

bool MaterializePredicateDefinitionDraft(
    IAuthoringDb* db, const CreatePredicateDefinitionDraftCommand& draft,
    PredicateAuthoringRevisionReceipt* receipt_out, std::string* error_out)
{
    if (db == nullptr) { SetError(error_out, "authoring database is unavailable"); return false; }
    return db->CreatePredicateDefinitionDraft(draft, receipt_out, error_out);
}

bool MaterializePredicateExecutionBindingDraft(
    IAuthoringDb* db, const CreatePredicateExecutionBindingDraftCommand& draft,
    PredicateAuthoringRevisionReceipt* receipt_out, std::string* error_out)
{
    if (db == nullptr) { SetError(error_out, "authoring database is unavailable"); return false; }
    return db->CreatePredicateExecutionBindingDraft(draft, receipt_out, error_out);
}

bool MaterializePredicateGroupDraft(
    IAuthoringDb* db, const CreatePredicateGroupDraftCommand& draft,
    PredicateAuthoringRevisionReceipt* receipt_out, std::string* error_out)
{
    if (db == nullptr) { SetError(error_out, "authoring database is unavailable"); return false; }
    return db->CreatePredicateGroupDraft(draft, receipt_out, error_out);
}

bool MaterializeBattlePlanHeader(
    IAuthoringDb* db, const SavePlanCommand& draft,
    std::int64_t* id_out, std::string* error_out)
{
    if (db == nullptr) { SetError(error_out, "authoring database is unavailable"); return false; }
    return db->SavePlan(draft, id_out, error_out);
}

bool MaterializeBattleActionPreset(
    IAuthoringDb* db, const SaveBattlePlanActionPresetCommand& draft,
    std::int64_t* id_out, std::string* error_out)
{
    if (db == nullptr) { SetError(error_out, "authoring database is unavailable"); return false; }
    return db->SaveBattlePlanActionPreset(draft, id_out, error_out);
}

bool MaterializeBattlePlanTurn(
    IAuthoringDb* db, const SaveBattlePlanTurnCommand& draft,
    std::int64_t* id_out, std::string* error_out)
{
    if (db == nullptr) { SetError(error_out, "authoring database is unavailable"); return false; }
    return db->SaveBattlePlanTurn(draft, id_out, error_out);
}

} // namespace savor::db::authoring
