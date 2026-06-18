#include "WorkflowGraphRoutingService.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../Authoring/IAuthoringDb.h"
#include "../IExecutionDb.h"

namespace savor::db::execution::workflow {
namespace {

bool IsTerminal(WorkflowStepState state) {
    return state == WorkflowStepState::Completed
        || state == WorkflowStepState::Failed
        || state == WorkflowStepState::Skipped;
}

bool IsTerminalActivation(WorkflowUnitActivationState state) {
    return state == WorkflowUnitActivationState::Completed
        || state == WorkflowUnitActivationState::Failed
        || state == WorkflowUnitActivationState::Skipped
        || state == WorkflowUnitActivationState::Canceled;
}

std::string BindingKey(const std::string& node_key, const std::string& input_key) {
    return node_key + "\n" + input_key;
}

const savor::db::WorkflowGraphNodeSnapshot* FindNode(
    const savor::db::WorkflowGraphSnapshot& graph,
    const std::string& node_key) {
    const auto it = std::find_if(
        graph.nodes.begin(),
        graph.nodes.end(),
        [&](const auto& node) { return node.node_key == node_key; });
    return it != graph.nodes.end() ? &*it : nullptr;
}

const savor::db::WorkflowGraphNodeInputSnapshot* FindInput(
    const savor::db::WorkflowGraphNodeSnapshot& node,
    const std::string& input_key) {
    const auto it = std::find_if(
        node.inputs.begin(),
        node.inputs.end(),
        [&](const auto& input) { return input.input_key == input_key; });
    return it != node.inputs.end() ? &*it : nullptr;
}

const WorkflowStepOutputRecord* FindOutput(
    const std::vector<WorkflowStepOutputRecord>& outputs,
    const std::string& graph_node_key,
    const std::string& output_key) {
    const auto it = std::find_if(
        outputs.begin(),
        outputs.end(),
        [&](const auto& output) {
            return output.graph_node_key == graph_node_key
                && output.output_key == output_key;
        });
    return it != outputs.end() ? &*it : nullptr;
}

bool HasActiveWorkForGraphNode(
    const WorkflowGraphSnapshot& execution_graph,
    const std::string& graph_node_key) {
    for (const auto& activation : execution_graph.unit_activations) {
        if (activation.graph_node_key == graph_node_key && !IsTerminalActivation(activation.state)) {
            return true;
        }
    }
    return std::any_of(
        execution_graph.steps.begin(),
        execution_graph.steps.end(),
        [&](const auto& step) {
            const auto step_graph_node_key = step.graph_node_key.empty() ? step.step_key : step.graph_node_key;
            return step_graph_node_key == graph_node_key && !IsTerminal(step.state);
        });
}

bool AllExecutionStepsTerminal(const WorkflowGraphSnapshot& execution_graph) {
    if (!execution_graph.unit_activations.empty()) {
        return std::all_of(
            execution_graph.unit_activations.begin(),
            execution_graph.unit_activations.end(),
            [](const auto& activation) {
                return activation.parent_workflow_unit_activation_id.has_value()
                    || IsTerminalActivation(activation.state);
            });
    }
    return std::all_of(
        execution_graph.steps.begin(),
        execution_graph.steps.end(),
        [](const auto& step) { return IsTerminal(step.state); });
}

} // namespace

WorkflowGraphRoutingService::WorkflowGraphRoutingService(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAuthoringDb* authoring_db,
    IWorkflowOrchestrationQueryService* query_service,
    IWorkflowOrchestrationCommandService* command_service,
    int successor_step_priority_boost)
    : execution_db_(execution_db)
    , authoring_db_(authoring_db)
    , query_service_(query_service)
    , command_service_(command_service)
    , successor_step_priority_boost_(successor_step_priority_boost) {
}

bool WorkflowGraphRoutingService::RouteTerminalStep(
    const WorkflowStepTerminalSnapshot& snapshot,
    WorkflowGraphRoutingResult* result_out,
    std::string* error_out) const {
    WorkflowGraphRoutingResult result{};
    if (snapshot.workflow_kind != "workflow_graph") {
        if (result_out) {
            *result_out = result;
        }
        return true;
    }
    result.graph_instance = true;

    if (!snapshot.workflow_graph_revision_id.has_value() || *snapshot.workflow_graph_revision_id <= 0) {
        if (error_out) *error_out = "workflow_graph_revision_id is required for graph routing";
        return false;
    }
    if (execution_db_ == nullptr || authoring_db_ == nullptr || query_service_ == nullptr || command_service_ == nullptr) {
        if (error_out) *error_out = "graph routing dependencies are not configured";
        return false;
    }

    const auto authored_graph = authoring_db_->GetWorkflowGraphRevision(*snapshot.workflow_graph_revision_id);
    if (!authored_graph.has_value()) {
        if (error_out) *error_out = "workflow graph revision not found";
        return false;
    }

    auto execution_graph = query_service_->GetWorkflowGraph(snapshot.workflow_instance_id);
    if (!execution_graph.has_value()) {
        if (error_out) *error_out = "workflow execution graph not found";
        return false;
    }

    auto graph_node_key = snapshot.graph_node_key.empty() ? snapshot.step_key : snapshot.graph_node_key;
    if (snapshot.workflow_unit_activation_id.has_value()) {
        const auto activation_it = std::find_if(
            execution_graph->unit_activations.begin(),
            execution_graph->unit_activations.end(),
            [&](const auto& activation) {
                return activation.workflow_unit_activation_id == *snapshot.workflow_unit_activation_id;
            });
        if (activation_it != execution_graph->unit_activations.end()) {
            if (!activation_it->graph_node_key.empty()) {
                graph_node_key = activation_it->graph_node_key;
            }
            if (!IsTerminalActivation(activation_it->state)) {
                if (result_out) {
                    *result_out = result;
                }
                return true;
            }
        }
    }
    const auto job_outputs = execution_db_->ListJobOutputsForWorkflowStep(snapshot.workflow_step_id);
    for (const auto& job_output : job_outputs) {
        std::string command_error;
        if (!command_service_->RecordStepOutput(
                {
                    .workflow_step_id = snapshot.workflow_step_id,
                    .output_key = job_output.output_key,
                    .output_data_kind = job_output.data_kind,
                    .output_ref_kind = job_output.ref_kind,
                    .output_ref_id = job_output.ref_id,
                    .requested_by = "workflow_graph_routing",
                },
                &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        }
    }

    const auto outputs = query_service_->ListStepOutputs(snapshot.workflow_instance_id);
    std::vector<std::string> touched_targets;
    std::unordered_set<std::string> satisfied_bindings;
    std::unordered_map<std::string, const WorkflowInstanceInputBindingRecord*> binding_by_key;
    for (const auto& binding : execution_graph->input_bindings) {
        const auto key = BindingKey(binding.node_key, binding.input_key);
        satisfied_bindings.insert(key);
        binding_by_key.emplace(key, &binding);
    }
    const auto touch_target = [&touched_targets](const std::string& node_key) {
        if (std::find(touched_targets.begin(), touched_targets.end(), node_key) == touched_targets.end()) {
            touched_targets.push_back(node_key);
        }
    };

    for (const auto& edge : authored_graph->edges) {
        if (edge.from_node_key != graph_node_key) {
            continue;
        }

        const auto target_binding_key = BindingKey(edge.to_node_key, edge.input_key);
        const auto existing_binding = binding_by_key.find(target_binding_key);
        if (existing_binding != binding_by_key.end()
            && existing_binding->second->source_kind == "external_override") {
            satisfied_bindings.insert(target_binding_key);
            touch_target(edge.to_node_key);
            continue;
        }

        const auto* output = FindOutput(outputs, edge.from_node_key, edge.output_key);
        if (output == nullptr) {
            if (HasActiveWorkForGraphNode(*execution_graph, graph_node_key)) {
                if (result_out) {
                    *result_out = result;
                }
                return true;
            }
            std::ostringstream reason;
            reason << "graph_output_missing:" << edge.from_node_key << "." << edge.output_key;
            result.blocked_reason = reason.str();
            if (result_out) {
                *result_out = result;
            }
            return true;
        }

        const auto* to_node = FindNode(*authored_graph, edge.to_node_key);
        if (to_node == nullptr) {
            if (error_out) *error_out = "workflow graph edge references unknown target node";
            return false;
        }
        const auto* target_input = FindInput(*to_node, edge.input_key);
        if (target_input == nullptr) {
            if (error_out) *error_out = "workflow graph edge references unknown target input";
            return false;
        }
        if (!target_input->data_kind.empty() && target_input->data_kind != output->data_kind) {
            if (error_out) *error_out = "workflow graph edge data_kind mismatch";
            return false;
        }

        std::string command_error;
        if (!command_service_->RecordInputBinding(
                {
                    .workflow_instance_id = snapshot.workflow_instance_id,
                    .workflow_graph_revision_id = *snapshot.workflow_graph_revision_id,
                    .node_key = edge.to_node_key,
                    .input_key = edge.input_key,
                    .data_kind = output->data_kind,
                    .ref_kind = output->ref_kind,
                    .ref_id = output->ref_id,
                    .source_kind = "upstream",
                    .requested_by = "workflow_graph_routing",
                },
                &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        }
        result.routed_input_binding = true;
        satisfied_bindings.insert(target_binding_key);
        touch_target(edge.to_node_key);
    }

    for (const auto& target_node_key : touched_targets) {
        const auto* target_node = FindNode(*authored_graph, target_node_key);
        if (target_node == nullptr) {
            continue;
        }

        const bool ready = std::all_of(
            target_node->inputs.begin(),
            target_node->inputs.end(),
            [&](const auto& input) {
                return !input.required || satisfied_bindings.find(BindingKey(target_node_key, input.input_key)) != satisfied_bindings.end();
            });
        if (!ready) {
            continue;
        }

        std::string command_error;
        if (!command_service_->MarkStepReady(
                {
                    .workflow_instance_id = snapshot.workflow_instance_id,
                    .step_key = target_node_key,
                    .requested_by = "workflow_graph_routing",
                    .priority_delta = successor_step_priority_boost_,
                },
                &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        }
        result.advanced_ready_step = true;
    }

    execution_graph = query_service_->GetWorkflowGraph(snapshot.workflow_instance_id);
    if (execution_graph.has_value()
        && AllExecutionStepsTerminal(*execution_graph)
        && !result.advanced_ready_step) {
        std::string command_error;
        if (!command_service_->CompleteWorkflowInstance(
                {
                    .workflow_instance_id = snapshot.workflow_instance_id,
                    .requested_by = "workflow_graph_routing",
                },
                &command_error)) {
            if (command_error != "complete precondition failed") {
                if (error_out) *error_out = command_error;
                return false;
            }
        } else {
            result.workflow_completed = true;
        }
    }

    if (result_out) {
        *result_out = result;
    }
    return true;
}

} // namespace savor::db::execution::workflow
