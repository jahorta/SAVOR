#pragma once

#include <cstdint>
#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "SavorDbRuntime.h"
#include "Analysis/IAnalysisDb.h"
#include "Authoring/AuthoringContentHash.h"
#include "DB/SavorDbAuthoringService.h"
#include "Execution/Workflow/WorkflowComposition.h"
#include "Execution/Workflow/WorkflowLaunchContract.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Execution/Workflow/WorkflowUnitActivationFactory.h"
#include "Execution/WorksetJobOrganizer.h"
#include "UIRead/IUiReadDb.h"

namespace savorqt::db {

struct WorkflowListRequest {
    std::string state;
    std::string display_state;
    std::string workflow_kind;
    bool exclude_final = false;
    std::optional<savor::db::UiReadListCursor> before;
    std::optional<savor::db::UiReadListCursor> after;
    int limit = 50;
    bool battle_final_victory_only = false;
    bool battle_final_victory_absent_only = false;
    std::optional<std::int64_t> workflow_instance_id;
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
    std::string created_by = "SavorQt";
    std::optional<std::string> launch_key;
    std::vector<WorkflowGraphInputBindingDraft> input_bindings;
    std::vector<WorkflowGraphArgumentDraft> arguments;
};

struct StandaloneWorkflowUnitGraphRequest {
    std::string unit_kind;
    std::optional<std::string> authored_ref_kind;
    std::optional<std::int64_t> authored_ref_id;
    bool hidden = true;
};

struct RecordBattleVictoryResult {
    std::int64_t workflow_instance_id = 0;
    bool recording_workflow_created = false;
    bool focused_existing_completion = false;
};

using WorkflowStandaloneLaunchEntry =
    savor::db::execution::workflow::WorkflowStandalonePresentationEntry;

class SavorDbWorkflowService {
public:
    using WorkflowUnitDefinition = savor::db::execution::workflow::WorkflowUnitDefinition;
    using WorkflowCompositionSpec = savor::db::execution::workflow::WorkflowCompositionSpec;
    using WorkflowCompositionPreview = savor::db::execution::workflow::WorkflowCompositionPreview;

    static ServiceResult<std::vector<WorkflowUnitDefinition>> ListComposableWorkflowUnits(bool include_hidden = false) {
        const auto registry = savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
        auto units = registry.ListUnits();
        FilterUnavailableProductionUnits(units);
        if (!include_hidden) {
            units.erase(
                std::remove_if(units.begin(), units.end(), [](const auto& unit) { return unit.hidden; }),
                units.end());
        }
        return ServiceResult<std::vector<WorkflowUnitDefinition>>::Ok(std::move(units));
    }

    static ServiceResult<std::vector<WorkflowStandaloneLaunchEntry>> ListStandaloneLaunchEntries() {
        const auto registry = savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
        auto units = registry.ListUnits();
        FilterUnavailableProductionUnits(units);

        auto entries = savor::db::execution::workflow::
            BuildStandalonePresentationEntries(std::move(units));
        return ServiceResult<std::vector<WorkflowStandaloneLaunchEntry>>::Ok(
            std::move(entries));
    }

    static ServiceResult<WorkflowCompositionPreview> PreviewComposition(
        const WorkflowCompositionSpec& composition) {
        const auto registry = savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
        const savor::db::execution::workflow::WorkflowCompositionService service(&registry);
        return ServiceResult<WorkflowCompositionPreview>::Ok(service.Preview(composition));
    }

    static ServiceResult<savor::db::SaveWorkflowGraphResult> EnsureStandaloneWorkflowUnitGraph(
        const StandaloneWorkflowUnitGraphRequest& request) {
        if (request.unit_kind.empty()) {
            return Invalid<savor::db::SaveWorkflowGraphResult>("workflow unit kind is required");
        }

        const auto registry = savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
        const auto* unit = registry.Find(request.unit_kind);
        if (unit == nullptr || unit->hidden || !unit->standalone_launchable) {
            return NotFound<savor::db::SaveWorkflowGraphResult>("workflow unit not found");
        }
        if (unit->authored_refs.size() > 1) {
            return Invalid<savor::db::SaveWorkflowGraphResult>("standalone launch currently supports one authored settings reference per workflow unit");
        }
        if (!unit->authored_refs.empty() && unit->authored_refs.front().required) {
            if (!request.authored_ref_kind.has_value()
                || !request.authored_ref_id.has_value()
                || request.authored_ref_kind->empty()
                || *request.authored_ref_id <= 0) {
                return Invalid<savor::db::SaveWorkflowGraphResult>(
                    "workflow unit requires authored settings: " + unit->authored_refs.front().ref_kind);
            }
            if (*request.authored_ref_kind != unit->authored_refs.front().ref_kind) {
                return Invalid<savor::db::SaveWorkflowGraphResult>(
                    "authored settings kind must be " + unit->authored_refs.front().ref_kind);
            }
        }

        const auto graphHash = StandaloneWorkflowGraphHash(*unit, request.authored_ref_kind, request.authored_ref_id);
        const auto existing = SavorDbAuthoringService::ListWorkflowGraphs(1000, true);
        if (!existing.ok) {
            return ServiceResult<savor::db::SaveWorkflowGraphResult>::Err(existing.error);
        }
        for (const auto& graph : existing.value) {
            if (graph.graph_hash == graphHash) {
                return ServiceResult<savor::db::SaveWorkflowGraphResult>::Ok(savor::db::SaveWorkflowGraphResult{
                    .workflow_graph_id = graph.workflow_graph_id,
                    .workflow_graph_revision_id = graph.workflow_graph_revision_id,
                });
            }
        }

        savor::db::SaveWorkflowGraphNodeCommand node{};
        node.node_key = StandaloneNodeKey(*unit);
        node.unit_kind = unit->unit_kind;
        node.display_name = unit->display_name;
        node.authored_ref_kind = request.authored_ref_kind;
        node.authored_ref_id = request.authored_ref_id;
        for (const auto& input : unit->required_inputs) {
            node.inputs.push_back(savor::db::SaveWorkflowGraphNodeInputCommand{
                .input_key = input.key,
                .data_kind = input.data_kind,
                .ref_kind = input.ref_kind,
                .display_name = input.display_name,
                .required = input.required,
            });
        }
        for (const auto& output : unit->possible_outputs) {
            node.possible_outputs.push_back(savor::db::SaveWorkflowGraphNodeOutputCommand{
                .output_key = output.key,
                .data_kind = output.data_kind,
                .ref_kind = output.ref_kind,
                .display_name = output.display_name,
            });
        }
        for (const auto& argument : unit->launch_arguments) {
            const auto value_type = [&]() -> std::string {
                using Type = savor::db::execution::workflow::WorkflowLaunchArgumentValueType;
                switch (argument.value_type) {
                case Type::Integer: return "integer";
                case Type::Text: return "text";
                case Type::Boolean: return "boolean";
                case Type::Json: return "json";
                case Type::Choice: return "choice";
                }
                return {};
            }();
            savor::db::SaveWorkflowGraphNodeArgumentCommand argument_command{
                .argument_key = argument.key,
                .display_name = argument.display_name,
                .value_type = value_type,
                .required = argument.required,
                .default_value = argument.default_value,
                .minimum_integer = argument.minimum_integer,
                .maximum_integer = argument.maximum_integer,
            };
            for (const auto& choice : argument.choices) {
                argument_command.choices.push_back({
                    .value = choice.value,
                    .display_name = choice.display_name,
                });
            }
            node.arguments.push_back(std::move(argument_command));
        }
        for (const auto& constraint : unit->launch_argument_constraints) {
            node.argument_constraints.push_back(savor::db::SaveWorkflowGraphNodeArgumentConstraintCommand{
                .lesser_or_equal_key = constraint.lesser_or_equal_key,
                .greater_or_equal_key = constraint.greater_or_equal_key,
                .message = constraint.message,
            });
        }

        WorkflowGraphDraft draft{};
        draft.name = StandaloneWorkflowGraphName(*unit, request.authored_ref_kind, request.authored_ref_id);
        draft.description = "Auto-authored single-unit workflow graph for standalone Qt2 launches.";
        draft.hidden = request.hidden;
        draft.graph_version = 1;
        draft.graph_hash = graphHash;
        draft.nodes.push_back(std::move(node));
        return SavorDbAuthoringService::SaveWorkflowGraph(draft);
    }

    static ServiceResult<savor::db::UiReadPage<savor::db::UiWorkflowInstanceSummary>> ListWorkflowInstances(
        const WorkflowListRequest& request) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<savor::db::UiReadPage<savor::db::UiWorkflowInstanceSummary>>(kSavorDbRuntimeUnavailableMessage);
        }

        savor::db::UiWorkflowInstanceListQuery query{};
        query.state = request.state;
        query.display_state = request.display_state;
        query.workflow_kind = request.workflow_kind;
        query.exclude_final = request.exclude_final;
        query.before = request.before;
        query.after = request.after;
        query.limit = request.limit;
        query.battle_final_victory_only = request.battle_final_victory_only;
        query.battle_final_victory_absent_only = request.battle_final_victory_absent_only;
        query.workflow_instance_id = request.workflow_instance_id;
        return ServiceResult<savor::db::UiReadPage<savor::db::UiWorkflowInstanceSummary>>::Ok(
            db->ListWorkflowInstances(query));
    }

    static ServiceResult<savor::db::UiWorkflowDisplayStateCounts> CountWorkflowDisplayStates() {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<savor::db::UiWorkflowDisplayStateCounts>(kSavorDbRuntimeUnavailableMessage);
        }
        return ServiceResult<savor::db::UiWorkflowDisplayStateCounts>::Ok(
            db->CountWorkflowDisplayStates());
    }

    static ServiceResult<savor::db::UiWorkflowDetail> GetWorkflowDetail(std::int64_t workflow_instance_id) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<savor::db::UiWorkflowDetail>(kSavorDbRuntimeUnavailableMessage);
        }
        const auto detail = db->GetWorkflowDetail(workflow_instance_id);
        if (!detail.has_value()) {
            return NotFound<savor::db::UiWorkflowDetail>("workflow instance not found");
        }
        return ServiceResult<savor::db::UiWorkflowDetail>::Ok(*detail);
    }

    static ServiceResult<std::int64_t> StartWorkflowGraphRevision(const WorkflowGraphStartRequest& request) {
        auto* command_service = WorkflowCommandService();
        if (command_service == nullptr) {
            return Unavailable<std::int64_t>(kSavorDbRuntimeUnavailableMessage);
        }
        if (request.workflow_graph_revision_id <= 0) {
            return Invalid<std::int64_t>("workflow graph revision id is required");
        }

        const auto graph_result = SavorDbAuthoringService::GetWorkflowGraphRevision(request.workflow_graph_revision_id);
        if (!graph_result.ok) {
            return ServiceResult<std::int64_t>::Err(graph_result.error);
        }
        const auto& graph = graph_result.value;
        if (graph.nodes.empty()) {
            return Invalid<std::int64_t>("workflow graph revision has no nodes");
        }

        const auto registry = savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
        std::vector<savor::db::execution::workflow::WorkflowLaunchInputValue> contract_inputs;
        contract_inputs.reserve(request.input_bindings.size());
        for (const auto& binding : request.input_bindings) {
            contract_inputs.push_back({
                .node_key = binding.node_key,
                .input_key = binding.input_key,
                .data_kind = binding.data_kind,
                .ref_kind = binding.ref_kind,
                .ref_id = binding.ref_id,
                .source_kind = binding.source_kind,
            });
        }
        std::vector<savor::db::execution::workflow::WorkflowLaunchArgumentValue> contract_arguments;
        contract_arguments.reserve(request.arguments.size());
        for (const auto& argument : request.arguments) {
            contract_arguments.push_back({
                .node_key = argument.node_key,
                .argument_key = argument.argument_key,
                .value_type = argument.value_type,
                .integer_value = argument.integer_value,
                .text_value = argument.text_value,
                .source_kind = argument.source_kind,
            });
        }
        const auto contract = savor::db::execution::workflow::WorkflowLaunchContractValidator::Validate(
            graph, registry, contract_inputs, contract_arguments);
        if (!contract.valid) {
            return Invalid<std::int64_t>(contract.issues.empty()
                ? "workflow launch contract is invalid" : contract.issues.front());
        }

        const auto binding_key = [](const std::string& node_key, const std::string& input_key) {
            return node_key + "\n" + input_key;
        };

        std::unordered_map<std::string, const savor::db::WorkflowGraphNodeSnapshot*> node_by_key;
        std::unordered_map<std::string, const savor::db::WorkflowGraphNodeInputSnapshot*> input_by_key;
        node_by_key.reserve(graph.nodes.size());
        for (const auto& node : graph.nodes) {
            if (node.node_key.empty() || node.unit_kind.empty()) {
                return Invalid<std::int64_t>("workflow graph contains a node without a key or unit kind");
            }
            if (!node_by_key.emplace(node.node_key, &node).second) {
                return Invalid<std::int64_t>("workflow graph contains duplicate node keys");
            }
            for (const auto& input : node.inputs) {
                input_by_key.emplace(binding_key(node.node_key, input.input_key), &input);
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
            supplied_by_edge.insert(binding_key(edge.to_node_key, edge.input_key));
        }

        std::unordered_set<std::string> supplied_by_binding;
        supplied_by_binding.reserve(request.input_bindings.size());
        savor::db::execution::workflow::WorkflowCreateInstanceCommand command{};
        command.workflow_kind = "workflow_graph";
        // Root-scope fields are retained only as historical Execution DB
        // storage. Current graph launches are manual, and their exact typed
        // input bindings are the source authority.
        command.root_scope_kind = "manual";
        command.root_scope_id = std::nullopt;
        command.workflow_graph_revision_id = graph.workflow_graph_revision_id;
        command.created_by = request.created_by.empty() ? "SavorQt" : request.created_by;
        command.launch_key = request.launch_key;
        command.created_at_utc = savor::db::types::UtcNow().time_since_epoch().count();

        for (const auto& binding : request.input_bindings) {
            if (node_by_key.find(binding.node_key) == node_by_key.end()) {
                return Invalid<std::int64_t>("input binding references an unknown workflow node");
            }
            if (binding.input_key.empty() || binding.data_kind.empty() || binding.ref_kind.empty() || binding.ref_id <= 0) {
                return Invalid<std::int64_t>("input binding requires input key, data kind, ref kind, and ref id");
            }
            const auto key = binding_key(binding.node_key, binding.input_key);
            const auto input_it = input_by_key.find(key);
            if (input_it == input_by_key.end()) {
                return Invalid<std::int64_t>("input binding references an unknown workflow input");
            }
            if (input_it->second->data_kind != binding.data_kind
                || input_it->second->ref_kind != binding.ref_kind) {
                return Invalid<std::int64_t>("input binding data/ref kind does not match workflow input");
            }
            if (binding.data_kind == "analysis.input_frame_set_id"
                && binding.ref_kind != "an.input_set"
                && binding.ref_kind != "au.input_set") {
                return Invalid<std::int64_t>("analysis.input_frame_set_id binding requires an.input_set or au.input_set");
            }
            if (binding.source_kind == "external_override" && supplied_by_edge.find(key) == supplied_by_edge.end()) {
                return Invalid<std::int64_t>("external_override input binding requires an authored edge");
            }
            if (!supplied_by_binding.emplace(key).second) {
                return Invalid<std::int64_t>("duplicate input binding for " + binding.node_key + "." + binding.input_key);
            }
            command.input_bindings.push_back(savor::db::execution::workflow::WorkflowCreateInstanceInputBindingSpec{
                .node_key = binding.node_key,
                .input_key = binding.input_key,
                .data_kind = binding.data_kind,
                .ref_kind = binding.ref_kind,
                .ref_id = binding.ref_id,
                .source_kind = binding.source_kind.empty() ? "external" : binding.source_kind,
            });
        }

        std::unordered_set<std::string> supplied_arguments;
        supplied_arguments.reserve(contract.normalized_arguments.size());
        for (const auto& argument : contract.normalized_arguments) {
            if (!argument.node_key.empty() && node_by_key.find(argument.node_key) == node_by_key.end()) {
                return Invalid<std::int64_t>("workflow argument references an unknown workflow node");
            }
            if (argument.argument_key.empty()) {
                return Invalid<std::int64_t>("workflow argument key is required");
            }
            if (argument.value_type != "integer"
                && argument.value_type != "text"
                && argument.value_type != "json"
                && argument.value_type != "boolean"
                && argument.value_type != "choice") {
                return Invalid<std::int64_t>("workflow argument value type must be integer, text, json, boolean, or choice");
            }
            const auto key = argument.node_key + "\n" + argument.argument_key;
            if (!supplied_arguments.emplace(key).second) {
                return Invalid<std::int64_t>("duplicate workflow argument for " + argument.node_key + "." + argument.argument_key);
            }
            command.arguments.push_back(savor::db::execution::workflow::WorkflowCreateInstanceArgumentSpec{
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
                const auto key = binding_key(node.node_key, input.input_key);
                if (supplied_by_edge.find(key) == supplied_by_edge.end()
                    && supplied_by_binding.find(key) == supplied_by_binding.end()) {
                    return Invalid<std::int64_t>("required input is not supplied: " + node.node_key + "." + input.input_key);
                }
            }

            const auto deps = dependencies_by_node.find(node.node_key);
            std::vector<std::string> dependencies;
            if (deps != dependencies_by_node.end()) {
                dependencies = deps->second;
            }
            std::string activation_error;
            auto activation = savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
                registry,
                node.node_key,
                node.node_key,
                node.unit_kind,
                node.display_name,
                node.authored_ref_kind,
                node.authored_ref_id,
                std::move(dependencies),
                &activation_error);
            if (!activation.has_value()) {
                return Invalid<std::int64_t>(activation_error);
            }
            command.unit_activations.push_back(std::move(*activation));
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
            return ServiceResult<void>::Err({ ServiceErrorKind::Unavailable, kSavorDbRuntimeUnavailableMessage });
        }
        savor::db::execution::workflow::WorkflowCancelInstanceCommand command{};
        command.workflow_instance_id = workflow_instance_id;
        command.reason = std::move(reason);
        command.requested_by = "SavorQt";

        std::string error;
        if (!command_service->CancelWorkflowInstance(command, &error)) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Failed, std::move(error) });
        }
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<void> ResumeWorkflow(std::int64_t workflow_instance_id) {
        auto* command_service = WorkflowCommandService();
        if (command_service == nullptr) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Unavailable, kSavorDbRuntimeUnavailableMessage });
        }
        savor::db::execution::workflow::WorkflowResumeInstanceCommand command{};
        command.workflow_instance_id = workflow_instance_id;
        command.requested_by = "SavorQt";

        std::string error;
        if (!command_service->ResumeWorkflowInstance(command, &error)) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Failed, std::move(error) });
        }
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<void> RetryStep(std::int64_t workflow_step_id) {
        auto* command_service = WorkflowCommandService();
        if (command_service == nullptr) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Unavailable, kSavorDbRuntimeUnavailableMessage });
        }
        savor::db::execution::workflow::WorkflowRetryStepCommand command{};
        command.workflow_step_id = workflow_step_id;
        command.requested_by = "SavorQt";

        std::string error;
        if (!command_service->RetryFailedStep(command, &error)) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Failed, std::move(error) });
        }
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<RecordBattleVictoryResult> RecordBattleVictory(
        std::int64_t turn_job_id) {
        auto& runtime = savorqt::SavorDbRuntime::instance();
        auto* analysis = runtime.analysisDb();
        auto* execution = runtime.executionDb();
        if (!analysis || !execution)
            return Unavailable<RecordBattleVictoryResult>(
                kSavorDbRuntimeUnavailableMessage);
        const auto job = analysis->GetBattleTurnJob(turn_job_id);
        const auto wave = job ? analysis->GetBattleTurnWave(job->wave_id) : std::nullopt;
        const auto result = job && job->exec_job_id
            ? analysis->GetBattleSingleTurnResultForExecJob(*job->exec_job_id)
            : std::nullopt;
        const auto execution_job = job && job->exec_job_id
            ? execution->GetExecutionJob(*job->exec_job_id) : std::nullopt;
        if (!job || !wave || !result || !execution_job || !job->exec_job_id
            || execution_job->state != "SUCCEEDED"
            || !execution_job->worker_terminal_fingerprint
            || *execution_job->worker_terminal_fingerprint
                != result->worker_terminal_sha256
            || result->terminal_kind != "SUCCEEDED"
            || result->domain_outcome != std::optional<std::string>("Victory")
            || !result->ending_rng || !result->successor_savestate_id
            || job->output_savestate_id != result->successor_savestate_id) {
            return Invalid<RecordBattleVictoryResult>(
                "Record Victory requires a current successful durable Victory with ending RNG and successor state");
        }

        savor::db::EnsurePendingVictoryRouteBranchReceipt route_branch{};
        std::string route_error;
        if (!analysis->EnsurePendingVictoryRouteBranch({
                .battle_set_id = wave->battle_set_id,
                .selected_turn_job_id = turn_job_id,
                .default_label = "Victory RNG " + std::to_string(*result->ending_rng),
                .created_at_utc = savor::db::types::UtcNow(),
            }, &route_branch, &route_error)) {
            return Failed<RecordBattleVictoryResult>(route_error.empty()
                ? "could not create the pending TAS route branch" : route_error);
        }
        const auto bind_route_workflow = [&](std::int64_t workflow_id,
                                             std::string status)
            -> ServiceResult<void> {
            std::string error;
            if (!analysis->BindVictoryRouteBranchWorkflow({
                    .selected_turn_job_id = turn_job_id,
                    .workflow_instance_id = workflow_id,
                    .status = std::move(status),
                    .updated_at_utc = savor::db::types::UtcNow(),
                }, &error)) {
                return ServiceResult<void>::Err({ServiceErrorKind::Failed,
                    error.empty() ? "could not bind the recording workflow to its TAS route branch" : std::move(error)});
            }
            return ServiceResult<void>::Ok();
        };

        const auto completion =
            analysis->GetBattleCompletionForSelectedTurnJob(turn_job_id);
        if (completion && completion->status != "COMPLETED") {
            const auto bound = bind_route_workflow(
                completion->workflow_instance_id, "WAITING_COMPLETION");
            if (!bound.ok) return ServiceResult<RecordBattleVictoryResult>::Err(bound.error);
            return ServiceResult<RecordBattleVictoryResult>::Ok({
                .workflow_instance_id = completion->workflow_instance_id,
                .focused_existing_completion = true,
            });
        }

        if (!completion) {
            for (const auto& pool :
                 analysis->ListBattleAdvancementPoolsForBattleTurn(
                     wave->battle_set_id, wave->turn_index)) {
                if (!pool.pool_name.starts_with("victory-completion-rng-")) continue;
                for (const auto& decision :
                     analysis->ListBattleAdvancementDecisionsForPool(
                         pool.battle_advancement_pool_id)) {
                    if (decision.turn_job_id == turn_job_id
                        && decision.decision_kind
                            == savor::db::BattleAdvancementDecisionKind::
                                SelectedForCompletion) {
                        const auto workflow =
                            analysis->GetBattleWorkflowInstanceId(
                                wave->battle_set_id);
                        if (!workflow)
                            return Failed<RecordBattleVictoryResult>(
                                "automatic Victory completion lost its Battle workflow identity");
                        const auto bound = bind_route_workflow(*workflow, "WAITING_COMPLETION");
                        if (!bound.ok) return ServiceResult<RecordBattleVictoryResult>::Err(bound.error);
                        return ServiceResult<RecordBattleVictoryResult>::Ok({
                            .workflow_instance_id = *workflow,
                            .focused_existing_completion = true,
                        });
                    }
                }
            }
        }

        const bool include_completion = !completion.has_value();
        const auto graph = EnsureVictoryRecordingGraph(include_completion);
        if (!graph.ok)
            return ServiceResult<RecordBattleVictoryResult>::Err(graph.error);
        WorkflowGraphStartRequest launch{
            .workflow_graph_revision_id = graph.value.workflow_graph_revision_id,
            .created_by = "SavorQt",
            .launch_key = "battle-victory-recording:" +
                std::to_string(turn_job_id),
        };
        if (include_completion) {
            launch.input_bindings.push_back({
                .node_key = "complete",
                .input_key = "victory_turn_job",
                .data_kind = "analysis_battle.battle_turn_job",
                .ref_kind = "analysis_battle.turn_job",
                .ref_id = turn_job_id,
                .source_kind = "record-victory",
            });
        } else {
            launch.input_bindings.push_back({
                .node_key = "record",
                .input_key = "completion",
                .data_kind = "analysis_battle.battle_completion",
                .ref_kind = "analysis_battle.battle_completion",
                .ref_id = completion->battle_completion_id,
                .source_kind = "record-victory",
            });
        }
        const auto started = StartWorkflowGraphRevision(launch);
        if (!started.ok)
            return ServiceResult<RecordBattleVictoryResult>::Err(started.error);
        const auto bound = bind_route_workflow(started.value, "RECORDING");
        if (!bound.ok) return ServiceResult<RecordBattleVictoryResult>::Err(bound.error);
        return ServiceResult<RecordBattleVictoryResult>::Ok({
            .workflow_instance_id = started.value,
            .recording_workflow_created = true,
        });
    }

    static ServiceResult<savor::db::WorksetJobReorganizationReceipt>
    RetryFailedJobs(std::int64_t workflow_instance_id) {
        auto* execution_db = savorqt::SavorDbRuntime::instance().executionDb();
        if (execution_db == nullptr) {
            return Unavailable<savor::db::WorksetJobReorganizationReceipt>(
                kSavorDbRuntimeUnavailableMessage);
        }
        auto candidates =
            execution_db->ListFailedWorkflowWorksetJobs(workflow_instance_id);
        savor::runner::parallel::savordb::WorksetJobOrganizer organizer;
        savor::db::WorksetJobReorganizationPlan plan{};
        std::string error;
        if (!organizer.Organize(
                workflow_instance_id,
                std::move(candidates),
                &plan,
                &error)) {
            return ServiceResult<savor::db::WorksetJobReorganizationReceipt>::Err(
                { ServiceErrorKind::Failed, std::move(error) });
        }
        savor::db::WorksetJobReorganizationReceipt receipt{};
        if (!execution_db->ApplyWorksetJobReorganization(
                plan, &receipt, &error)) {
            return ServiceResult<savor::db::WorksetJobReorganizationReceipt>::Err(
                { ServiceErrorKind::Failed, std::move(error) });
        }
        return ServiceResult<savor::db::WorksetJobReorganizationReceipt>::Ok(
            std::move(receipt));
    }

private:
    static ServiceResult<savor::db::SaveWorkflowGraphResult>
    EnsureVictoryRecordingGraph(bool include_completion) {
        const std::string name = include_completion
            ? "Internal: Complete and Record Battle Victory"
            : "Internal: Record Completed Battle Victory";
        const std::string description = include_completion
            ? "Completes the selected Victory lineage, records it, and validates the resulting TAS movie tree."
            : "Records a completed Victory and validates the resulting TAS movie tree.";
        const auto registry =
            savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
        const auto* completion = registry.Find("battle_completion");
        const auto* recording = registry.Find("battle_recording");
        if (!recording || (include_completion && !completion))
            return Failed<savor::db::SaveWorkflowGraphResult>(
                "hidden Battle recording workflow units are unavailable");

        auto node = [](const WorkflowUnitDefinition& unit,
                       std::string key) {
            savor::db::SaveWorkflowGraphNodeCommand result{};
            result.node_key = std::move(key);
            result.unit_kind = unit.unit_kind;
            result.display_name = unit.display_name;
            for (const auto& input : unit.required_inputs)
                result.inputs.push_back({
                    .input_key = input.key, .data_kind = input.data_kind,
                    .ref_kind = input.ref_kind,
                    .display_name = input.display_name,
                    .required = input.required});
            for (const auto& output : unit.possible_outputs)
                result.possible_outputs.push_back({
                    .output_key = output.key, .data_kind = output.data_kind,
                    .ref_kind = output.ref_kind,
                    .display_name = output.display_name});
            return result;
        };

        WorkflowGraphDraft draft{};
        draft.name = name;
        draft.description = description;
        draft.hidden = true;
        if (include_completion) draft.nodes.push_back(node(*completion, "complete"));
        draft.nodes.push_back(node(*recording, "record"));
        if (include_completion)
            draft.edges.push_back({
                .from_node_key = "complete", .output_key = "completion",
                .to_node_key = "record", .input_key = "completion"});

        std::vector<savor::db::authoring::WorkflowGraphHashNode> hash_nodes;
        for (const auto& item : draft.nodes)
            hash_nodes.push_back({item.node_key, item.unit_kind});
        std::vector<savor::db::authoring::WorkflowGraphHashEdge> hash_edges;
        for (const auto& edge : draft.edges)
            hash_edges.push_back({
                edge.from_node_key, edge.output_key,
                edge.to_node_key, edge.input_key});
        draft.graph_hash = savor::db::authoring::ComputeWorkflowGraphHash(
            draft.name, draft.description, hash_nodes, hash_edges);

        const auto existing =
            SavorDbAuthoringService::ListWorkflowGraphs(1000, true);
        if (!existing.ok)
            return ServiceResult<savor::db::SaveWorkflowGraphResult>::Err(
                existing.error);
        for (const auto& graph : existing.value) {
            if (graph.name == name && graph.graph_hash == draft.graph_hash)
                return ServiceResult<savor::db::SaveWorkflowGraphResult>::Ok({
                    .workflow_graph_id = graph.workflow_graph_id,
                    .workflow_graph_revision_id =
                        graph.workflow_graph_revision_id});
            if (graph.name == name)
                return Failed<savor::db::SaveWorkflowGraphResult>(
                    "hidden Battle recording workflow graph drifted");
        }
        return SavorDbAuthoringService::SaveWorkflowGraph(draft);
    }

    static void FilterUnavailableProductionUnits(
        std::vector<WorkflowUnitDefinition>& units) {
        auto* production = savorqt::SavorDbRuntime::instance().programKindRegistry();
        units.erase(
            std::remove_if(units.begin(), units.end(), [&](const auto& unit) {
                if (production == nullptr || unit.internal_step_kinds.empty()) return true;
                return std::any_of(unit.internal_step_kinds.begin(), unit.internal_step_kinds.end(),
                    [&](const auto& step_kind) {
                        return !production->HasRequiredAdaptersForStepKind(step_kind);
                    });
            }),
            units.end());
    }

    static std::string StandaloneNodeKey(const WorkflowUnitDefinition& unit) {
        return unit.unit_kind + "_standalone";
    }

    static std::string StandaloneWorkflowGraphName(
        const WorkflowUnitDefinition& unit,
        const std::optional<std::string>& authored_ref_kind,
        const std::optional<std::int64_t>& authored_ref_id) {
        std::string name = "Standalone " + unit.display_name;
        if (authored_ref_kind.has_value() && authored_ref_id.has_value()) {
            name += " " + *authored_ref_kind + "#" + std::to_string(*authored_ref_id);
        }
        return name;
    }

    static std::string StandaloneWorkflowGraphHash(
        const WorkflowUnitDefinition& unit,
        const std::optional<std::string>& authored_ref_kind,
        const std::optional<std::int64_t>& authored_ref_id) {
        return savor::db::authoring::ComputeStandaloneWorkflowGraphHash(
            unit.unit_kind, authored_ref_kind, authored_ref_id);
    }

    static savor::db::IUiReadDb* UiReadDb() {
        return savorqt::SavorDbRuntime::instance().uiReadDb();
    }

    static savor::db::execution::workflow::IWorkflowOrchestrationCommandService* WorkflowCommandService() {
        return savorqt::SavorDbRuntime::instance().workflowCommandService();
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

} // namespace savorqt::db

