#pragma once

#include <string>
#include <vector>

#include "SeedProbeWorkflowDefinition.h"

namespace simcore::db::execution::workflow {

class WorkflowInstanceValidator {
public:
    bool ValidateAvailableInputs(
        const WorkflowDefinition& definition,
        const std::vector<std::string>& available_inputs,
        std::string* error_out) const;
};

} // namespace simcore::db::execution::workflow
