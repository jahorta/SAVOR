#include "WorkflowTerminalAdvancementService.h"

namespace simcore::db::execution::workflow {

WorkflowTerminalAdvancementService::WorkflowTerminalAdvancementService(
    const AdapterChainOrchestrator* orchestrator,
    IWorkflowOrchestrationQueryService* query_service,
    IWorkflowOrchestrationCommandService* command_service)
    : orchestrator_(orchestrator)
    , query_service_(query_service)
    , command_service_(command_service) {
}

bool WorkflowTerminalAdvancementService::AdvanceForTerminalJob(
    std::int64_t job_id,
    WorkflowTerminalAdvancementResult* result_out,
    std::string* error_out) const {
    if (query_service_ == nullptr) {
        if (error_out) *error_out = "workflow query service is not configured";
        return false;
    }

    auto snapshot = query_service_->GetStepTerminalSnapshotForJob(job_id);
    if (!snapshot.has_value()) {
        if (result_out) {
            *result_out = WorkflowTerminalAdvancementResult{};
        }
        return true;
    }

    return AdvanceSnapshot(*snapshot, result_out, error_out);
}

bool WorkflowTerminalAdvancementService::AdvanceSnapshot(
    const WorkflowStepTerminalSnapshot& snapshot,
    WorkflowTerminalAdvancementResult* result_out,
    std::string* error_out) const {
    WorkflowTerminalAdvancementResult result{};
    result.snapshot_found = true;

    if (orchestrator_ == nullptr || command_service_ == nullptr) {
        if (error_out) *error_out = "terminal advancement dependencies are not configured";
        return false;
    }

    const StepCompletionSnapshot completion{
        .workflow_step_id = snapshot.workflow_step_id,
        .job_set_id = snapshot.job_set_id,
        .expected_total = snapshot.expected_total,
        .discovered_total = snapshot.discovered_total,
        .terminal_total = snapshot.terminal_total,
    };
    const programdb::WorkflowTransitionContext context{
        .workflow_instance_id = snapshot.workflow_instance_id,
        .workflow_step_id = snapshot.workflow_step_id,
        .job_set_id = snapshot.job_set_id,
        .workflow_kind = snapshot.workflow_kind,
        .step_key = snapshot.step_key,
        .input_ref_kind = snapshot.input_ref_kind,
        .input_ref_id = snapshot.input_ref_id,
    };

    const auto terminal = orchestrator_->OnStepTerminal(snapshot.step_kind, context, completion, nullptr);
    result.gate_can_transition = terminal.gate.can_transition;
    result.blocked_reason = terminal.gate.blocked_reason;

    std::string command_error;
    if (!terminal.gate.can_transition) {
        if (terminal.gate.blocked_reason.has_value()) {
            if (!command_service_->MarkStepBlocked(
                {
                    .workflow_step_id = snapshot.workflow_step_id,
                    .blocked_reason = terminal.gate.blocked_reason,
                    .requested_by = "workflow_terminal_advancement",
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
            .requested_by = "workflow_terminal_advancement",
        },
        &command_error)) {
        if (error_out) *error_out = command_error;
        return false;
    }

    const auto terminal_state = snapshot.failed_total > 0 ? "FAILED" : "COMPLETED";
    if (!command_service_->MarkStepTerminal(
        {
            .workflow_step_id = snapshot.workflow_step_id,
            .terminal_state = terminal_state,
            .requested_by = "workflow_terminal_advancement",
        },
        &command_error)) {
        if (error_out) *error_out = command_error;
        return false;
    }
    result.step_marked_terminal = true;

    if (!command_service_->AppendLifecycleEvent(
        {
            .workflow_instance_id = snapshot.workflow_instance_id,
            .workflow_step_id = snapshot.workflow_step_id,
            .event_kind = "Execution.WorkflowTransitionEvaluated.v1",
            .message = std::optional<std::string>("transition_evaluated"),
            .requested_by = "workflow_terminal_advancement",
        },
        &command_error)) {
        if (error_out) *error_out = command_error;
        return false;
    }
    result.transition_evaluated = true;

    const bool advanced = terminal.transition.has_value() && terminal.transition->should_advance;
    const auto event_kind = advanced
        ? "Execution.WorkflowTransitionAdvanced.v1"
        : "Execution.WorkflowTransitionBlocked.v1";
    const auto event_message = advanced
        ? std::optional<std::string>("transition_advanced")
        : terminal.transition.has_value() ? terminal.transition->blocked_reason : std::optional<std::string>("transition_blocked");

    if (advanced) {
        if (!terminal.transition->spawn_steps.empty()) {
            WorkflowAppendDynamicStepsCommand append{};
            append.workflow_instance_id = snapshot.workflow_instance_id;
            append.parent_workflow_step_id = snapshot.workflow_step_id;
            append.requested_by = "workflow_terminal_advancement";
            append.steps.reserve(terminal.transition->spawn_steps.size());
            for (const auto& step : terminal.transition->spawn_steps) {
                append.steps.push_back(WorkflowAppendDynamicStepSpec{
                    .step_key = step.step_key,
                    .step_kind = step.step_kind,
                    .input_ref_kind = step.input_ref_kind,
                    .input_ref_id = step.input_ref_id,
                    .guard_kind = step.guard_kind,
                    .guard_value = step.guard_value,
                    .priority = step.priority,
                    .max_attempts = step.max_attempts,
                });
            }
            if (!command_service_->AppendDynamicSteps(append, &command_error)) {
                if (error_out) *error_out = command_error;
                return false;
            }
            result.spawned_step_count = static_cast<int>(append.steps.size());
            result.advanced_next_step = result.spawned_step_count > 0;
        } else if (terminal.transition->next_step_key.has_value()) {
            if (!command_service_->MarkStepReady(
                {
                    .workflow_instance_id = snapshot.workflow_instance_id,
                    .step_key = *terminal.transition->next_step_key,
                    .requested_by = "workflow_terminal_advancement",
                },
                &command_error)) {
                if (error_out) *error_out = command_error;
                return false;
            }
            result.advanced_next_step = true;
        } else if (!command_service_->CompleteWorkflowInstance(
            {
                .workflow_instance_id = snapshot.workflow_instance_id,
                .requested_by = "workflow_terminal_advancement",
            },
            &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        } else {
            result.workflow_completed = true;
        }
    }

    if (!command_service_->AppendLifecycleEvent(
        {
            .workflow_instance_id = snapshot.workflow_instance_id,
            .workflow_step_id = snapshot.workflow_step_id,
            .event_kind = event_kind,
            .message = event_message,
            .requested_by = "workflow_terminal_advancement",
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

} // namespace simcore::db::execution::workflow
