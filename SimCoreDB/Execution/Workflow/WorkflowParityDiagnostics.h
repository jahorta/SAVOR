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

WorkflowParityReport CompareLegacyAndWorkflowOutcomes(
    const std::vector<WorkflowOutcomeItem>& legacy,
    const std::vector<WorkflowOutcomeItem>& workflow);

} // namespace simcore::db::execution::workflow
