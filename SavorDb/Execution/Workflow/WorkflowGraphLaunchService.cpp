#include "WorkflowGraphLaunchService.h"

#include "WorkflowUnitActivationFactory.h"
#include "../../Authoring/IAuthoringDb.h"
#include "../IExecutionDb.h"

#include <unordered_map>
#include <unordered_set>

namespace savor::db::execution::workflow {
namespace {

bool Fail(std::string message, std::string* error_out) {
    if (error_out) *error_out = std::move(message);
    return false;
}

}

WorkflowGraphLaunchService::WorkflowGraphLaunchService(
    IAuthoringDb* authoring, IExecutionDb* execution)
    : authoring_(authoring), execution_(execution) {}

bool WorkflowGraphLaunchService::Start(
    const WorkflowGraphLaunchRequest& request,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) const {
    if (!authoring_ || !execution_ || request.workflow_graph_revision_id <= 0)
        return Fail("workflow graph launch dependencies are incomplete", error_out);
    const auto graph = authoring_->GetWorkflowGraphRevision(
        request.workflow_graph_revision_id);
    if (!graph || graph->nodes.empty())
        return Fail("workflow graph revision was not found or has no nodes", error_out);

    const auto registry = BuildDefaultWorkflowUnitRegistry();
    const auto contract = WorkflowLaunchContractValidator::Validate(
        *graph, registry, request.input_bindings, request.arguments);
    if (!contract.valid)
        return Fail(contract.issues.empty() ? "workflow launch contract is invalid"
                                            : contract.issues.front(), error_out);

    const auto key = [](const std::string& node, const std::string& input) {
        return node + "\n" + input;
    };
    std::unordered_map<std::string, const WorkflowGraphNodeSnapshot*> nodes;
    std::unordered_map<std::string, const WorkflowGraphNodeInputSnapshot*> inputs;
    for (const auto& node : graph->nodes) {
        if (node.node_key.empty() || node.unit_kind.empty() ||
            !nodes.emplace(node.node_key, &node).second)
            return Fail("workflow graph contains an invalid or duplicate node", error_out);
        const auto* unit = registry.Find(node.unit_kind);
        if (!unit || unit->execution_shape != WorkflowUnitExecutionShape::WorkflowInstance)
            return Fail("workflow instance graph contains an expansion node", error_out);
        for (const auto& input : node.inputs)
            inputs.emplace(key(node.node_key, input.input_key), &input);
    }

    std::unordered_set<std::string> supplied_by_edge;
    for (const auto& edge : graph->edges) {
        if (!nodes.contains(edge.from_node_key) || !nodes.contains(edge.to_node_key))
            return Fail("workflow graph edge references an unknown node", error_out);
        if (edge.edge_kind == "DATA") {
            supplied_by_edge.insert(key(edge.to_node_key, edge.input_key));
        } else if (edge.edge_kind != "CONTROL") {
            return Fail("workflow graph edge has an unsupported kind", error_out);
        }
    }

    WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.workflow_graph_revision_id = graph->workflow_graph_revision_id;
    command.created_by = request.created_by.empty() ? "SavorDb" : request.created_by;
    command.launch_key = request.launch_key;
    command.created_at_utc = types::UtcNow().time_since_epoch().count();

    std::unordered_set<std::string> supplied_inputs;
    for (const auto& binding : request.input_bindings) {
        const auto binding_key = key(binding.node_key, binding.input_key);
        const auto found = inputs.find(binding_key);
        if (found == inputs.end() || binding.ref_id <= 0 ||
            found->second->data_kind != binding.data_kind ||
            found->second->ref_kind != binding.ref_kind ||
            !supplied_inputs.emplace(binding_key).second)
            return Fail("workflow input binding is invalid: " + binding.node_key + "." + binding.input_key, error_out);
        command.input_bindings.push_back({binding.node_key, binding.input_key,
            binding.data_kind, binding.ref_kind, binding.ref_id,
            binding.source_kind.empty() ? "external" : binding.source_kind});
    }
    for (const auto& argument : contract.normalized_arguments)
        command.arguments.push_back({argument.node_key, argument.argument_key,
            argument.value_type, argument.integer_value, argument.text_value,
            argument.source_kind.empty() ? "launcher" : argument.source_kind});
    for (const auto& edge : graph->edges) {
        command.activation_edges.push_back({
            .from_activation_key = edge.from_node_key,
            .to_activation_key = edge.to_node_key,
            .output_key = edge.edge_kind == "DATA"
                ? std::optional<std::string>(edge.output_key) : std::nullopt,
            .input_key = edge.edge_kind == "DATA"
                ? std::optional<std::string>(edge.input_key) : std::nullopt,
            .condition_kind = edge.guard_kind,
            .condition_value = edge.guard_value,
        });
    }

    for (const auto& node : graph->nodes) {
        for (const auto& input : node.inputs) {
            const auto input_key = key(node.node_key, input.input_key);
            if (input.required && !supplied_by_edge.contains(input_key) &&
                !supplied_inputs.contains(input_key))
                return Fail("required input is not supplied: " + node.node_key + "." + input.input_key, error_out);
        }
        std::string activation_error;
        auto activation = BuildUnitActivationSpecFromDefinition(registry,
            node.node_key, node.node_key, node.unit_kind, node.display_name,
            node.authored_ref_kind, node.authored_ref_id,
            {}, &activation_error);
        if (!activation) return Fail(std::move(activation_error), error_out);
        command.unit_activations.push_back(std::move(*activation));
    }
    return execution_->CreateWorkflowInstance(
        command, workflow_instance_id_out, error_out);
}

} // namespace savor::db::execution::workflow
