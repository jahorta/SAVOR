#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace simcore::db::execution::workflow {

struct WorkflowStepDefinition {
    std::string step_key;
    std::string step_kind;
    std::vector<std::string> dependencies;
    std::vector<std::string> required_inputs;
    std::vector<std::string> provided_outputs;
    std::optional<std::string> guard_kind;
    std::optional<std::string> guard_value;
    int max_attempts = 1;
};

struct WorkflowDefinition {
    std::string workflow_kind;
    std::vector<std::string> initial_inputs;
    std::vector<WorkflowStepDefinition> steps;
};

WorkflowDefinition BuildSeedProbeChainDefinition();
WorkflowDefinition BuildTasMovieChainDefinition();
WorkflowDefinition BuildTasMovieSeedProbeChainDefinition();
bool ValidateWorkflowDefinition(const WorkflowDefinition& definition, std::string* error_out);

class WorkflowDefinitionRegistry {
public:
    bool RegisterDefinition(WorkflowDefinition definition, std::string* error_out);
    const WorkflowDefinition* Find(std::string_view workflow_kind) const;
    bool RegisterSeedProbeDefaults(std::string* error_out);
    bool RegisterTasMovieDefaults(std::string* error_out);

private:
    std::unordered_map<std::string, WorkflowDefinition> definitions_;
};

} // namespace simcore::db::execution::workflow
