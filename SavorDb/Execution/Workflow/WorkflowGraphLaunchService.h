#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "WorkflowLaunchContract.h"

namespace savor::db {
struct IAuthoringDb;
struct IExecutionDb;
}

namespace savor::db::execution::workflow {

struct WorkflowGraphLaunchRequest {
    std::int64_t workflow_graph_revision_id = 0;
    std::string created_by;
    std::optional<std::string> launch_key;
    std::vector<WorkflowLaunchInputValue> input_bindings;
    std::vector<WorkflowLaunchArgumentValue> arguments;
};

class WorkflowGraphLaunchService {
public:
    WorkflowGraphLaunchService(IAuthoringDb* authoring, IExecutionDb* execution);

    bool Start(const WorkflowGraphLaunchRequest& request,
        std::int64_t* workflow_instance_id_out,
        std::string* error_out = nullptr) const;

private:
    IAuthoringDb* authoring_ = nullptr;
    IExecutionDb* execution_ = nullptr;
};

} // namespace savor::db::execution::workflow
