#include "WorkflowSettlementAdvancementService.h"

#include "../IExecutionDb.h"

#include <map>

namespace savor::db::execution::workflow {

WorkflowTransitionApplicationService::WorkflowTransitionApplicationService(
    IWorkflowOrchestrationCommandService* command_service,
    int successor_step_priority_boost)
    : command_service_(command_service)
    , successor_step_priority_boost_(successor_step_priority_boost) {
}

bool WorkflowTransitionApplicationService::ApplyDynamicSteps(
    std::int64_t workflow_instance_id,
    std::int64_t default_parent_workflow_step_id,
    int source_priority,
    const programdb::WorkflowTransitionDecision& transition,
    std::string_view requested_by,
    int* spawned_step_count_out,
    std::string* error_out) const {
    if (spawned_step_count_out) *spawned_step_count_out = 0;
    if (command_service_ == nullptr || workflow_instance_id <= 0
        || default_parent_workflow_step_id <= 0 || requested_by.empty()) {
        if (error_out) *error_out = "workflow transition application is incomplete";
        return false;
    }
    std::map<std::int64_t, std::vector<WorkflowAppendDynamicStepSpec>> grouped;
    for (const auto& step : transition.spawn_steps) {
        const auto parent = step.parent_workflow_step_id.value_or(
            default_parent_workflow_step_id);
        if (parent <= 0) {
            if (error_out) *error_out = "dynamic workflow step has no valid parent";
            return false;
        }
        grouped[parent].push_back({
            .step_key = step.step_key,
            .step_kind = step.step_kind,
            .input_ref_kind = step.input_ref_kind,
            .input_ref_id = step.input_ref_id,
            .guard_kind = step.guard_kind,
            .guard_value = step.guard_value,
            .priority = source_priority + successor_step_priority_boost_ + step.priority,
            .max_attempts = step.max_attempts,
        });
    }
    int spawned = 0;
    for (auto& [parent, steps] : grouped) {
        WorkflowAppendDynamicStepsCommand append{
            .workflow_instance_id = workflow_instance_id,
            .parent_workflow_step_id = parent,
            .steps = std::move(steps),
            .requested_by = std::string(requested_by),
        };
        if (!command_service_->AppendDynamicSteps(append, error_out)) return false;
        spawned += static_cast<int>(append.steps.size());
    }
    if (spawned_step_count_out) *spawned_step_count_out = spawned;
    return true;
}

WorkflowSettlementAdvancementService::WorkflowSettlementAdvancementService(
    const programdb::ProgramKindRegistry* program_kind_registry,
    StepSettlementGateService* completion_gate,
    savor::db::IExecutionDb* execution_db,
    IWorkflowOrchestrationQueryService* query_service,
    IWorkflowOrchestrationCommandService* command_service,
    const WorkflowGraphRoutingService* graph_routing_service,
    int successor_step_priority_boost)
    : program_kind_registry_(program_kind_registry)
    , completion_gate_(completion_gate)
    , execution_db_(execution_db)
    , query_service_(query_service)
    , command_service_(command_service)
    , graph_routing_service_(graph_routing_service)
    , successor_step_priority_boost_(successor_step_priority_boost) {
}

WorkflowSettlementAdvancementService::WorkflowSettlementAdvancementService(
    const programdb::ProgramKindRegistry* program_kind_registry,
    StepSettlementGateService* completion_gate,
    IWorkflowOrchestrationQueryService* query_service,
    IWorkflowOrchestrationCommandService* command_service,
    const WorkflowGraphRoutingService* graph_routing_service,
    int successor_step_priority_boost)
    : WorkflowSettlementAdvancementService(
        program_kind_registry,
        completion_gate,
        nullptr,
        query_service,
        command_service,
        graph_routing_service,
        successor_step_priority_boost) {
}

bool WorkflowSettlementAdvancementService::AdvanceForSettledJob(
    std::int64_t job_id,
    WorkflowSettlementAdvancementResult* result_out,
    std::string* error_out,
    std::optional<programdb::ProgramJobContinuationOutput> output) const {
    if (query_service_ == nullptr) {
        if (error_out) *error_out = "workflow query service is not configured";
        return false;
    }

    auto snapshot = query_service_->GetStepSettlementSnapshotForJob(job_id);
    if (!snapshot.has_value()) {
        if (result_out) {
            *result_out = WorkflowSettlementAdvancementResult{};
        }
        return true;
    }

    return AdvanceSnapshot(*snapshot, result_out, error_out, std::move(output));
}

bool WorkflowSettlementAdvancementService::AdvanceSnapshot(
    const WorkflowStepSettlementSnapshot& snapshot,
    WorkflowSettlementAdvancementResult* result_out,
    std::string* error_out,
    std::optional<programdb::ProgramJobContinuationOutput> output) const {
    WorkflowSettlementAdvancementResult result{};
    result.snapshot_found = true;

    if (program_kind_registry_ == nullptr || completion_gate_ == nullptr
        || command_service_ == nullptr) {
        if (error_out) *error_out = "settlement advancement dependencies are not configured";
        return false;
    }

    std::string blocking_error;
    if (snapshot.failed_total > 0) {
        if (!command_service_->FailWorkflowInstance({
                .workflow_instance_id = snapshot.workflow_instance_id,
                .workflow_step_id = snapshot.workflow_step_id,
                .failure_code = "STEP_FAILED",
                .failure_message = "workflow step contains failed jobs",
                .requested_by = "workflow_settlement_advancement",
            }, &blocking_error)) {
            if (error_out) *error_out = blocking_error;
            return false;
        }
        result.gate_can_transition = true;
        result.step_marked_terminal = true;
        result.workflow_failed = true;
        if (result_out) *result_out = result;
        return true;
    }
    if (snapshot.interrupted_total > 0) {
        if (!command_service_->InterruptWorkflowInstance({
                .workflow_instance_id = snapshot.workflow_instance_id,
                .workflow_step_id = snapshot.workflow_step_id,
                .interruption_code = "WORKFLOW_INTERRUPTED",
                .interruption_message = "workflow step contains interrupted jobs",
                .requested_by = "workflow_settlement_advancement",
            }, &blocking_error)) {
            if (error_out) *error_out = blocking_error;
            return false;
        }
        result.gate_can_transition = true;
        result.step_marked_terminal = true;
        if (result_out) *result_out = result;
        return true;
    }
    if (snapshot.canceled_total > 0) {
        if (snapshot.workflow_state == "CANCELLING") {
            if (result_out) *result_out = result;
            return true;
        }
        if (!command_service_->FailWorkflowInstance({
                .workflow_instance_id = snapshot.workflow_instance_id,
                .workflow_step_id = snapshot.workflow_step_id,
                .failure_code = "JOB_CANCELED_OUTSIDE_WORKFLOW_CANCELLATION",
                .failure_message = "canceled job found outside workflow cancellation",
                .requested_by = "workflow_settlement_advancement",
            }, &blocking_error)) {
            if (error_out) *error_out = blocking_error;
            return false;
        }
        result.gate_can_transition = true;
        result.step_marked_terminal = true;
        result.workflow_failed = true;
        if (result_out) *result_out = result;
        return true;
    }
    if (snapshot.discovered_total > 0 && snapshot.succeeded_total == 0) {
        if (!command_service_->FailWorkflowInstance({
                .workflow_instance_id = snapshot.workflow_instance_id,
                .workflow_step_id = snapshot.workflow_step_id,
                .failure_code = "STEP_NO_SUCCESSFUL_JOBS",
                .failure_message = "settled workflow step has no successful jobs",
                .requested_by = "workflow_settlement_advancement",
            }, &blocking_error)) {
            if (error_out) *error_out = blocking_error;
            return false;
        }
        result.gate_can_transition = true;
        result.step_marked_terminal = true;
        result.workflow_failed = true;
        if (result_out) *result_out = result;
        return true;
    }

    const StepSettlementSnapshot completion{
        .workflow_step_id = snapshot.workflow_step_id,
        .job_set_id = snapshot.job_set_id,
        .expected_total = snapshot.expected_total,
        .discovered_total = snapshot.discovered_total,
        .settled_total = snapshot.settled_total,
        .succeeded_total = snapshot.succeeded_total,
        .failed_total = snapshot.failed_total,
        .interrupted_total = snapshot.interrupted_total,
        .superseded_total = snapshot.superseded_total,
        .canceled_total = snapshot.canceled_total,
    };
    std::optional<std::string> output_ref_kind =
        output.has_value() && !output->ref_kind.empty()
            ? std::optional<std::string>(output->ref_kind)
            : snapshot.output_ref_kind;
    std::optional<std::int64_t> output_ref_id =
        output.has_value() && output->ref_id > 0
            ? std::optional<std::int64_t>(output->ref_id)
            : snapshot.output_ref_id;
    if ((!output_ref_id.has_value() || *output_ref_id <= 0) && execution_db_ != nullptr) {
        const auto job_outputs = execution_db_->ListJobOutputsForWorkflowStep(snapshot.workflow_step_id);
        if (!job_outputs.empty()) {
            output_ref_kind = job_outputs.front().ref_kind;
            output_ref_id = job_outputs.front().ref_id;
        }
    }

    const programdb::WorkflowTransitionContext context{
        .workflow_instance_id = snapshot.workflow_instance_id,
        .workflow_step_id = snapshot.workflow_step_id,
        .job_set_id = snapshot.job_set_id,
        .expected_total = snapshot.expected_total,
        .discovered_total = snapshot.discovered_total,
        .settled_total = snapshot.settled_total,
        .succeeded_total = snapshot.succeeded_total,
        .failed_total = snapshot.failed_total,
        .interrupted_total = snapshot.interrupted_total,
        .superseded_total = snapshot.superseded_total,
        .canceled_total = snapshot.canceled_total,
        .priority = snapshot.priority,
        .workflow_kind = snapshot.workflow_kind,
        .step_key = snapshot.step_key,
        .graph_node_key = snapshot.graph_node_key,
        .step_kind = snapshot.step_kind,
        .input_ref_kind = snapshot.input_ref_kind,
        .input_ref_id = snapshot.input_ref_id,
        .output_ref_kind = output_ref_kind,
        .output_ref_id = output_ref_id,
    };

    const auto* descriptor =
        program_kind_registry_->FindForStepKind(snapshot.step_kind);
    auto gate = completion_gate_->Evaluate(completion);
    if (gate.workflow_fail) gate.can_transition = true;

    std::optional<programdb::WorkflowTransitionDecision> transition;
    if (gate.can_transition && !gate.workflow_fail && descriptor != nullptr
        && descriptor->workflow_transition != nullptr) {
        transition = descriptor->workflow_transition->EvaluateTransition(context);
        if (transition->workflow_failure) {
            gate.workflow_fail = true;
            gate.blocked_reason = transition->blocked_reason;
        }
    }
    result.gate_can_transition = gate.can_transition;
    result.blocked_reason = gate.blocked_reason;

    std::string command_error;
    if (!gate.can_transition) {
        if (gate.blocked_reason.has_value()) {
            if (!command_service_->MarkStepBlocked(
                {
                    .workflow_step_id = snapshot.workflow_step_id,
                    .blocked_reason = gate.blocked_reason,
                    .requested_by = "workflow_settlement_advancement",
                },
                &command_error)) {
                if (error_out) *error_out = command_error;
                return false;
            }
        }
        if (result_out) {
            *result_out = result;
        }
        return true;
    }

    if (!command_service_->MarkStepBlocked(
        {
            .workflow_step_id = snapshot.workflow_step_id,
            .blocked_reason = std::nullopt,
            .requested_by = "workflow_settlement_advancement",
        },
        &command_error)) {
        if (error_out) *error_out = command_error;
        return false;
    }

    if (!gate.workflow_fail) {
        if (!command_service_->CompleteWorkflowStep(
            {
                .workflow_step_id = snapshot.workflow_step_id,
                .completion_state = "COMPLETED",
                .output_ref_kind = context.output_ref_kind,
                .output_ref_id = context.output_ref_id,
                .requested_by = "workflow_settlement_advancement",
            },
            &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        }
        result.step_marked_terminal = true;
    }

    if (!gate.workflow_fail && snapshot.discovered_total == 0
        && !command_service_->AppendLifecycleEvent(
            {
                .workflow_instance_id = snapshot.workflow_instance_id,
                .workflow_step_id = snapshot.workflow_step_id,
                .event_kind = "Execution.WorkflowStepEmpty.v1",
                .message = std::optional<std::string>("settlement_reason=EMPTY"),
                .requested_by = "workflow_settlement_advancement",
            },
            &command_error)) {
        if (error_out) *error_out = command_error;
        return false;
    }

    if (gate.workflow_fail) {
        const auto failure_message = gate.blocked_reason.value_or(
            "workflow step completed with failed jobs");
        if (!command_service_->FailWorkflowInstance(
            {
                .workflow_instance_id = snapshot.workflow_instance_id,
                .workflow_step_id = snapshot.workflow_step_id,
                .failure_code = transition.has_value()
                        && transition->workflow_failure
                    ? "TRANSITION_REJECTED"
                    : "STEP_FAILED",
                .failure_message = failure_message,
                .requested_by = "workflow_settlement_advancement",
            },
            &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        }
        result.step_marked_terminal = true;
        result.workflow_failed = true;

        if (!command_service_->AppendLifecycleEvent(
            {
                .workflow_instance_id = snapshot.workflow_instance_id,
                .workflow_step_id = snapshot.workflow_step_id,
                .event_kind = "Execution.WorkflowTransitionEvaluated.v1",
                .message = std::optional<std::string>("transition_failed"),
                .requested_by = "workflow_settlement_advancement",
            },
            &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        }
        result.transition_evaluated = true;

        if (!command_service_->AppendLifecycleEvent(
            {
                .workflow_instance_id = snapshot.workflow_instance_id,
                .workflow_step_id = snapshot.workflow_step_id,
                .event_kind = "Execution.WorkflowTransitionBlocked.v1",
                .message = std::optional<std::string>("transition_failed"),
                .requested_by = "workflow_settlement_advancement",
            },
            &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        }

        if (result_out) {
            *result_out = result;
        }
        return true;
    }

    if (!command_service_->AppendLifecycleEvent(
        {
            .workflow_instance_id = snapshot.workflow_instance_id,
            .workflow_step_id = snapshot.workflow_step_id,
            .event_kind = "Execution.WorkflowTransitionEvaluated.v1",
            .message = std::optional<std::string>("transition_evaluated"),
            .requested_by = "workflow_settlement_advancement",
        },
        &command_error)) {
        if (error_out) *error_out = command_error;
        return false;
    }
    result.transition_evaluated = true;

    const bool advanced = transition.has_value() && transition->should_advance;
    const auto event_kind = advanced
        ? "Execution.WorkflowTransitionAdvanced.v1"
        : "Execution.WorkflowTransitionBlocked.v1";
    const auto event_message = advanced
        ? std::optional<std::string>("transition_advanced")
        : transition.has_value() ? transition->blocked_reason : std::optional<std::string>("transition_blocked");

    if (advanced) {
        const int successor_priority = snapshot.priority + successor_step_priority_boost_;
        if (!transition->spawn_steps.empty()) {
            WorkflowTransitionApplicationService applicator(
                command_service_, successor_step_priority_boost_);
            if (!applicator.ApplyDynamicSteps(
                    snapshot.workflow_instance_id,
                    snapshot.workflow_step_id,
                    snapshot.priority,
                    *transition,
                    "workflow_settlement_advancement",
                    &result.spawned_step_count,
                    &command_error)) {
                if (error_out) *error_out = command_error;
                return false;
            }
            result.advanced_next_step = result.spawned_step_count > 0;
        } else if (transition->next_step_key.has_value()
            && snapshot.workflow_kind != "workflow_graph") {
            if (!command_service_->MarkStepReady(
                {
                    .workflow_instance_id = snapshot.workflow_instance_id,
                    .step_key = *transition->next_step_key,
                    .requested_by = "workflow_settlement_advancement",
                    .ready_priority = successor_priority,
                },
                &command_error)) {
                if (error_out) *error_out = command_error;
                return false;
            }
            result.advanced_next_step = true;
        } else if (snapshot.workflow_kind != "workflow_graph") {
            if (!command_service_->CompleteWorkflowInstance(
                    {
                        .workflow_instance_id = snapshot.workflow_instance_id,
                        .requested_by = "workflow_settlement_advancement",
                    },
                    &command_error)) {
                if (error_out) *error_out = command_error;
                return false;
            }
            result.workflow_completed = true;
        }
    }

    if (snapshot.workflow_kind == "workflow_graph" && graph_routing_service_ != nullptr) {
        WorkflowGraphRoutingResult graph_result{};
        if (!graph_routing_service_->RouteTerminalStep(snapshot, &graph_result, &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        }
        if (graph_result.blocked_reason.has_value()) {
            result.blocked_reason = graph_result.blocked_reason;
            if (!command_service_->MarkStepBlocked(
                    {
                        .workflow_step_id = snapshot.workflow_step_id,
                        .blocked_reason = graph_result.blocked_reason,
                        .requested_by = "workflow_graph_routing",
                    },
                    &command_error)) {
                if (error_out) *error_out = command_error;
                return false;
            }
        }
        if (graph_result.advanced_ready_step) {
            result.advanced_next_step = true;
        }
        if (graph_result.workflow_completed) {
            result.workflow_completed = true;
        }
    }

    if (!command_service_->AppendLifecycleEvent(
        {
            .workflow_instance_id = snapshot.workflow_instance_id,
            .workflow_step_id = snapshot.workflow_step_id,
            .event_kind = event_kind,
            .message = event_message,
            .requested_by = "workflow_settlement_advancement",
        },
        &command_error)) {
        if (error_out) *error_out = command_error;
        return false;
    }

    if (result_out) {
        *result_out = result;
    }
    return true;
}

} // namespace savor::db::execution::workflow
