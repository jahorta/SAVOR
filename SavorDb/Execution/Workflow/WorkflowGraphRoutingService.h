#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "WorkflowOrchestration.h"

namespace savor::db {
struct IExecutionDb;
struct IAuthoringDb;
}

namespace savor::db::execution::workflow {

enum class WorkflowGraphRoutingFailureClass {
    None = 0,
    GraphConstruction,
    Operation,
};

struct WorkflowGraphRoutingFailure {
    WorkflowGraphRoutingFailureClass failure_class =
        WorkflowGraphRoutingFailureClass::None;
    std::string code;
    std::string message;
};

struct WorkflowGraphRoutingResult {
    bool graph_instance = false;
    bool routed_input_binding = false;
    bool advanced_ready_step = false;
    int skipped_step_count = 0;
    bool workflow_completed = false;
    std::optional<std::string> blocked_reason;
    std::optional<WorkflowGraphRoutingFailure> failure;
};

class WorkflowGraphRoutingService {
public:
    WorkflowGraphRoutingService(
        savor::db::IExecutionDb* execution_db,
        savor::db::IAuthoringDb* authoring_db,
        IWorkflowOrchestrationQueryService* query_service,
        IWorkflowOrchestrationCommandService* command_service,
        int successor_step_priority_boost = 10);

    bool RouteTerminalStep(
        const WorkflowStepSettlementSnapshot& snapshot,
        WorkflowGraphRoutingResult* result_out,
        std::string* error_out) const;

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IAuthoringDb* authoring_db_ = nullptr;
    IWorkflowOrchestrationQueryService* query_service_ = nullptr;
    IWorkflowOrchestrationCommandService* command_service_ = nullptr;
    int successor_step_priority_boost_ = 10;
};

} // namespace savor::db::execution::workflow
