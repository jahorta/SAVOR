#pragma once

#include <cstdint>
#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "SimCoreDbRuntime.h"
#include "DB/SimCoreDbAuthoringService.h"
#include "Execution/Workflow/WorkflowComposition.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "UIRead/IUiReadDb.h"

namespace soasimqt2::db {

struct WorkflowListRequest {
    std::string state;
    std::string workflow_kind;
    std::optional<simcore::db::UiReadListCursor> before;
    std::optional<simcore::db::UiReadListCursor> after;
    int limit = 50;
};

struct WorkflowGraphInputBindingDraft {
    std::string node_key;
    std::string input_key;
    std::string data_kind;
    std::string ref_kind;
    std::int64_t ref_id = 0;
    std::string source_kind = "external";
};

struct WorkflowGraphArgumentDraft {
    std::string node_key;
    std::string argument_key;
    std::string value_type = "text";
    std::optional<std::int64_t> integer_value;
    std::optional<std::string> text_value;
    std::string source_kind = "launcher";
};

struct WorkflowGraphStartRequest {
    std::int64_t workflow_graph_revision_id = 0;
    std::string root_scope_kind = "manual";
    std::optional<std::int64_t> root_scope_id;
    std::string created_by = "SoaSimQt2";
    std::vector<WorkflowGraphInputBindingDraft> input_bindings;
    std::vector<WorkflowGraphArgumentDraft> arguments;
};

class SimCoreDbWorkflowService {
public:
    using WorkflowUnitDefinition = simcore::db::execution::workflow::WorkflowUnitDefinition;
    using WorkflowCompositionSpec = simcore::db::execution::workflow::WorkflowCompositionSpec;
    using WorkflowCompositionPreview = simcore::db::execution::workflow::WorkflowCompositionPreview;

    static ServiceResult<std::vector<WorkflowUnitDefinition>> ListWorkflowUnits(bool include_hidden = false) {
        const auto registry = simcore::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
        auto units = registry.ListUnits();
        if (!include_hidden) {
            units.erase(
                std::remove_if(units.begin(), units.end(), [](const auto& unit) { return unit.hidden; }),
                units.end());
        }
        return ServiceResult<std::vector<WorkflowUnitDefinition>>::Ok(std::move(units));
    }

    static ServiceResult<WorkflowCompositionPreview> PreviewComposition(
        const WorkflowCompositionSpec& composition) {
        const auto registry = simcore::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
        const simcore::db::execution::workflow::WorkflowCompositionService service(&registry);
        return ServiceResult<WorkflowCompositionPreview>::Ok(service.Preview(composition));
    }

    static ServiceResult<simcore::db::UiReadPage<simcore::db::UiWorkflowInstanceSummary>> ListWorkflowInstances(
        const WorkflowListRequest& request) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<simcore::db::UiReadPage<simcore::db::UiWorkflowInstanceSummary>>("legacy SimCore/DB path is temporarily unavailable in this Qt2 migration slice");
        }

        simcore::db::UiWorkflowInstanceListQuery query{};
        query.state = request.state;
        query.workflow_kind = request.workflow_kind;
        query.before = request.before;
        query.after = request.after;
        query.limit = request.limit;
        return ServiceResult<simcore::db::UiReadPage<simcore::db::UiWorkflowInstanceSummary>>::Ok(
            db->ListWorkflowInstances(query));
    }

    static ServiceResult<simcore::db::UiWorkflowDetail> GetWorkflowDetail(std::int64_t workflow_instance_id) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<simcore::db::UiWorkflowDetail>("legacy SimCore/DB path is temporarily unavailable in this Qt2 migration slice");
        }
        const auto detail = db->GetWorkflowDetail(workflow_instance_id);
        if (!detail.has_value()) {
            return NotFound<simcore::db::UiWorkflowDetail>("workflow instance not found");
        }
        return ServiceResult<simcore::db::UiWorkflowDetail>::Ok(*detail);
    }

    static ServiceResult<std::int64_t> StartWorkflowGraphRevision(const WorkflowGraphStartRequest& request) {
        auto* command_service = WorkflowCommandService();
        if (command_service == nullptr) {
            return Unavailable<std::int64_t>("legacy SimCore/DB path is temporarily unavailable in this Qt2 migration slice");
        }
        if (request.workflow_graph_revision_id <= 0) {
            return Invalid<std::int64_t>("workflow graph revision id is required");
        }

        const auto graph_result = SimCoreDbAuthoringService::GetWorkflowGraphRevision(request.workflow_graph_revision_id);
        if (!graph_result.ok) {
            return ServiceResult<std::int64_t>::Err(graph_result.error);
        }
        const auto& graph = graph_result.value;
        if (graph.nodes.empty()) {
            return Invalid<std::int64_t>("workflow graph revision has no nodes");
        }

        std::unordered_map<std::string, const simcore::db::WorkflowGraphNodeSnapshot*> node_by_key;
        node_by_key.reserve(graph.nodes.size());
        for (const auto& node : graph.nodes) {
            if (node.node_key.empty() || node.unit_kind.empty()) {
                return Invalid<std::int64_t>("workflow graph contains a node without a key or unit kind");
            }
            if (!node_by_key.emplace(node.node_key, &node).second) {
                return Invalid<std::int64_t>("workflow graph contains duplicate node keys");
            }
        }

        std::unordered_map<std::string, std::vector<std::string>> dependencies_by_node;
        std::unordered_set<std::string> supplied_by_edge;
        for (const auto& edge : graph.edges) {
            if (node_by_key.find(edge.from_node_key) == node_by_key.end()
                || node_by_key.find(edge.to_node_key) == node_by_key.end()) {
                return Invalid<std::int64_t>("workflow graph contains an edge with an unknown node");
            }
            dependencies_by_node[edge.to_node_key].push_back(edge.from_node_key);
            supplied_by_edge.insert(edge.to_node_key + "\n" + edge.input_key);
        }

        std::unordered_set<std::string> supplied_by_binding;
        supplied_by_binding.reserve(request.input_bindings.size());
        simcore::db::execution::workflow::WorkflowCreateInstanceCommand command{};
        command.workflow_kind = "workflow_graph";
        command.root_scope_kind = request.root_scope_kind.empty() ? "manual" : request.root_scope_kind;
        command.root_scope_id = request.root_scope_id;
        command.workflow_graph_revision_id = graph.workflow_graph_revision_id;
        command.created_by = request.created_by.empty() ? "SoaSimQt2" : request.created_by;
        command.created_at_utc = simcore::db::types::UtcNow().time_since_epoch().count();

        for (const auto& binding : request.input_bindings) {
            if (node_by_key.find(binding.node_key) == node_by_key.end()) {
                return Invalid<std::int64_t>("input binding references an unknown workflow node");
            }
            if (binding.input_key.empty() || binding.data_kind.empty() || binding.ref_kind.empty() || binding.ref_id <= 0) {
                return Invalid<std::int64_t>("input binding requires input key, data kind, ref kind, and ref id");
            }
            const auto key = binding.node_key + "\n" + binding.input_key;
            if (!supplied_by_binding.emplace(key).second) {
                return Invalid<std::int64_t>("duplicate input binding for " + binding.node_key + "." + binding.input_key);
            }
            command.input_bindings.push_back(simcore::db::execution::workflow::WorkflowCreateInstanceInputBindingSpec{
                .node_key = binding.node_key,
                .input_key = binding.input_key,
                .data_kind = binding.data_kind,
                .ref_kind = binding.ref_kind,
                .ref_id = binding.ref_id,
                .source_kind = binding.source_kind.empty() ? "external" : binding.source_kind,
            });
        }

        std::unordered_set<std::string> supplied_arguments;
        supplied_arguments.reserve(request.arguments.size());
        for (const auto& argument : request.arguments) {
            if (!argument.node_key.empty() && node_by_key.find(argument.node_key) == node_by_key.end()) {
                return Invalid<std::int64_t>("workflow argument references an unknown workflow node");
            }
            if (argument.argument_key.empty()) {
                return Invalid<std::int64_t>("workflow argument key is required");
            }
            if (argument.value_type != "integer"
                && argument.value_type != "text"
                && argument.value_type != "json"
                && argument.value_type != "boolean") {
                return Invalid<std::int64_t>("workflow argument value type must be integer, text, json, or boolean");
            }
            const auto key = argument.node_key + "\n" + argument.argument_key;
            if (!supplied_arguments.emplace(key).second) {
                return Invalid<std::int64_t>("duplicate workflow argument for " + argument.node_key + "." + argument.argument_key);
            }
            command.arguments.push_back(simcore::db::execution::workflow::WorkflowCreateInstanceArgumentSpec{
                .node_key = argument.node_key,
                .argument_key = argument.argument_key,
                .value_type = argument.value_type,
                .integer_value = argument.integer_value,
                .text_value = argument.text_value,
                .source_kind = argument.source_kind.empty() ? "launcher" : argument.source_kind,
            });
        }

        for (const auto& node : graph.nodes) {
            for (const auto& input : node.inputs) {
                if (!input.required) {
                    continue;
                }
                const auto key = node.node_key + "\n" + input.input_key;
                if (supplied_by_edge.find(key) == supplied_by_edge.end()
                    && supplied_by_binding.find(key) == supplied_by_binding.end()) {
                    return Invalid<std::int64_t>("required input is not supplied: " + node.node_key + "." + input.input_key);
                }
            }

            simcore::db::execution::workflow::WorkflowCreateStepSpec step{};
            step.step_key = node.node_key;
            step.step_kind = node.unit_kind;
            const auto deps = dependencies_by_node.find(node.node_key);
            if (deps != dependencies_by_node.end()) {
                step.dependencies = deps->second;
            }
            step.max_attempts = 1;
            command.steps.push_back(std::move(step));
        }

        std::int64_t workflow_instance_id = 0;
        std::string error;
        if (!command_service->CreateWorkflowInstance(command, &workflow_instance_id, &error)) {
            return Failed<std::int64_t>(error);
        }
        return ServiceResult<std::int64_t>::Ok(workflow_instance_id);
    }

    static ServiceResult<void> CancelWorkflow(std::int64_t workflow_instance_id, std::string reason) {
        auto* command_service = WorkflowCommandService();
        if (command_service == nullptr) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Unavailable, "legacy SimCore/DB path is temporarily unavailable in this Qt2 migration slice" });
        }
        simcore::db::execution::workflow::WorkflowCancelInstanceCommand command{};
        command.workflow_instance_id = workflow_instance_id;
        command.reason = std::move(reason);
        command.requested_by = "SoaSimQt2";

        std::string error;
        if (!command_service->CancelWorkflowInstance(command, &error)) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Failed, std::move(error) });
        }
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<void> ResumeWorkflow(std::int64_t workflow_instance_id) {
        auto* command_service = WorkflowCommandService();
        if (command_service == nullptr) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Unavailable, "legacy SimCore/DB path is temporarily unavailable in this Qt2 migration slice" });
        }
        simcore::db::execution::workflow::WorkflowResumeInstanceCommand command{};
        command.workflow_instance_id = workflow_instance_id;
        command.requested_by = "SoaSimQt2";

        std::string error;
        if (!command_service->ResumeWorkflowInstance(command, &error)) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Failed, std::move(error) });
        }
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<void> RetryStep(std::int64_t workflow_step_id) {
        auto* command_service = WorkflowCommandService();
        if (command_service == nullptr) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Unavailable, "legacy SimCore/DB path is temporarily unavailable in this Qt2 migration slice" });
        }
        simcore::db::execution::workflow::WorkflowRetryStepCommand command{};
        command.workflow_step_id = workflow_step_id;
        command.requested_by = "SoaSimQt2";

        std::string error;
        if (!command_service->RetryFailedStep(command, &error)) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Failed, std::move(error) });
        }
        return ServiceResult<void>::Ok();
    }

private:
    static simcore::db::IUiReadDb* UiReadDb() {
        return soasimqt2::SimCoreDbRuntime::instance().uiReadDb();
    }

    static simcore::db::execution::workflow::IWorkflowOrchestrationCommandService* WorkflowCommandService() {
        return soasimqt2::SimCoreDbRuntime::instance().workflowCommandService();
    }

    template <typename T>
    static ServiceResult<T> Unavailable(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::Unavailable, std::move(message) });
    }

    template <typename T>
    static ServiceResult<T> NotFound(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::NotFound, std::move(message) });
    }

    template <typename T>
    static ServiceResult<T> Invalid(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::InvalidInput, std::move(message) });
    }

    template <typename T>
    static ServiceResult<T> Failed(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::Failed, std::move(message) });
    }
};

} // namespace soasimqt2::db

