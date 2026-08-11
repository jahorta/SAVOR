#include "WorkflowStepCompletionGate.h"

namespace savor::db::execution::workflow {
namespace {

std::string GateKey(const StepCompletionSnapshot& snapshot) {
    return std::to_string(snapshot.workflow_step_id) + ":"
        + std::to_string(snapshot.job_set_id);
}

} // namespace

StepCompletionGateDecision StepCompletionGateService::Evaluate(
    const StepCompletionSnapshot& snapshot) {
    StepCompletionGateDecision decision{};

    if (snapshot.discovered_total <= 0) {
        decision.can_transition = true;
        return decision;
    }

    if (snapshot.expected_total > 0
        && snapshot.expected_total != snapshot.discovered_total) {
        const auto key = GateKey(snapshot);
        const int attempts = ++mismatch_attempts_[key];
        decision.can_transition = false;
        if (attempts == 1) {
            decision.blocked_reason = "STEP_BLOCKED_COUNT_MISMATCH";
        } else {
            decision.blocked_reason =
                "STEP_BLOCKED_COUNT_MISMATCH_TERMINAL_FAIL";
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

} // namespace savor::db::execution::workflow
