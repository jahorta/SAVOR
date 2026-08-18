#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "WorkflowComposition.h"
#include "../../Authoring/IAuthoringDb.h"

namespace savor::db::execution::workflow {

struct WorkflowLaunchInputValue {
    std::string node_key;
    std::string input_key;
    std::string data_kind;
    std::string ref_kind;
    std::int64_t ref_id = 0;
    std::string source_kind = "external";
};

struct WorkflowLaunchArgumentValue {
    std::string node_key;
    std::string argument_key;
    std::string value_type;
    std::optional<std::int64_t> integer_value;
    std::optional<std::string> text_value;
    std::string source_kind = "launcher";
};

struct WorkflowLaunchContractValidation {
    bool valid = false;
    std::vector<WorkflowLaunchArgumentValue> normalized_arguments;
    std::vector<std::string> issues;
};

class WorkflowLaunchContractValidator {
public:
    static WorkflowLaunchContractValidation Validate(
        const savor::db::WorkflowGraphSnapshot& graph,
        const WorkflowUnitRegistry& registry,
        const std::vector<WorkflowLaunchInputValue>& inputs,
        const std::vector<WorkflowLaunchArgumentValue>& arguments);
};

} // namespace savor::db::execution::workflow
