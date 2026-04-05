#include "Execution/Workflow/WorkflowParityDiagnostics.h"

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

} // namespace simcore::db::execution::workflow
