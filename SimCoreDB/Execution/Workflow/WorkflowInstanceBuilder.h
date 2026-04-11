#pragma once

#include <optional>
#include <string>
#include <vector>

#include "WorkflowOrchestration.h"
#include "SeedProbeWorkflowDefinition.h"
#include "WorkflowInstanceValidator.h"

namespace simcore::db::execution::workflow {

struct WorkflowDefinitionInstantiationInput {
    std::string workflow_kind;
    std::string root_scope_kind;
    std::optional<std::int64_t> root_scope_id;
    std::optional<std::string> input_ref_kind;
    std::optional<std::int64_t> input_ref_id;
    std::string created_by;
    std::int64_t created_at_utc = 0;
    std::vector<std::string> available_inputs;
};

class WorkflowInstanceBuilder {
public:
    WorkflowInstanceBuilder(
        const WorkflowDefinitionRegistry* registry,
        const WorkflowInstanceValidator* validator = nullptr);

    bool BuildCreateCommand(
        const WorkflowDefinitionInstantiationInput& input,
        WorkflowCreateInstanceCommand* command_out,
        std::string* error_out) const;

private:
    const WorkflowDefinitionRegistry* registry_ = nullptr;
    WorkflowInstanceValidator owned_validator_{};
    const WorkflowInstanceValidator* validator_ = nullptr;
};

} // namespace simcore::db::execution::workflow
