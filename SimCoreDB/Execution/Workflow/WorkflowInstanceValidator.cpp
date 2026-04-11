#include "WorkflowInstanceValidator.h"

#include <set>
#include <sstream>

namespace simcore::db::execution::workflow {

bool WorkflowInstanceValidator::ValidateAvailableInputs(
    const WorkflowDefinition& definition,
    const std::vector<std::string>& available_inputs,
    std::string* error_out) const {
    std::set<std::string> available(available_inputs.begin(), available_inputs.end());

    for (const auto& required_initial_input : definition.initial_inputs) {
        if (available.find(required_initial_input) == available.end()) {
            if (error_out) *error_out = "missing required workflow input: " + required_initial_input;
            return false;
        }
    }

    for (const auto& step : definition.steps) {
        for (const auto& required_input : step.required_inputs) {
            if (available.find(required_input) == available.end()) {
                std::ostringstream oss;
                oss << "step '" << step.step_key << "' is missing required input '" << required_input << "'";
                if (error_out) *error_out = oss.str();
                return false;
            }
        }
        for (const auto& output : step.provided_outputs) {
            available.insert(output);
        }
    }

    return true;
}

} // namespace simcore::db::execution::workflow
