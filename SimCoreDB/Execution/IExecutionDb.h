#pragma once

namespace simcore::db::execution::workflow {
struct IWorkflowOrchestrationCommandService;
struct IWorkflowOrchestrationQueryService;
}

namespace simcore::db {

struct IExecutionDb {
    virtual ~IExecutionDb() = default;

    virtual execution::workflow::IWorkflowOrchestrationQueryService* WorkflowQueryService() = 0;
    virtual execution::workflow::IWorkflowOrchestrationCommandService* WorkflowCommandService() = 0;
};

} // namespace simcore::db
