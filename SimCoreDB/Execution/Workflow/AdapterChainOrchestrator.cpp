#include "AdapterChainOrchestrator.h"

namespace simcore::db::execution::workflow {

namespace {

std::string GateKey(const StepCompletionSnapshot& snapshot) {
    return std::to_string(snapshot.workflow_step_id) + ":" + std::to_string(snapshot.job_set_id);
}

} // namespace

StepCompletionGateDecision StepCompletionGateService::Evaluate(const StepCompletionSnapshot& snapshot) {
    StepCompletionGateDecision decision{};

    if (snapshot.discovered_total <= 0) {
        decision.can_transition = true;
        return decision;
    }

    if (snapshot.expected_total > 0 && snapshot.expected_total != snapshot.discovered_total) {
        const auto key = GateKey(snapshot);
        const int attempts = ++mismatch_attempts_[key];
        decision.can_transition = false;
        if (attempts == 1) {
            decision.blocked_reason = "STEP_BLOCKED_COUNT_MISMATCH";
        } else {
            decision.blocked_reason = "STEP_BLOCKED_COUNT_MISMATCH_TERMINAL_FAIL";
            decision.terminal_fail = true;
        }
        return decision;
    }

    if (snapshot.terminal_total < snapshot.discovered_total) {
        decision.can_transition = false;
        decision.blocked_reason = "STEP_BLOCKED_JOBS_NON_TERMINAL";
        return decision;
    }

    if (snapshot.failed_total > 0) {
        decision.can_transition = true;
        decision.terminal_fail = true;
        return decision;
    }

    decision.can_transition = true;
    return decision;
}

AdapterChainOrchestrator::AdapterChainOrchestrator(
    const programdb::ProgramKindRegistry* registry,
    StepCompletionGateService* completion_gate)
    : registry_(registry)
    , completion_gate_(completion_gate) {
}

std::optional<programdb::WorkflowStepScheduleResult> AdapterChainOrchestrator::OnInputComplete(
    std::string_view step_kind,
    std::int64_t domain_ref_id,
    AdapterChainTrace* trace) const {
    if (registry_ == nullptr) return std::nullopt;
    const auto* descriptor = registry_->FindForStepKind(step_kind);
    if (descriptor == nullptr || descriptor->job_persistence == nullptr) {
        return std::nullopt;
    }
    if (trace) trace->job_persistence_invoked = true;
    return descriptor->job_persistence->EncodeForQueueing(domain_ref_id);
}

std::optional<programdb::WorkflowStepScheduleResult> AdapterChainOrchestrator::OnGraphInputComplete(
    std::string_view step_kind,
    const programdb::WorkflowGraphStepScheduleContext& context,
    AdapterChainTrace* trace) const {
    if (registry_ == nullptr) return std::nullopt;
    const auto* descriptor = registry_->FindForStepKind(step_kind);
    if (descriptor == nullptr || descriptor->graph_job_persistence == nullptr) {
        return std::nullopt;
    }
    if (trace) trace->job_persistence_invoked = true;
    return descriptor->graph_job_persistence->EncodeForGraphQueueing(context);
}

std::optional<programdb::RuntimeInitRequest> AdapterChainOrchestrator::OnJobClaimed(
    std::string_view step_kind,
    std::int64_t job_id,
    AdapterChainTrace* trace) const {
    if (registry_ == nullptr) return std::nullopt;
    const auto* descriptor = registry_->FindForStepKind(step_kind);
    if (descriptor == nullptr || descriptor->runtime_init == nullptr) {
        return std::nullopt;
    }
    if (trace) trace->runtime_init_invoked = true;
    return descriptor->runtime_init->BuildRuntimeInit(job_id);
}

std::optional<programdb::ResultMapPayload> AdapterChainOrchestrator::OnJobTerminal(
    std::string_view step_kind,
    std::int64_t job_id,
    const std::string& result_ini,
    AdapterChainTrace* trace,
    std::string* error_out) const {
    if (registry_ == nullptr) return std::nullopt;
    const auto* descriptor = registry_->FindForStepKind(step_kind);
    if (descriptor == nullptr || descriptor->result_mapper == nullptr) {
        return std::nullopt;
    }

    if (trace) trace->result_mapper_invoked = true;
    auto payload = descriptor->result_mapper->MapPrimaryResult(job_id, result_ini);
    if (descriptor->result_payload_writer != nullptr) {
        if (!descriptor->result_payload_writer->Persist(payload, error_out)) {
            return std::nullopt;
        }
        if (trace) trace->result_writer_invoked = true;
    }
    return payload;
}

std::optional<programdb::ResultMapPayload> AdapterChainOrchestrator::OnJobTerminal(
    std::string_view step_kind,
    std::int64_t job_id,
    const simcore::PRResult& result,
    AdapterChainTrace* trace,
    std::string* error_out) const {
    if (registry_ == nullptr) return std::nullopt;
    const auto* descriptor = registry_->FindForStepKind(step_kind);
    if (descriptor == nullptr || descriptor->result_mapper == nullptr) {
        return std::nullopt;
    }

    const auto result_ini = descriptor->result_mapper->BuildResultIniFromPrResult(job_id, result);
    return OnJobTerminal(step_kind, job_id, result_ini, trace, error_out);
}

AdapterChainOrchestrator::StepTerminalResult AdapterChainOrchestrator::OnStepTerminal(
    std::string_view step_kind,
    const programdb::WorkflowTransitionContext& context,
    const StepCompletionSnapshot& snapshot,
    AdapterChainTrace* trace) const {
    StepTerminalResult result{};
    const auto* descriptor = registry_ != nullptr ? registry_->FindForStepKind(step_kind) : nullptr;

    if (completion_gate_ == nullptr) {
        result.gate.can_transition = true;
    } else {
        result.gate = completion_gate_->Evaluate(snapshot);
    }

    if (snapshot.failed_total > 0
        && snapshot.discovered_total > 0
        && snapshot.terminal_total >= snapshot.discovered_total) {
        result.gate.can_transition = true;
        const int succeeded_total = snapshot.terminal_total - snapshot.failed_total;
        result.gate.terminal_fail = succeeded_total <= 0
            || descriptor == nullptr
            || !descriptor->allow_mixed_success_failed_transition;
        result.gate.blocked_reason.reset();
    }

    if (!result.gate.can_transition || result.gate.terminal_fail || registry_ == nullptr) {
        return result;
    }

    if (descriptor == nullptr || descriptor->workflow_transition == nullptr) {
        return result;
    }

    if (trace) trace->transition_handler_invoked = true;
    result.transition = descriptor->workflow_transition->EvaluateTransition(context);
    return result;
}

} // namespace simcore::db::execution::workflow
