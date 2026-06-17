#include "WorkflowComposition.h"

#include <algorithm>
#include <set>
#include <utility>

namespace savor::db::execution::workflow {
namespace {

WorkflowPortDefinition Port(
    std::string key,
    std::string data_kind,
    std::string display_name,
    bool required = true) {
    return WorkflowPortDefinition{
        .key = std::move(key),
        .data_kind = std::move(data_kind),
        .display_name = std::move(display_name),
        .required = required,
    };
}

std::string BindingLabel(std::string_view node_key, std::string_view port_key) {
    return std::string(node_key) + "." + std::string(port_key);
}

std::vector<WorkflowUnitStepTemplate> SingleStep(std::string step_kind) {
    return {
        WorkflowUnitStepTemplate{
            .step_key_suffix = "",
            .step_kind = std::move(step_kind),
            .priority = 1,
            .max_attempts = 1,
        },
    };
}

WorkflowUnitDefinition SeedProbeUnit(
    std::string unit_kind,
    std::string display_name,
    std::string description,
    std::string variant,
    std::string breakpoint_profile) {
    return WorkflowUnitDefinition{
        .unit_kind = std::move(unit_kind),
        .display_name = std::move(display_name),
        .description = std::move(description),
        .unit_variant = std::move(variant),
        .breakpoint_profile_key = std::move(breakpoint_profile),
        .default_activation_params_json = "{}",
        .authored_refs = {
            { .ref_kind = "seed_probe_spec", .display_name = "Seed probe spec" },
        },
        .required_inputs = {
            Port("entry_savestate", "state.savestate_id", "Entry savestate"),
        },
        .possible_outputs = {
            Port("unique_input_frames", "analysis.input_frame_set_id", "Unique input frames"),
        },
        .internal_step_kinds = { "seed_probe_chain", "seedprobe.grid", "seedprobe.unique" },
        .step_templates = SingleStep("seed_probe_chain"),
    };
}

} // namespace

bool WorkflowUnitRegistry::RegisterUnit(WorkflowUnitDefinition definition, std::string* error_out) {
    if (definition.unit_kind.empty()) {
        if (error_out) *error_out = "unit_kind is required";
        return false;
    }
    if (definition.display_name.empty()) {
        if (error_out) *error_out = "display_name is required for unit " + definition.unit_kind;
        return false;
    }

    std::set<std::string> input_keys;
    for (const auto& input : definition.required_inputs) {
        if (input.key.empty() || input.data_kind.empty()) {
            if (error_out) *error_out = "input key and data_kind are required for unit " + definition.unit_kind;
            return false;
        }
        if (!input_keys.emplace(input.key).second) {
            if (error_out) *error_out = "duplicate input key '" + input.key + "' for unit " + definition.unit_kind;
            return false;
        }
    }

    std::set<std::string> output_keys;
    for (const auto& output : definition.possible_outputs) {
        if (output.key.empty() || output.data_kind.empty()) {
            if (error_out) *error_out = "output key and data_kind are required for unit " + definition.unit_kind;
            return false;
        }
        if (!output_keys.emplace(output.key).second) {
            if (error_out) *error_out = "duplicate output key '" + output.key + "' for unit " + definition.unit_kind;
            return false;
        }
    }

    const auto inserted = units_.emplace(definition.unit_kind, std::move(definition));
    if (!inserted.second) {
        if (error_out) *error_out = "workflow unit already registered: " + inserted.first->first;
        return false;
    }
    return true;
}

const WorkflowUnitDefinition* WorkflowUnitRegistry::Find(std::string_view unit_kind) const {
    const auto it = units_.find(std::string(unit_kind));
    if (it == units_.end()) {
        return nullptr;
    }
    return &it->second;
}

std::vector<WorkflowUnitDefinition> WorkflowUnitRegistry::ListUnits() const {
    std::vector<WorkflowUnitDefinition> out;
    out.reserve(units_.size());
    for (const auto& [_, unit] : units_) {
        out.push_back(unit);
    }
    std::sort(out.begin(), out.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.display_name < rhs.display_name;
    });
    return out;
}

WorkflowUnitRegistry BuildDefaultWorkflowUnitRegistry() {
    WorkflowUnitRegistry registry;
    std::string ignored;

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "tas_movie",
            .display_name = "TAS Movie",
            .description = "Runs a DTM/TAS movie and produces a savestate for downstream chains.",
            .default_activation_params_json = "{}",
            .authored_refs = {
                { .ref_kind = "tas_spec", .display_name = "TAS spec" },
            },
            .required_inputs = {
                Port("dtm_artifact", "state_artifact.dtm_artifact_id", "DTM artifact"),
            },
            .possible_outputs = {
                Port("savestate", "state.savestate_id", "Output savestate"),
            },
            .internal_step_kinds = { "tasmovie.play" },
            .step_templates = SingleStep("tas_movie"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "seed_probe_chain",
            .display_name = "Seed Probe Chain",
            .description = "Runs neutral, grid, and unique seed probing as one reusable chain.",
            .unit_variant = "generic",
            .breakpoint_profile_key = "seedprobe.default",
            .default_activation_params_json = "{}",
            .authored_refs = {
                { .ref_kind = "seed_probe_spec", .display_name = "Seed probe spec" },
            },
            .required_inputs = {
                Port("entry_savestate", "state.savestate_id", "Entry savestate"),
            },
            .possible_outputs = {
                Port("unique_input_frames", "analysis.input_frame_set_id", "Unique input frames"),
            },
            .internal_step_kinds = { "seed_probe_chain", "seedprobe.grid", "seedprobe.unique" },
            .step_templates = SingleStep("seed_probe_chain"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        SeedProbeUnit(
            "battle_seed_probe",
            "Battle Seed Probe",
            "Runs the seed probe chain with the battle breakpoint profile.",
            "battle",
            "seedprobe.battle"),
        &ignored);

    (void)registry.RegisterUnit(
        SeedProbeUnit(
            "dungeon_seed_probe",
            "Dungeon Seed Probe",
            "Runs the seed probe chain with the dungeon breakpoint profile.",
            "dungeon",
            "seedprobe.dungeon"),
        &ignored);

    (void)registry.RegisterUnit(
        SeedProbeUnit(
            "overworld_seed_probe",
            "Overworld Seed Probe",
            "Runs the seed probe chain with the overworld breakpoint profile.",
            "overworld",
            "seedprobe.overworld"),
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "battle.context_probe",
            .display_name = "Battle Context Probe",
            .description = "Builds the initial battle context from a turn wave.",
            .unit_variant = "battle",
            .breakpoint_profile_key = "battle.context_probe",
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("turn_wave", "analysis_battle.turn_wave_id", "Turn wave"),
            },
            .possible_outputs = {
                Port("battle_context", "analysisbattle.context_probe", "Battle context"),
            },
            .internal_step_kinds = { "battle.context_probe" },
            .step_templates = SingleStep("battle.context_probe"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "battle_chain",
            .display_name = "Battle Chain",
            .description = "Builds battle context, then runs one or more battle turns from candidate input frames.",
            .unit_variant = "battle",
            .breakpoint_profile_key = "battle.default",
            .default_activation_params_json = "{}",
            .authored_refs = {
                { .ref_kind = "authoring.battle_chain_spec", .display_name = "Battle chain spec" },
            },
            .required_inputs = {
                Port("entry_savestate", "state.savestate_id", "Entry savestate"),
                Port("initial_input_frames", "analysis.input_frame_set_id", "Initial input frames"),
            },
            .possible_outputs = {
                Port("terminal_savestate", "state.savestate_id", "Terminal savestate"),
                Port("battle_manual_followup", "analysis.battle_manual_followup_id", "Battle manual follow-up"),
            },
            .internal_step_kinds = { "battle.context_probe", "battle.single_turn" },
            .step_templates = SingleStep("battle_chain"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "overworld_explorer",
            .display_name = "Overworld Explorer",
            .description = "Placeholder contract for future overworld traversal from a terminal savestate.",
            .hidden = true,
            .unit_variant = "overworld",
            .breakpoint_profile_key = "overworld.default",
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("entry_savestate", "state.savestate_id", "Entry savestate"),
            },
            .possible_outputs = {
                Port("terminal_savestate", "state.savestate_id", "Terminal savestate"),
            },
            .internal_step_kinds = {},
            .step_templates = SingleStep("overworld_explorer"),
        },
        &ignored);

    (void)registry.RegisterUnit(
        WorkflowUnitDefinition{
            .unit_kind = "dungeon_explorer",
            .display_name = "Dungeon Explorer",
            .description = "Placeholder contract for future dungeon traversal from a terminal savestate.",
            .hidden = true,
            .unit_variant = "dungeon",
            .breakpoint_profile_key = "dungeon.default",
            .default_activation_params_json = "{}",
            .required_inputs = {
                Port("entry_savestate", "state.savestate_id", "Entry savestate"),
            },
            .possible_outputs = {
                Port("terminal_savestate", "state.savestate_id", "Terminal savestate"),
            },
            .internal_step_kinds = {},
            .step_templates = SingleStep("dungeon_explorer"),
        },
        &ignored);

    return registry;
}

WorkflowCompositionService::WorkflowCompositionService(const WorkflowUnitRegistry* registry)
    : registry_(registry) {
}

WorkflowCompositionPreview WorkflowCompositionService::Preview(const WorkflowCompositionSpec& composition) const {
    WorkflowCompositionPreview preview{};
    if (registry_ == nullptr) {
        preview.issues.push_back({ "", "", "", "workflow unit registry unavailable" });
        return preview;
    }

    std::set<std::string> node_keys;
    for (const auto& node : composition.nodes) {
        if (node.node_key.empty()) {
            preview.issues.push_back({ "", "", "", "node_key is required" });
            continue;
        }
        if (!node_keys.emplace(node.node_key).second) {
            preview.issues.push_back({ node.node_key, "", "", "duplicate node_key" });
            continue;
        }
        const auto* unit = FindNodeUnit(node);
        if (unit == nullptr) {
            preview.issues.push_back({ node.node_key, "", "", "unknown workflow unit: " + node.unit_kind });
            continue;
        }

        WorkflowCompositionPreviewNode preview_node{};
        preview_node.node_key = node.node_key;
        preview_node.unit_kind = node.unit_kind;
        for (const auto& output : unit->possible_outputs) {
            preview_node.possible_outputs.push_back(BindingLabel(node.node_key, output.key) + " -> " + output.data_kind);
        }

        for (const auto& input : unit->required_inputs) {
            std::string resolved_label;
            WorkflowCompositionIssue issue{};
            if (ExternalBindingMatches(composition, node, input, &resolved_label)
                || OutputBindingMatches(composition, node, input, &resolved_label, &issue)) {
                preview_node.resolved_inputs.push_back(BindingLabel(node.node_key, input.key) + " <- " + resolved_label);
                continue;
            }

            if (!issue.message.empty()) {
                preview.issues.push_back(std::move(issue));
            } else if (input.required) {
                preview.issues.push_back({
                    node.node_key,
                    input.key,
                    input.data_kind,
                    "required input is not bound",
                });
            }
        }
        preview.nodes.push_back(std::move(preview_node));
    }

    preview.valid = preview.issues.empty();
    return preview;
}

std::vector<WorkflowUnitDefinition> WorkflowCompositionService::ListUnitsCompatibleWith(
    const std::vector<WorkflowPortDefinition>& available_outputs) const {
    std::vector<WorkflowUnitDefinition> out;
    if (registry_ == nullptr) {
        return out;
    }
    for (const auto& unit : registry_->ListUnits()) {
        bool all_required_satisfied = true;
        for (const auto& input : unit.required_inputs) {
            if (!input.required) {
                continue;
            }
            const auto it = std::find_if(available_outputs.begin(), available_outputs.end(), [&](const auto& output) {
                return output.data_kind == input.data_kind;
            });
            if (it == available_outputs.end()) {
                all_required_satisfied = false;
                break;
            }
        }
        if (all_required_satisfied) {
            out.push_back(unit);
        }
    }
    return out;
}

const WorkflowUnitDefinition* WorkflowCompositionService::FindNodeUnit(const WorkflowCompositionNode& node) const {
    return registry_ != nullptr ? registry_->Find(node.unit_kind) : nullptr;
}

const WorkflowPortDefinition* WorkflowCompositionService::FindInput(
    const WorkflowUnitDefinition& unit,
    std::string_view input_key) const {
    const auto it = std::find_if(unit.required_inputs.begin(), unit.required_inputs.end(), [&](const auto& input) {
        return input.key == input_key;
    });
    return it == unit.required_inputs.end() ? nullptr : &*it;
}

const WorkflowPortDefinition* WorkflowCompositionService::FindOutput(
    const WorkflowUnitDefinition& unit,
    std::string_view output_key) const {
    const auto it = std::find_if(unit.possible_outputs.begin(), unit.possible_outputs.end(), [&](const auto& output) {
        return output.key == output_key;
    });
    return it == unit.possible_outputs.end() ? nullptr : &*it;
}

bool WorkflowCompositionService::OutputBindingMatches(
    const WorkflowCompositionSpec& composition,
    const WorkflowCompositionNode& node,
    const WorkflowPortDefinition& input,
    std::string* resolved_label_out,
    WorkflowCompositionIssue* issue_out) const {
    for (const auto& binding : composition.output_bindings) {
        if (binding.to_node_key != node.node_key || binding.input_key != input.key) {
            continue;
        }

        const auto from_it = std::find_if(composition.nodes.begin(), composition.nodes.end(), [&](const auto& candidate) {
            return candidate.node_key == binding.from_node_key;
        });
        if (from_it == composition.nodes.end()) {
            if (issue_out) {
                *issue_out = { node.node_key, input.key, input.data_kind, "binding source node not found: " + binding.from_node_key };
            }
            return false;
        }
        const auto* from_unit = FindNodeUnit(*from_it);
        if (from_unit == nullptr) {
            if (issue_out) {
                *issue_out = { node.node_key, input.key, input.data_kind, "binding source unit not found" };
            }
            return false;
        }
        const auto* output = FindOutput(*from_unit, binding.output_key);
        if (output == nullptr) {
            if (issue_out) {
                *issue_out = { node.node_key, input.key, input.data_kind, "binding source output not found: " + binding.output_key };
            }
            return false;
        }
        if (output->data_kind != input.data_kind) {
            if (issue_out) {
                *issue_out = {
                    node.node_key,
                    input.key,
                    input.data_kind,
                    "binding type mismatch: " + output->data_kind + " cannot satisfy " + input.data_kind,
                };
            }
            return false;
        }
        if (resolved_label_out) {
            *resolved_label_out = BindingLabel(binding.from_node_key, binding.output_key);
        }
        return true;
    }
    return false;
}

bool WorkflowCompositionService::ExternalBindingMatches(
    const WorkflowCompositionSpec& composition,
    const WorkflowCompositionNode& node,
    const WorkflowPortDefinition& input,
    std::string* resolved_label_out) const {
    for (const auto& binding : composition.external_inputs) {
        if (binding.node_key != node.node_key || binding.input_key != input.key) {
            continue;
        }
        if (binding.data_kind != input.data_kind) {
            return false;
        }
        if (resolved_label_out) {
            *resolved_label_out = "external." + binding.data_kind;
        }
        return true;
    }
    return false;
}

} // namespace savor::db::execution::workflow
