#include "WorkflowParityDiagnostics.h"

#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace simcore::db::execution::workflow {

WorkflowParityReport CompareLegacyAndWorkflowOutcomes(
    const std::vector<WorkflowOutcomeItem>& legacy,
    const std::vector<WorkflowOutcomeItem>& workflow) {
    WorkflowParityReport report;

    std::unordered_map<std::string, std::string> legacy_by_key;
    std::unordered_map<std::string, std::string> workflow_by_key;

    for (const auto& item : legacy) {
        legacy_by_key[item.step_key] = item.outcome;
    }
    for (const auto& item : workflow) {
        workflow_by_key[item.step_key] = item.outcome;
    }

    for (const auto& [step_key, legacy_outcome] : legacy_by_key) {
        ++report.compared_steps;
        const auto workflow_it = workflow_by_key.find(step_key);
        if (workflow_it == workflow_by_key.end()) {
            report.mismatches.push_back({
                .step_key = step_key,
                .legacy_outcome = legacy_outcome,
                .workflow_outcome = "",
                .category = "missing_workflow_step",
            });
            continue;
        }

        if (workflow_it->second == legacy_outcome) {
            ++report.matched_steps;
        } else {
            report.mismatches.push_back({
                .step_key = step_key,
                .legacy_outcome = legacy_outcome,
                .workflow_outcome = workflow_it->second,
                .category = "outcome_mismatch",
            });
        }
    }

    for (const auto& [step_key, workflow_outcome] : workflow_by_key) {
        if (legacy_by_key.find(step_key) != legacy_by_key.end()) {
            continue;
        }
        ++report.compared_steps;
        report.mismatches.push_back({
            .step_key = step_key,
            .legacy_outcome = "",
            .workflow_outcome = workflow_outcome,
            .category = "unexpected_workflow_step",
        });
    }

    return report;
}

WorkflowPromotionDecision EvaluateWorkflowPromotionGate(const WorkflowPromotionEvidence& evidence) {
    WorkflowPromotionDecision decision;

    if (evidence.parity_compared_steps <= 0) {
        decision.blockers.push_back("parity_no_comparisons");
        return decision;
    }

    decision.parity_percent =
        (static_cast<double>(evidence.parity_matched_steps) / static_cast<double>(evidence.parity_compared_steps)) * 100.0;

    if (decision.parity_percent < 99.0) {
        decision.blockers.push_back("parity_below_99_percent");
    }
    if (!evidence.recovery_passed) {
        decision.blockers.push_back("recovery_failed");
    }
    if (!evidence.integrity_passed) {
        decision.blockers.push_back("integrity_failed");
    }
    if (evidence.readiness_scan_threshold_ms > 0.0
        && evidence.readiness_scan_p95_ms > evidence.readiness_scan_threshold_ms) {
        decision.blockers.push_back("readiness_latency_above_threshold");
    }

    decision.approved = decision.blockers.empty();
    return decision;
}

std::string BuildWorkflowPromotionDecisionJson(
    const WorkflowPromotionEvidence& evidence,
    const WorkflowPromotionDecision& decision) {
    std::ostringstream out;
    out << "{";
    out << "\"approved\":" << (decision.approved ? "true" : "false") << ",";
    out << "\"parity_percent\":" << std::fixed << std::setprecision(2) << decision.parity_percent << ",";
    out << "\"parity_compared_steps\":" << evidence.parity_compared_steps << ",";
    out << "\"parity_matched_steps\":" << evidence.parity_matched_steps << ",";
    out << "\"recovery_passed\":" << (evidence.recovery_passed ? "true" : "false") << ",";
    out << "\"integrity_passed\":" << (evidence.integrity_passed ? "true" : "false") << ",";
    out << "\"readiness_scan_p95_ms\":" << std::fixed << std::setprecision(2) << evidence.readiness_scan_p95_ms << ",";
    out << "\"readiness_scan_threshold_ms\":" << std::fixed << std::setprecision(2) << evidence.readiness_scan_threshold_ms << ",";
    out << "\"blockers\":[";
    for (size_t i = 0; i < decision.blockers.size(); ++i) {
        if (i > 0) out << ",";
        out << "\"" << decision.blockers[i] << "\"";
    }
    out << "]";
    out << "}";
    return out.str();
}

} // namespace simcore::db::execution::workflow
