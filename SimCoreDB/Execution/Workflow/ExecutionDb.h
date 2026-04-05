#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

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
    std::optional<events::ExecutionWorkflowJobPayloadView> ResolveExecutionWorkflowJobPayload(
        const events::EventEnvelope& envelope) const override;
    std::optional<events::ExecutionWorkflowJobPayloadView> ResolveExecutionWorkflowJobPayload(
        std::string_view event_type,
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;

private:
    sqlite3* db_ = nullptr;
    std::unique_ptr<SqliteWorkflowOrchestrationQueryService> query_service_;
    std::unique_ptr<SqliteWorkflowOrchestrationCommandService> command_service_;
};

} // namespace simcore::db::execution::workflow
