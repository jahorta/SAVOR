#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace simcore::db::execution::workflow {

struct WorkflowStepDefinition {
    std::string step_key;
    std::string step_kind;
    std::vector<std::string> dependencies;
    std::optional<std::string> guard_kind;
    std::optional<std::string> guard_value;
    int max_attempts = 1;
};

struct WorkflowDefinition {
    std::string workflow_kind;
    std::vector<WorkflowStepDefinition> steps;
};

WorkflowDefinition BuildSeedProbeChainDefinition();
bool ValidateWorkflowDefinition(const WorkflowDefinition& definition, std::string* error_out);

} // namespace simcore::db::execution::workflow
