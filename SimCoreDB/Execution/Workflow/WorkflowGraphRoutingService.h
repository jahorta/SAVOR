#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "WorkflowOrchestration.h"

namespace simcore::db {
struct IExecutionDb;
struct IAuthoringDb;
}

namespace simcore::db::execution::workflow {

struct WorkflowGraphRoutingResult {
    bool graph_instance = false;
    bool routed_input_binding = false;
    bool advanced_ready_step = false;
    bool workflow_completed = false;
    std::optional<std::string> blocked_reason;
};

class WorkflowGraphRoutingService {
public:
    WorkflowGraphRoutingService(
        simcore::db::IExecutionDb* execution_db,
        simcore::db::IAuthoringDb* authoring_db,
        IWorkflowOrchestrationQueryService* query_service,
        IWorkflowOrchestrationCommandService* command_service);

    bool RouteTerminalStep(
        const WorkflowStepTerminalSnapshot& snapshot,
        WorkflowGraphRoutingResult* result_out,
        std::string* error_out) const;

private:
    simcore::db::IExecutionDb* execution_db_ = nullptr;
    simcore::db::IAuthoringDb* authoring_db_ = nullptr;
    IWorkflowOrchestrationQueryService* query_service_ = nullptr;
    IWorkflowOrchestrationCommandService* command_service_ = nullptr;
};

} // namespace simcore::db::execution::workflow
