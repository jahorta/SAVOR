#pragma once

#include <string>
#include <vector>

namespace simcore::db::execution::workflow {

struct WorkflowOutcomeItem {
    std::string step_key;
    std::string outcome;
};

struct WorkflowParityMismatch {
    std::string step_key;
    std::string legacy_outcome;
    std::string workflow_outcome;
    std::string category;
};

struct WorkflowParityReport {
    int compared_steps = 0;
    int matched_steps = 0;
    std::vector<WorkflowParityMismatch> mismatches;
};

struct WorkflowPromotionEvidence {
    int parity_compared_steps = 0;
    int parity_matched_steps = 0;
    bool recovery_passed = false;
    bool integrity_passed = false;
    double readiness_scan_p95_ms = 0.0;
    double readiness_scan_threshold_ms = 0.0;
};

struct WorkflowPromotionDecision {
    bool approved = false;
    double parity_percent = 0.0;
    std::vector<std::string> blockers;
};

WorkflowParityReport CompareLegacyAndWorkflowOutcomes(
    const std::vector<WorkflowOutcomeItem>& legacy,
    const std::vector<WorkflowOutcomeItem>& workflow);

WorkflowPromotionDecision EvaluateWorkflowPromotionGate(const WorkflowPromotionEvidence& evidence);
std::string BuildWorkflowPromotionDecisionJson(
    const WorkflowPromotionEvidence& evidence,
    const WorkflowPromotionDecision& decision);

} // namespace simcore::db::execution::workflow
