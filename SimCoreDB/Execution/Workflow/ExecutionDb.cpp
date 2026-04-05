#include "Execution/Workflow/ExecutionDb.h"

#include "Execution/Workflow/SqliteWorkflowOrchestration.h"

namespace simcore::db::execution::workflow {

ExecutionDb::ExecutionDb(sqlite3* db)
    : db_(db)
    , query_service_(std::make_unique<SqliteWorkflowOrchestrationQueryService>(db_))
    , command_service_(std::make_unique<SqliteWorkflowOrchestrationCommandService>(db_)) {
}

IWorkflowOrchestrationQueryService* ExecutionDb::WorkflowQueryService() {
    return query_service_.get();
}

IWorkflowOrchestrationCommandService* ExecutionDb::WorkflowCommandService() {
    return command_service_.get();
}

} // namespace simcore::db::execution::workflow
