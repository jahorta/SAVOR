#include "WorkflowLaunchContract.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace savor::db::execution::workflow {
namespace {

std::string Key(const std::string& node, const std::string& member) {
    return node + "\n" + member;
}

const WorkflowGraphNodeInputSnapshot* FindInput(
    const WorkflowGraphNodeSnapshot& node,
    const std::string& key) {
    const auto it = std::find_if(node.inputs.begin(), node.inputs.end(),
        [&](const auto& input) { return input.input_key == key; });
    return it == node.inputs.end() ? nullptr : &*it;
}

const WorkflowGraphNodeOutputSnapshot* FindOutput(
    const WorkflowGraphNodeSnapshot& node,
    const std::string& key) {
    const auto it = std::find_if(node.possible_outputs.begin(), node.possible_outputs.end(),
        [&](const auto& output) { return output.output_key == key; });
    return it == node.possible_outputs.end() ? nullptr : &*it;
}

std::optional<std::int64_t> ParseInteger(const std::string& text) {
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

std::string_view ValueTypeName(WorkflowLaunchArgumentValueType type) {
    switch (type) {
    case WorkflowLaunchArgumentValueType::Integer: return "integer";
    case WorkflowLaunchArgumentValueType::Text: return "text";
    case WorkflowLaunchArgumentValueType::Boolean: return "boolean";
    case WorkflowLaunchArgumentValueType::Json: return "json";
    case WorkflowLaunchArgumentValueType::Choice: return "choice";
    }
    return {};
}

} // namespace

WorkflowLaunchContractValidation WorkflowLaunchContractValidator::Validate(
    const savor::db::WorkflowGraphSnapshot& graph,
    const WorkflowUnitRegistry& registry,
    const std::vector<WorkflowLaunchInputValue>& inputs,
    const std::vector<WorkflowLaunchArgumentValue>& arguments) {
    WorkflowLaunchContractValidation out{};
    std::unordered_map<std::string, const WorkflowGraphNodeSnapshot*> nodes;
    for (const auto& node : graph.nodes) {
        if (node.node_key.empty() || !nodes.emplace(node.node_key, &node).second) {
            out.issues.push_back("workflow graph has an empty or duplicate node key");
            continue;
        }
        const auto* unit = registry.Find(node.unit_kind);
        if (unit == nullptr) {
            out.issues.push_back("unknown workflow unit: " + node.unit_kind);
            continue;
        }
        if (node.inputs.size() != unit->required_inputs.size()
            || node.possible_outputs.size() != unit->possible_outputs.size()
            || node.arguments.size() != unit->launch_arguments.size()
            || node.argument_constraints.size() != unit->launch_argument_constraints.size()) {
            out.issues.push_back("saved workflow node contract does not match its registered unit: " + node.node_key);
        }
        for (const auto& expected : unit->required_inputs) {
            const auto* actual = FindInput(node, expected.key);
            if (actual == nullptr || actual->data_kind != expected.data_kind
                || actual->ref_kind != expected.ref_kind || actual->required != expected.required) {
                out.issues.push_back("saved workflow input contract mismatch: " + node.node_key + "." + expected.key);
            }
        }
        for (const auto& expected : unit->possible_outputs) {
            const auto* actual = FindOutput(node, expected.key);
            if (actual == nullptr || actual->data_kind != expected.data_kind
                || actual->ref_kind != expected.ref_kind) {
                out.issues.push_back("saved workflow output contract mismatch: " + node.node_key + "." + expected.key);
            }
        }
        for (const auto& expected : unit->launch_arguments) {
            const auto actual = std::find_if(node.arguments.begin(), node.arguments.end(),
                [&](const auto& value) { return value.argument_key == expected.key; });
            if (actual == node.arguments.end()
                || actual->value_type != ValueTypeName(expected.value_type)
                || actual->required != expected.required
                || actual->default_value != expected.default_value
                || actual->minimum_integer != expected.minimum_integer
                || actual->maximum_integer != expected.maximum_integer
                || actual->choices.size() != expected.choices.size()) {
                out.issues.push_back("saved workflow argument contract mismatch: " + node.node_key + "." + expected.key);
            } else {
                for (std::size_t index = 0; index < expected.choices.size(); ++index) {
                    if (actual->choices[index].value != expected.choices[index].value
                        || actual->choices[index].display_name != expected.choices[index].display_name) {
                        out.issues.push_back("saved workflow argument choice contract mismatch: " + node.node_key + "." + expected.key);
                        break;
                    }
                }
            }
        }
        for (const auto& expected : unit->launch_argument_constraints) {
            const auto actual = std::find_if(node.argument_constraints.begin(), node.argument_constraints.end(),
                [&](const auto& value) {
                    return value.lesser_or_equal_key == expected.lesser_or_equal_key
                        && value.greater_or_equal_key == expected.greater_or_equal_key
                        && value.message == expected.message;
                });
            if (actual == node.argument_constraints.end()) {
                out.issues.push_back("saved workflow argument constraint mismatch: " + node.node_key);
            }
        }
        for (const auto& requirement : unit->authored_refs) {
            if (requirement.required
                && (node.authored_ref_kind != std::optional<std::string>(requirement.ref_kind)
                    || !node.authored_ref_id || *node.authored_ref_id <= 0)) {
                out.issues.push_back("required authored reference is unresolved: " + node.node_key + "." + requirement.ref_kind);
            }
        }
    }

    std::unordered_set<std::string> edge_inputs;
    for (const auto& edge : graph.edges) {
        const auto from = nodes.find(edge.from_node_key);
        const auto to = nodes.find(edge.to_node_key);
        if (from == nodes.end() || to == nodes.end()) {
            out.issues.push_back("workflow edge references an unknown node");
            continue;
        }
        if (edge.edge_kind == "CONTROL") {
            if (!edge.output_key.empty() || !edge.input_key.empty())
                out.issues.push_back("workflow control edge must not declare data ports");
            continue;
        }
        if (edge.edge_kind != "DATA") {
            out.issues.push_back("workflow edge has an unsupported kind");
            continue;
        }
        const auto* output = FindOutput(*from->second, edge.output_key);
        const auto* input = FindInput(*to->second, edge.input_key);
        if (output == nullptr || input == nullptr) {
            out.issues.push_back("workflow edge references an unknown port");
            continue;
        }
        if (output->data_kind != input->data_kind || output->ref_kind != input->ref_kind) {
            out.issues.push_back("workflow edge data/ref kind mismatch: " + edge.to_node_key + "." + edge.input_key);
            continue;
        }
        if (!edge_inputs.emplace(Key(edge.to_node_key, edge.input_key)).second) {
            out.issues.push_back("workflow input has more than one upstream edge: " + edge.to_node_key + "." + edge.input_key);
        }
    }

    std::unordered_set<std::string> external_inputs;
    for (const auto& supplied : inputs) {
        const auto node = nodes.find(supplied.node_key);
        if (node == nodes.end() || supplied.ref_id <= 0) {
            out.issues.push_back("external input references an unknown node or invalid id");
            continue;
        }
        const auto* input = FindInput(*node->second, supplied.input_key);
        if (input == nullptr || input->data_kind != supplied.data_kind || input->ref_kind != supplied.ref_kind) {
            out.issues.push_back("external input data/ref kind mismatch: " + supplied.node_key + "." + supplied.input_key);
            continue;
        }
        const auto key = Key(supplied.node_key, supplied.input_key);
        if (!external_inputs.emplace(key).second) {
            out.issues.push_back("duplicate external input: " + supplied.node_key + "." + supplied.input_key);
        }
        if (supplied.source_kind == "external_override" && edge_inputs.find(key) == edge_inputs.end()) {
            out.issues.push_back("external_override requires an authored edge: " + supplied.node_key + "." + supplied.input_key);
        } else if (supplied.source_kind != "external_override" && edge_inputs.find(key) != edge_inputs.end()) {
            out.issues.push_back("input is supplied by both an edge and an external binding: " + supplied.node_key + "." + supplied.input_key);
        }
    }
    for (const auto& [node_key, node] : nodes) {
        for (const auto& input : node->inputs) {
            if (input.required && edge_inputs.find(Key(node_key, input.input_key)) == edge_inputs.end()
                && external_inputs.find(Key(node_key, input.input_key)) == external_inputs.end()) {
                out.issues.push_back("required input is not supplied: " + node_key + "." + input.input_key);
            }
        }
    }

    std::unordered_map<std::string, const WorkflowLaunchArgumentValue*> supplied_arguments;
    for (const auto& argument : arguments) {
        const auto node = nodes.find(argument.node_key);
        if (node == nodes.end()) {
            out.issues.push_back("workflow argument references an unknown node");
            continue;
        }
        const auto definition = std::find_if(node->second->arguments.begin(), node->second->arguments.end(),
            [&](const auto& item) { return item.argument_key == argument.argument_key; });
        if (definition == node->second->arguments.end() || definition->value_type != argument.value_type) {
            out.issues.push_back("unknown or mistyped workflow argument: " + argument.node_key + "." + argument.argument_key);
            continue;
        }
        if (definition->binding_mode ==
            savor::db::SaveWorkflowGraphNodeArgumentCommand::BindingMode::Constant) {
            out.issues.push_back("graph constant cannot be overridden: "
                + argument.node_key + "." + argument.argument_key);
            continue;
        }
        if (!supplied_arguments.emplace(Key(argument.node_key, argument.argument_key), &argument).second) {
            out.issues.push_back("duplicate workflow argument: " + argument.node_key + "." + argument.argument_key);
        }
    }

    std::unordered_map<std::string, std::int64_t> normalized_integers;
    for (const auto& [node_key, node] : nodes) {
        for (const auto& definition : node->arguments) {
            const auto key = Key(node_key, definition.argument_key);
            WorkflowLaunchArgumentValue normalized{};
            normalized.node_key = node_key;
            normalized.argument_key = definition.argument_key;
            normalized.value_type = definition.value_type;
            normalized.source_kind = "catalog_default";
            if (definition.binding_mode ==
                savor::db::SaveWorkflowGraphNodeArgumentCommand::BindingMode::Constant) {
                if (!definition.constant_value) {
                    out.issues.push_back("workflow graph constant is missing: "
                        + node_key + "." + definition.argument_key);
                    continue;
                }
                normalized.source_kind = "graph_constant";
                if (definition.value_type == "integer")
                    normalized.integer_value = ParseInteger(*definition.constant_value);
                else normalized.text_value = definition.constant_value;
            } else if (const auto supplied = supplied_arguments.find(key); supplied != supplied_arguments.end()) {
                normalized = *supplied->second;
                if (normalized.source_kind.empty()) normalized.source_kind = "launcher";
            } else if (definition.default_value.has_value()) {
                if (definition.value_type == "integer") normalized.integer_value = ParseInteger(*definition.default_value);
                else normalized.text_value = definition.default_value;
            } else if (definition.required) {
                out.issues.push_back("required workflow argument is not supplied: " + node_key + "." + definition.argument_key);
                continue;
            } else {
                continue;
            }

            if (definition.value_type == "integer") {
                if (!normalized.integer_value.has_value() || normalized.text_value.has_value()) {
                    out.issues.push_back("integer workflow argument has the wrong value shape: " + node_key + "." + definition.argument_key);
                    continue;
                }
                const auto value = *normalized.integer_value;
                if ((definition.minimum_integer && value < *definition.minimum_integer)
                    || value < 0
                    || (definition.maximum_integer && static_cast<std::uint64_t>(value) > *definition.maximum_integer)) {
                    out.issues.push_back("workflow argument is outside its numeric bounds: " + node_key + "." + definition.argument_key);
                    continue;
                }
                normalized_integers[key] = value;
            } else if (!normalized.text_value.has_value() || normalized.integer_value.has_value()) {
                out.issues.push_back("workflow argument has the wrong value shape: " + node_key + "." + definition.argument_key);
                continue;
            } else if (definition.value_type == "choice") {
                const auto selected = std::find_if(
                    definition.choices.begin(),
                    definition.choices.end(),
                    [&](const auto& choice) { return choice.value == *normalized.text_value; });
                if (selected == definition.choices.end()) {
                    out.issues.push_back("workflow choice is not in its saved catalog: " + node_key + "." + definition.argument_key);
                    continue;
                }
            }
            out.normalized_arguments.push_back(std::move(normalized));
        }
        for (const auto& constraint : node->argument_constraints) {
            const auto lesser = normalized_integers.find(Key(node_key, constraint.lesser_or_equal_key));
            const auto greater = normalized_integers.find(Key(node_key, constraint.greater_or_equal_key));
            if (lesser == normalized_integers.end() || greater == normalized_integers.end()
                || lesser->second > greater->second) {
                out.issues.push_back(constraint.message);
            }
        }
    }
    out.valid = out.issues.empty();
    return out;
}

} // namespace savor::db::execution::workflow
