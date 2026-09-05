#include "WorkflowSettlementAdvancementService.h"

#include "../IExecutionDb.h"

#include <charconv>
#include <map>

#include "../../../SavorCore/Utils/Hash.h"

namespace savor::db::execution::workflow {

namespace {

void AppendField(std::string* encoded, std::string_view value) {
    encoded->append(std::to_string(value.size()));
    encoded->push_back(':');
    encoded->append(value);
}

void AppendOptionalField(
    std::string* encoded, const std::optional<std::string>& value) {
    AppendField(encoded, value ? "1" : "0");
    if (value) AppendField(encoded, *value);
}

bool ReadField(std::string_view encoded, std::size_t* cursor, std::string* value) {
    if (!cursor || !value || *cursor >= encoded.size()) return false;
    const auto colon = encoded.find(':', *cursor);
    if (colon == std::string_view::npos || colon == *cursor) return false;
    std::size_t size = 0;
    const auto parsed = std::from_chars(
        encoded.data() + *cursor, encoded.data() + colon, size);
    if (parsed.ec != std::errc{} || parsed.ptr != encoded.data() + colon
        || size > encoded.size() - colon - 1) return false;
    *value = std::string(encoded.substr(colon + 1, size));
    *cursor = colon + 1 + size;
    return true;
}

bool ReadOptionalField(std::string_view encoded, std::size_t* cursor,
    std::optional<std::string>* value) {
    std::string present;
    if (!ReadField(encoded, cursor, &present)
        || (present != "0" && present != "1")) return false;
    if (present == "0") {
        value->reset();
        return true;
    }
    std::string text;
    if (!ReadField(encoded, cursor, &text)) return false;
    *value = std::move(text);
    return true;
}

template <typename Integer>
bool ParseInteger(std::string_view text, Integer* value) {
    if (!value || text.empty()) return false;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), *value);
    return parsed.ec == std::errc{}
        && parsed.ptr == text.data() + text.size();
}

std::string EncodeTransitionDecision(
    const programdb::WorkflowTransitionDecision& decision) {
    std::string encoded;
    AppendField(&encoded, "savor.workflow-transition-decision/1");
    AppendField(&encoded, decision.should_advance ? "1" : "0");
    AppendField(&encoded, decision.workflow_failure ? "1" : "0");
    AppendOptionalField(&encoded, decision.blocked_reason);
    AppendOptionalField(&encoded, decision.next_step_key);
    AppendField(&encoded, std::to_string(decision.spawn_steps.size()));
    for (const auto& step : decision.spawn_steps) {
        AppendField(&encoded,
            std::to_string(step.parent_workflow_step_id.value_or(0)));
        AppendField(&encoded, step.step_key);
        AppendField(&encoded, step.step_kind);
        AppendOptionalField(&encoded, step.input_ref_kind);
        AppendField(&encoded, std::to_string(step.input_ref_id.value_or(0)));
        AppendOptionalField(&encoded, step.guard_kind);
        AppendOptionalField(&encoded, step.guard_value);
        AppendField(&encoded, std::to_string(step.priority));
        AppendField(&encoded, std::to_string(step.max_attempts));
    }
    return encoded;
}

bool DecodeTransitionDecision(std::string_view encoded,
    programdb::WorkflowTransitionDecision* decision) {
    if (!decision) return false;
    std::size_t cursor = 0;
    std::string value;
    if (!ReadField(encoded, &cursor, &value)
        || value != "savor.workflow-transition-decision/1") return false;
    programdb::WorkflowTransitionDecision decoded{};
    if (!ReadField(encoded, &cursor, &value)
        || (value != "0" && value != "1")) return false;
    decoded.should_advance = value == "1";
    if (!ReadField(encoded, &cursor, &value)
        || (value != "0" && value != "1")) return false;
    decoded.workflow_failure = value == "1";
    if (!ReadOptionalField(encoded, &cursor, &decoded.blocked_reason)
        || !ReadOptionalField(encoded, &cursor, &decoded.next_step_key)
        || !ReadField(encoded, &cursor, &value)) return false;
    std::size_t count = 0;
    if (!ParseInteger(value, &count) || count > 100000) return false;
    decoded.spawn_steps.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        programdb::WorkflowTransitionDecision::DynamicStep step{};
        std::int64_t parent = 0;
        std::int64_t input_id = 0;
        if (!ReadField(encoded, &cursor, &value)
            || !ParseInteger(value, &parent)) return false;
        if (parent > 0) step.parent_workflow_step_id = parent;
        if (!ReadField(encoded, &cursor, &step.step_key)
            || !ReadField(encoded, &cursor, &step.step_kind)
            || !ReadOptionalField(encoded, &cursor, &step.input_ref_kind)
            || !ReadField(encoded, &cursor, &value)
            || !ParseInteger(value, &input_id)) return false;
        if (input_id > 0) step.input_ref_id = input_id;
        if (!ReadOptionalField(encoded, &cursor, &step.guard_kind)
            || !ReadOptionalField(encoded, &cursor, &step.guard_value)
            || !ReadField(encoded, &cursor, &value)
            || !ParseInteger(value, &step.priority)
            || !ReadField(encoded, &cursor, &value)
            || !ParseInteger(value, &step.max_attempts)) return false;
        decoded.spawn_steps.push_back(std::move(step));
    }
    if (cursor != encoded.size()) return false;
    *decision = std::move(decoded);
    return true;
}

std::string SettlementTriggerFingerprint(
    const WorkflowStepSettlementSnapshot& snapshot,
    const std::optional<std::string>& output_ref_kind,
    const std::optional<std::int64_t>& output_ref_id) {
    std::string canonical;
    AppendField(&canonical, "savor.workflow-transition-trigger/1");
    const auto add_number = [&](const auto value) {
        AppendField(&canonical, std::to_string(value));
    };
    add_number(snapshot.workflow_instance_id);
    add_number(snapshot.workflow_step_id);
    add_number(snapshot.job_set_id);
    add_number(snapshot.workflow_graph_revision_id.value_or(0));
    AppendField(&canonical, snapshot.workflow_kind);
    AppendField(&canonical, snapshot.step_key);
    AppendField(&canonical, snapshot.graph_node_key);
    AppendField(&canonical, snapshot.step_kind);
    add_number(snapshot.expected_total);
    add_number(snapshot.discovered_total);
    add_number(snapshot.settled_total);
    add_number(snapshot.succeeded_total);
    add_number(snapshot.failed_total);
    add_number(snapshot.interrupted_total);
    add_number(snapshot.superseded_total);
    add_number(snapshot.canceled_total);
    AppendField(&canonical, output_ref_kind.value_or(""));
    add_number(output_ref_id.value_or(0));
    return hash::sha256(canonical.data(), canonical.size());
}

} // namespace

WorkflowTransitionApplicationService::WorkflowTransitionApplicationService(
    IWorkflowOrchestrationQueryService* query_service,
    IWorkflowOrchestrationCommandService* command_service,
    int successor_step_priority_boost)
    : query_service_(query_service)
    , command_service_(command_service)
    , successor_step_priority_boost_(successor_step_priority_boost) {
}

bool WorkflowTransitionApplicationService::ResolveActivation(
    const WorkflowTransitionActivationRequest& request,
    const std::function<programdb::WorkflowTransitionDecision()>& evaluate,
    WorkflowTransitionActivationResolution* resolution_out,
    std::string* error_out) const {
    if (resolution_out) *resolution_out = {};
    if (!query_service_ || !command_service_ || !evaluate
        || request.workflow_instance_id <= 0
        || request.source_workflow_step_id <= 0
        || request.activation_key.empty()
        || request.trigger_fingerprint.empty()
        || request.requested_by.empty()) {
        if (error_out) *error_out =
            "workflow transition activation request is incomplete";
        return false;
    }
    auto existing = query_service_->GetWorkflowTransitionActivation(
        request.workflow_instance_id, request.activation_key);
    if (existing) {
        if (existing->source_workflow_step_id != request.source_workflow_step_id
            || existing->activation_kind != request.activation_kind
            || existing->trigger_fingerprint != request.trigger_fingerprint) {
            if (error_out) *error_out = "WORKFLOW_TRANSITION_DECISION_DRIFT";
            return false;
        }
        programdb::WorkflowTransitionDecision decision{};
        if (!DecodeTransitionDecision(existing->decision_payload, &decision)
            || hash::sha256(existing->decision_payload.data(),
                    existing->decision_payload.size())
                != existing->decision_sha256) {
            if (error_out) *error_out =
                "stored workflow transition decision is invalid";
            return false;
        }
        if (resolution_out) {
            resolution_out->activation = *existing;
            resolution_out->decision = std::move(decision);
            resolution_out->already_applied = existing->state
                == WorkflowTransitionActivationState::Applied;
        }
        return true;
    }

    const auto decision = evaluate();
    const auto payload = EncodeTransitionDecision(decision);
    const auto sha = hash::sha256(payload.data(), payload.size());
    WorkflowFreezeTransitionActivationReceipt receipt{};
    if (!command_service_->FreezeTransitionActivation({
            .workflow_instance_id = request.workflow_instance_id,
            .source_workflow_step_id = request.source_workflow_step_id,
            .activation_kind = request.activation_kind,
            .activation_key = request.activation_key,
            .trigger_fingerprint = request.trigger_fingerprint,
            .decision_payload = payload,
            .decision_sha256 = sha,
            .requested_by = request.requested_by,
        }, &receipt, error_out)) return false;
    if (receipt.activation.trigger_fingerprint != request.trigger_fingerprint
        || receipt.activation.decision_sha256 != sha
        || receipt.activation.decision_payload != payload) {
        if (error_out) *error_out = "WORKFLOW_TRANSITION_DECISION_DRIFT";
        return false;
    }
    if (resolution_out) {
        resolution_out->activation = std::move(receipt.activation);
        resolution_out->decision = decision;
        resolution_out->newly_frozen = receipt.created;
        resolution_out->already_applied = resolution_out->activation.state
            == WorkflowTransitionActivationState::Applied;
    }
    return true;
}

bool WorkflowTransitionApplicationService::MarkActivationApplied(
    const WorkflowTransitionActivationResolution& resolution,
    std::string disposition,
    std::string_view requested_by,
    std::string* error_out) const {
    if (!command_service_) {
        if (error_out) *error_out = "workflow command service is unavailable";
        return false;
    }
    return command_service_->ApplyTransitionActivation({
        .workflow_transition_activation_id =
            resolution.activation.workflow_transition_activation_id,
        .expected_decision_sha256 = resolution.activation.decision_sha256,
        .disposition = std::move(disposition),
        .requested_by = std::string(requested_by),
    }, error_out);
}

void WorkflowTransitionApplicationService::RecordActivationFailure(
    const WorkflowTransitionActivationResolution& resolution,
    std::string_view diagnostic) const {
    if (!command_service_ || resolution.already_applied) return;
    (void)command_service_->RecordTransitionActivationFailure({
        .workflow_transition_activation_id =
            resolution.activation.workflow_transition_activation_id,
        .expected_decision_sha256 = resolution.activation.decision_sha256,
        .diagnostic = std::string(diagnostic),
    }, nullptr);
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
        .workflow_graph_revision_id = snapshot.workflow_graph_revision_id,
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
    std::optional<WorkflowTransitionActivationResolution> activation;
    WorkflowTransitionApplicationService applicator(
        query_service_, command_service_, successor_step_priority_boost_);
    if (gate.can_transition && !gate.workflow_fail) {
        WorkflowTransitionActivationResolution resolved{};
        const WorkflowTransitionActivationRequest request{
            .workflow_instance_id = snapshot.workflow_instance_id,
            .source_workflow_step_id = snapshot.workflow_step_id,
            .activation_kind = WorkflowTransitionActivationKind::Settlement,
            .activation_key = "settlement:"
                + std::to_string(snapshot.workflow_step_id) + ":"
                + std::to_string(snapshot.job_set_id),
            .trigger_fingerprint = SettlementTriggerFingerprint(
                snapshot, output_ref_kind, output_ref_id),
            .requested_by = "workflow_settlement_advancement",
        };
        std::string activation_error;
        if (!applicator.ResolveActivation(request, [&]() {
                if (descriptor != nullptr
                    && descriptor->workflow_transition != nullptr) {
                    return descriptor->workflow_transition
                        ->EvaluateTransition(context);
                }
                return programdb::WorkflowTransitionDecision{};
            }, &resolved, &activation_error)) {
            if (error_out) *error_out = activation_error;
            return false;
        }
        result.transition_evaluated = true;
        if (resolved.already_applied) {
            if (result_out) *result_out = result;
            return true;
        }
        transition = resolved.decision;
        activation = std::move(resolved);
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

        if (activation && !applicator.MarkActivationApplied(
                *activation, "FAILED", "workflow_settlement_advancement",
                &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        }

        if (result_out) {
            *result_out = result;
        }
        return true;
    }

    const bool advanced = transition.has_value() && transition->should_advance;

    if (advanced) {
        const int successor_priority = snapshot.priority + successor_step_priority_boost_;
        if (!transition->spawn_steps.empty()) {
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
            result.graph_routing_failure = graph_result.failure;
            if (result_out) *result_out = result;
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

    if (activation && !applicator.MarkActivationApplied(
            *activation,
            result.workflow_completed ? "COMPLETED"
                : (advanced || result.advanced_next_step) ? "ADVANCED"
                : "BLOCKED",
            "workflow_settlement_advancement",
            &command_error)) {
        applicator.RecordActivationFailure(*activation, command_error);
        if (error_out) *error_out = command_error;
        return false;
    }

    if (result_out) {
        *result_out = result;
    }
    return true;
}

} // namespace savor::db::execution::workflow
