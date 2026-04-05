#pragma once

#include <memory>

#include <sqlite3.h>

#include "../IExecutionDb.h"
#include "SqliteWorkflowOrchestration.h"

namespace simcore::db::execution::workflow {

class SqliteWorkflowOrchestrationQueryService;
class SqliteWorkflowOrchestrationCommandService;

class ExecutionDb final : public simcore::db::IExecutionDb {
public:
    explicit ExecutionDb(sqlite3* db);

    IWorkflowOrchestrationQueryService* WorkflowQueryService() override;
    IWorkflowOrchestrationCommandService* WorkflowCommandService() override;

private:
    sqlite3* db_ = nullptr;
    std::unique_ptr<SqliteWorkflowOrchestrationQueryService> query_service_;
    std::unique_ptr<SqliteWorkflowOrchestrationCommandService> command_service_;
};

} // namespace simcore::db::execution::workflow
