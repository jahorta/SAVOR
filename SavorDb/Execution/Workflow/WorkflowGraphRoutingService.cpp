#include "WorkflowGraphRoutingService.h"
#include "WorkflowComposition.h"

#include <algorithm>
#include <deque>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../Authoring/IAuthoringDb.h"
#include "../IExecutionDb.h"

namespace savor::db::execution::workflow {
namespace {

bool IsSettled(WorkflowStepState state) {
    return state == WorkflowStepState::Completed
        || state == WorkflowStepState::Failed
        || state == WorkflowStepState::Skipped
        || state == WorkflowStepState::Canceled
        || state == WorkflowStepState::Interrupted;
}

bool IsSettledActivation(WorkflowUnitActivationState state) {
    return state == WorkflowUnitActivationState::Completed
        || state == WorkflowUnitActivationState::Failed
        || state == WorkflowUnitActivationState::Skipped
        || state == WorkflowUnitActivationState::Canceled
        || state == WorkflowUnitActivationState::Interrupted;
}

bool IsSuccessful(WorkflowStepState state) {
    return state == WorkflowStepState::Completed
        || state == WorkflowStepState::Skipped;
}

bool IsSuccessfulActivation(WorkflowUnitActivationState state) {
    return state == WorkflowUnitActivationState::Completed
        || state == WorkflowUnitActivationState::Skipped;
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

const savor::db::WorkflowGraphNodeOutputSnapshot* FindDeclaredOutput(
    const savor::db::WorkflowGraphNodeSnapshot& node,
    const std::string& output_key) {
    const auto it = std::find_if(
        node.possible_outputs.begin(),
        node.possible_outputs.end(),
        [&](const auto& output) { return output.output_key == output_key; });
    return it != node.possible_outputs.end() ? &*it : nullptr;
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
        if (activation.graph_node_key == graph_node_key && !IsSettledActivation(activation.state)) {
            return true;
        }
    }
    return std::any_of(
        execution_graph.steps.begin(),
        execution_graph.steps.end(),
        [&](const auto& step) {
            const auto step_graph_node_key = step.graph_node_key.empty() ? step.step_key : step.graph_node_key;
            return step_graph_node_key == graph_node_key && !IsSettled(step.state);
        });
}

bool AllExecutionStepsSuccessful(const WorkflowGraphSnapshot& execution_graph) {
    if (!execution_graph.unit_activations.empty()) {
        return std::all_of(
            execution_graph.unit_activations.begin(),
            execution_graph.unit_activations.end(),
            [](const auto& activation) {
                return activation.parent_workflow_unit_activation_id.has_value()
                    || IsSuccessfulActivation(activation.state);
            });
    }
    return std::all_of(
        execution_graph.steps.begin(),
        execution_graph.steps.end(),
        [](const auto& step) { return IsSuccessful(step.state); });
}

bool ValidateAuthoredGraphDefinition(
    const savor::db::WorkflowGraphSnapshot& graph,
    const WorkflowUnitRegistry& registry,
    std::string* code_out,
    std::string* error_out) {
    for (const auto& node : graph.nodes) {
        if (registry.Find(node.unit_kind) == nullptr) {
            if (code_out) *code_out = "UNREGISTERED_WORKFLOW_UNIT";
            if (error_out) *error_out =
                "workflow graph node references an unregistered unit: "
                + node.node_key;
            return false;
        }
    }
    for (const auto& edge : graph.edges) {
        const auto* source = FindNode(graph, edge.from_node_key);
        const auto* target = FindNode(graph, edge.to_node_key);
        if (source == nullptr || target == nullptr) {
            if (code_out) *code_out = "UNKNOWN_WORKFLOW_EDGE_NODE";
            if (error_out) *error_out =
                "workflow graph edge references an unknown node";
            return false;
        }
        if (edge.edge_kind != "DATA") continue;
        if ((edge.guard_kind.has_value()
                && *edge.guard_kind != savor::db::kWorkflowOutputPresentGuard)
            || edge.guard_value.has_value()) {
            if (code_out) *code_out = "UNSUPPORTED_WORKFLOW_EDGE_GUARD";
            if (error_out) *error_out =
                "workflow graph contains an unsupported edge guard";
            return false;
        }
        const auto* output = FindDeclaredOutput(*source, edge.output_key);
        const auto* input = FindInput(*target, edge.input_key);
        if (output == nullptr || input == nullptr) {
            if (code_out) *code_out = "UNKNOWN_WORKFLOW_EDGE_PORT";
            if (error_out) *error_out =
                "workflow graph edge references an unknown port";
            return false;
        }
        if (output->data_kind != input->data_kind
            || output->ref_kind != input->ref_kind) {
            if (code_out) *code_out = "WORKFLOW_EDGE_CONTRACT_MISMATCH";
            if (error_out) *error_out =
                "workflow graph edge contract mismatch: "
                + edge.from_node_key + "." + edge.output_key + " -> "
                + edge.to_node_key + "." + edge.input_key;
            return false;
        }
    }
    return true;
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
    const WorkflowStepSettlementSnapshot& snapshot,
    WorkflowGraphRoutingResult* result_out,
    std::string* error_out) const {
    WorkflowGraphRoutingResult result{};
    const auto fail = [&](WorkflowGraphRoutingFailureClass failure_class,
                          std::string code,
                          std::string message) {
        result.failure = WorkflowGraphRoutingFailure{
            .failure_class = failure_class,
            .code = std::move(code),
            .message = std::move(message),
        };
        if (result_out) *result_out = result;
        if (error_out) *error_out = result.failure->message;
        return false;
    };
    if (snapshot.workflow_kind != "workflow_graph") {
        if (result_out) {
            *result_out = result;
        }
        return true;
    }
    result.graph_instance = true;

    if (!snapshot.workflow_graph_revision_id.has_value() || *snapshot.workflow_graph_revision_id <= 0) {
        return fail(WorkflowGraphRoutingFailureClass::GraphConstruction,
            "MISSING_WORKFLOW_GRAPH_REVISION",
            "workflow_graph_revision_id is required for graph routing");
    }
    if (execution_db_ == nullptr || authoring_db_ == nullptr || query_service_ == nullptr || command_service_ == nullptr) {
        return fail(WorkflowGraphRoutingFailureClass::Operation,
            "GRAPH_ROUTING_NOT_CONFIGURED",
            "graph routing dependencies are not configured");
    }

    const auto authored_graph = authoring_db_->GetWorkflowGraphRevision(*snapshot.workflow_graph_revision_id);
    if (!authored_graph.has_value()) {
        return fail(WorkflowGraphRoutingFailureClass::GraphConstruction,
            "WORKFLOW_GRAPH_REVISION_NOT_FOUND",
            "workflow graph revision not found");
    }

    auto execution_graph = query_service_->GetWorkflowGraph(snapshot.workflow_instance_id);
    if (!execution_graph.has_value()) {
        return fail(WorkflowGraphRoutingFailureClass::GraphConstruction,
            "WORKFLOW_EXECUTION_GRAPH_NOT_FOUND",
            "workflow execution graph not found");
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
            if (!IsSettledActivation(activation_it->state)) {
                if (result_out) {
                    *result_out = result;
                }
                return true;
            }
        }
    }
    std::vector<std::int64_t> output_step_ids{snapshot.workflow_step_id};
    if (snapshot.workflow_unit_activation_id) {
        output_step_ids.clear();
        for (const auto& step : execution_graph->steps) {
            if (step.workflow_unit_activation_id
                == snapshot.workflow_unit_activation_id)
                output_step_ids.push_back(step.workflow_step_id);
        }
    }
    const auto* source_node = FindNode(*authored_graph, graph_node_key);
    if (source_node == nullptr) {
        return fail(WorkflowGraphRoutingFailureClass::GraphConstruction,
            "UNKNOWN_SETTLEMENT_GRAPH_NODE",
            "workflow settlement references an unknown graph node");
    }
    const auto registry = BuildDefaultWorkflowUnitRegistry();
    std::string preflight_code;
    std::string preflight_error;
    if (!ValidateAuthoredGraphDefinition(
            *authored_graph, registry, &preflight_code, &preflight_error)) {
        return fail(WorkflowGraphRoutingFailureClass::GraphConstruction,
            std::move(preflight_code), std::move(preflight_error));
    }
    const auto* source_unit = registry.Find(source_node->unit_kind);
    if (source_unit == nullptr) {
        return fail(WorkflowGraphRoutingFailureClass::GraphConstruction,
            "UNREGISTERED_SOURCE_WORKFLOW_UNIT",
            "workflow graph node references an unregistered unit");
    }
    for (const auto workflow_step_id : output_step_ids) {
        for (const auto& job_output :
             execution_db_->ListJobOutputsForWorkflowStep(workflow_step_id)) {
            const auto* declared = FindDeclaredOutput(
                *source_node, job_output.output_key);
            if (declared == nullptr
                || declared->data_kind != job_output.data_kind
                || declared->ref_kind != job_output.ref_kind) {
                return fail(WorkflowGraphRoutingFailureClass::GraphConstruction,
                    "PROGRAM_OUTPUT_OUTSIDE_UNIT_CONTRACT",
                    "program output is outside the workflow unit contract: "
                        + graph_node_key + "." + job_output.output_key);
            }
        }
    }
    for (const auto& mapping : source_unit->pass_through_outputs) {
        const auto binding = std::find_if(
            execution_graph->input_bindings.begin(),
            execution_graph->input_bindings.end(),
            [&](const auto& value) {
                return value.node_key == graph_node_key
                    && value.input_key == mapping.input_key;
            });
        const auto* declared = FindDeclaredOutput(
            *source_node, mapping.output_key);
        if (binding == execution_graph->input_bindings.end()
            || declared == nullptr
            || binding->data_kind != declared->data_kind
            || binding->ref_kind != declared->ref_kind) {
            return fail(WorkflowGraphRoutingFailureClass::GraphConstruction,
                "UNRESOLVED_WORKFLOW_PASS_THROUGH",
                "workflow pass-through mapping could not be resolved: "
                    + graph_node_key + "." + mapping.output_key);
        }
    }
    for (const auto workflow_step_id : output_step_ids) {
        const auto job_outputs =
            execution_db_->ListJobOutputsForWorkflowStep(workflow_step_id);
        for (const auto& job_output : job_outputs) {
            const auto* declared = FindDeclaredOutput(
                *source_node, job_output.output_key);
            if (declared == nullptr ||
                declared->data_kind != job_output.data_kind ||
                declared->ref_kind != job_output.ref_kind) {
                return fail(WorkflowGraphRoutingFailureClass::GraphConstruction,
                    "PROGRAM_OUTPUT_OUTSIDE_UNIT_CONTRACT",
                    "program output is outside the workflow unit contract: "
                        + graph_node_key + "." + job_output.output_key);
            }
            std::string command_error;
            if (!command_service_->RecordStepOutput(
                    {
                        .workflow_step_id = workflow_step_id,
                        .output_key = job_output.output_key,
                        .output_data_kind = job_output.data_kind,
                        .output_ref_kind = job_output.ref_kind,
                        .output_ref_id = job_output.ref_id,
                        .requested_by = "workflow_graph_routing",
                    },
                    &command_error)) {
                return fail(WorkflowGraphRoutingFailureClass::Operation,
                    "RECORD_STEP_OUTPUT_FAILED", command_error);
            }
        }
    }

    for (const auto& mapping : source_unit->pass_through_outputs) {
        const auto binding = std::find_if(
            execution_graph->input_bindings.begin(),
            execution_graph->input_bindings.end(),
            [&](const auto& value) {
                return value.node_key == graph_node_key &&
                    value.input_key == mapping.input_key;
            });
        const auto* declared = FindDeclaredOutput(
            *source_node, mapping.output_key);
        if (binding == execution_graph->input_bindings.end() ||
            declared == nullptr || binding->data_kind != declared->data_kind ||
            binding->ref_kind != declared->ref_kind) {
            return fail(WorkflowGraphRoutingFailureClass::GraphConstruction,
                "UNRESOLVED_WORKFLOW_PASS_THROUGH",
                "workflow pass-through mapping could not be resolved: "
                    + graph_node_key + "." + mapping.output_key);
        }
        std::string command_error;
        if (!command_service_->RecordStepOutput({
                .workflow_step_id = snapshot.workflow_step_id,
                .output_key = mapping.output_key,
                .output_data_kind = binding->data_kind,
                .output_ref_kind = binding->ref_kind,
                .output_ref_id = binding->ref_id,
                .requested_by = "workflow_graph_pass_through",
            }, &command_error)) {
            return fail(WorkflowGraphRoutingFailureClass::Operation,
                "RECORD_PASS_THROUGH_OUTPUT_FAILED", command_error);
        }
    }

    std::deque<std::string> terminal_sources;
    terminal_sources.push_back(graph_node_key);
    std::unordered_set<std::string> evaluated_sources;
    while (!terminal_sources.empty()) {
        const std::string source_node_key =
            std::move(terminal_sources.front());
        terminal_sources.pop_front();
        if (!evaluated_sources.emplace(source_node_key).second) {
            continue;
        }

        execution_graph = query_service_->GetWorkflowGraph(
            snapshot.workflow_instance_id);
        if (!execution_graph) {
            return fail(WorkflowGraphRoutingFailureClass::GraphConstruction,
                "WORKFLOW_EXECUTION_GRAPH_DISAPPEARED",
                "workflow execution graph disappeared during routing");
        }
        const auto outputs = query_service_->ListStepOutputs(
            snapshot.workflow_instance_id);
        std::unordered_map<
            std::string,
            const WorkflowInstanceInputBindingRecord*> binding_by_key;
        for (const auto& binding : execution_graph->input_bindings) {
            binding_by_key.emplace(
                BindingKey(binding.node_key, binding.input_key),
                &binding);
        }

        std::vector<std::string> touched_targets;
        for (const auto& edge : authored_graph->edges) {
            if (edge.from_node_key != source_node_key) {
                continue;
            }
            if (std::find(
                    touched_targets.begin(),
                    touched_targets.end(),
                    edge.to_node_key) == touched_targets.end()) {
                touched_targets.push_back(edge.to_node_key);
            }
        }

        for (const auto& target_node_key : touched_targets) {
            const auto* target_node = FindNode(
                *authored_graph, target_node_key);
            if (target_node == nullptr) {
                return fail(WorkflowGraphRoutingFailureClass::GraphConstruction,
                    "UNKNOWN_WORKFLOW_EDGE_TARGET",
                    "workflow graph edge references unknown target node");
            }

            bool all_required_satisfied = true;
            bool should_wait = false;
            std::optional<std::string> skip_reason;
            for (const auto& input : target_node->inputs) {
                if (!input.required) {
                    continue;
                }
                const auto input_key = BindingKey(
                    target_node_key, input.input_key);
                const auto existing_binding = binding_by_key.find(input_key);
                if (existing_binding != binding_by_key.end()) {
                    continue;
                }

                bool has_provider = false;
                bool provider_active = false;
                bool guarded_provider_absent = false;
                std::optional<std::string> unguarded_missing;
                for (const auto& edge : authored_graph->edges) {
                    if (edge.edge_kind != "DATA"
                        || edge.to_node_key != target_node_key
                        || edge.input_key != input.input_key) {
                        continue;
                    }
                    has_provider = true;
                    if ((edge.guard_kind.has_value()
                            && *edge.guard_kind
                                != savor::db::kWorkflowOutputPresentGuard)
                        || edge.guard_value.has_value()) {
                        return fail(WorkflowGraphRoutingFailureClass::GraphConstruction,
                            "UNSUPPORTED_WORKFLOW_EDGE_GUARD",
                            "workflow graph contains an unsupported edge guard");
                    }

                    const auto* output = FindOutput(
                        outputs,
                        edge.from_node_key,
                        edge.output_key);
                    if (output != nullptr) {
                        if (!input.data_kind.empty()
                            && input.data_kind != output->data_kind) {
                            return fail(WorkflowGraphRoutingFailureClass::GraphConstruction,
                                "WORKFLOW_EDGE_DATA_KIND_MISMATCH",
                                "workflow graph edge data_kind mismatch");
                        }
                        std::string command_error;
                        if (!command_service_->RecordInputBinding(
                                {
                                    .workflow_instance_id =
                                        snapshot.workflow_instance_id,
                                    .workflow_graph_revision_id =
                                        *snapshot.workflow_graph_revision_id,
                                    .node_key = target_node_key,
                                    .input_key = input.input_key,
                                    .data_kind = output->data_kind,
                                    .ref_kind = output->ref_kind,
                                    .ref_id = output->ref_id,
                                    .source_kind = "upstream",
                                    .requested_by =
                                        "workflow_graph_routing",
                                },
                                &command_error)) {
                            return fail(WorkflowGraphRoutingFailureClass::Operation,
                                "RECORD_INPUT_BINDING_FAILED", command_error);
                        }
                        result.routed_input_binding = true;
                        binding_by_key.emplace(input_key, nullptr);
                        break;
                    }

                    if (HasActiveWorkForGraphNode(
                            *execution_graph, edge.from_node_key)) {
                        provider_active = true;
                        continue;
                    }
                    if (edge.guard_kind.has_value()) {
                        guarded_provider_absent = true;
                    } else {
                        unguarded_missing =
                            "graph_output_missing:" + edge.from_node_key
                            + "." + edge.output_key;
                    }
                }

                if (binding_by_key.contains(input_key)) {
                    continue;
                }
                all_required_satisfied = false;
                if (provider_active) {
                    should_wait = true;
                    continue;
                }
                if (unguarded_missing) {
                    result.blocked_reason = *unguarded_missing;
                    if (result_out) *result_out = result;
                    return true;
                }
                if (has_provider && guarded_provider_absent) {
                    const auto absent_edge = std::find_if(
                        authored_graph->edges.begin(),
                        authored_graph->edges.end(),
                        [&](const auto& edge) {
                            return edge.edge_kind == "DATA"
                                && edge.to_node_key == target_node_key
                                && edge.input_key == input.input_key
                                && edge.guard_kind.has_value();
                        });
                    skip_reason = "guard_not_satisfied:"
                        + absent_edge->from_node_key + "."
                        + absent_edge->output_key;
                } else if (!has_provider) {
                    result.blocked_reason =
                        "graph_input_unbound:" + target_node_key + "."
                        + input.input_key;
                    if (result_out) *result_out = result;
                    return true;
                }
            }

            if (skip_reason && !should_wait) {
                bool skipped_target = false;
                for (const auto& step : execution_graph->steps) {
                    const auto step_node_key = step.graph_node_key.empty()
                        ? step.step_key
                        : step.graph_node_key;
                    if (step_node_key != target_node_key
                        || step.state == WorkflowStepState::Skipped
                        || IsSettled(step.state)) {
                        continue;
                    }
                    if (step.state != WorkflowStepState::Waiting
                        && step.state != WorkflowStepState::Ready) {
                        return fail(WorkflowGraphRoutingFailureClass::GraphConstruction,
                            "GUARDED_TARGET_ACTIVE_WITHOUT_OUTPUT",
                            "guarded workflow target became active before its required output was available");
                    }
                    std::string command_error;
                    if (!command_service_->SkipStep(
                            {
                                .workflow_step_id = step.workflow_step_id,
                                .reason = *skip_reason,
                                .requested_by = "workflow_graph_routing",
                            },
                            &command_error)) {
                        return fail(WorkflowGraphRoutingFailureClass::Operation,
                            "SKIP_WORKFLOW_STEP_FAILED", command_error);
                    }
                    ++result.skipped_step_count;
                    skipped_target = true;
                }
                if (skipped_target) {
                    terminal_sources.push_back(target_node_key);
                }
                continue;
            }
            if (!all_required_satisfied || should_wait) {
                continue;
            }

            const auto target_step = std::find_if(
                execution_graph->steps.begin(),
                execution_graph->steps.end(),
                [&](const auto& step) {
                    const auto step_node_key = step.graph_node_key.empty()
                        ? step.step_key
                        : step.graph_node_key;
                    return step_node_key == target_node_key;
                });
            if (target_step == execution_graph->steps.end()
                || target_step->state != WorkflowStepState::Waiting) {
                continue;
            }
            std::string command_error;
            if (!command_service_->MarkStepReady(
                    {
                        .workflow_instance_id = snapshot.workflow_instance_id,
                        .step_key = target_step->step_key,
                        .requested_by = "workflow_graph_routing",
                        .ready_priority =
                            snapshot.priority
                            + successor_step_priority_boost_,
                    },
                    &command_error)) {
                return fail(WorkflowGraphRoutingFailureClass::Operation,
                    "MARK_WORKFLOW_STEP_READY_FAILED", command_error);
            }
            result.advanced_ready_step = true;
        }
    }

    execution_graph = query_service_->GetWorkflowGraph(snapshot.workflow_instance_id);
    if (execution_graph.has_value()
        && AllExecutionStepsSuccessful(*execution_graph)
        && !result.advanced_ready_step) {
        std::string command_error;
        if (!command_service_->CompleteWorkflowInstance(
                {
                    .workflow_instance_id = snapshot.workflow_instance_id,
                    .requested_by = "workflow_graph_routing",
                },
                &command_error)) {
            if (command_error != "complete precondition failed") {
                return fail(WorkflowGraphRoutingFailureClass::Operation,
                    "COMPLETE_WORKFLOW_INSTANCE_FAILED", command_error);
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
