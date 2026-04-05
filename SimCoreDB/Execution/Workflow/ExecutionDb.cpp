#include "ExecutionDb.h"

#include "../../Common/Events/EventPayloadDispatch.h"
#include "../../Common/Events/EventPayloadValidation.h"
#include "SqliteWorkflowOrchestration.h"

namespace simcore::db::execution::workflow {
namespace {

struct Statement {
    sqlite3_stmt* st = nullptr;
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }
};

bool Prepare(sqlite3* db, const char* sql, Statement* st) {
    return sqlite3_prepare_v2(db, sql, -1, &st->st, nullptr) == SQLITE_OK;
}

std::optional<events::ExecutionWorkflowJobPayloadView> ResolveWorkflowEventPayload(sqlite3* db, std::int64_t payload_ref_id) {
    Statement st;
    if (!Prepare(db,
        "SELECT e.workflow_instance_id, COALESCE(e.workflow_step_id, 0), "
        "COALESCE(s.job_set_id, 0) "
        "FROM exec_workflow_event e "
        "LEFT JOIN exec_workflow_step s ON s.workflow_step_id=e.workflow_step_id "
        "WHERE e.workflow_event_id=?1;",
        &st)) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::ExecutionWorkflowJobPayloadView view{};
    view.workflow_instance_id = sqlite3_column_int64(st.st, 0);
    view.workflow_step_id = sqlite3_column_int64(st.st, 1);
    view.job_set_id = sqlite3_column_int64(st.st, 2);
    return view;
}

std::optional<events::ExecutionWorkflowJobPayloadView> ResolveJobSetPayload(sqlite3* db, std::int64_t payload_ref_id) {
    Statement st;
    if (!Prepare(db, "SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1;", &st)) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::ExecutionWorkflowJobPayloadView view{};
    view.job_set_id = sqlite3_column_int64(st.st, 0);
    return view;
}

std::optional<events::ExecutionWorkflowJobPayloadView> ResolveJobPayload(sqlite3* db, std::int64_t payload_ref_id) {
    Statement st;
    if (!Prepare(db,
        "SELECT job_id, job_set_id "
        "FROM exec_job "
        "WHERE job_id=?1;",
        &st)) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::ExecutionWorkflowJobPayloadView view{};
    view.job_id = sqlite3_column_int64(st.st, 0);
    view.job_set_id = sqlite3_column_int64(st.st, 1);
    return view;
}

} // namespace

ExecutionDb::ExecutionDb(sqlite3* db)
    : db_(db)
    , query_service_(std::make_unique<SqliteWorkflowOrchestrationQueryService>(db_))
    , command_service_(std::make_unique<SqliteWorkflowOrchestrationCommandService>(db_))
    , job_command_service_(std::make_unique<jobs::SqliteJobEventCommandService>(db_)) {
}

IWorkflowOrchestrationQueryService* ExecutionDb::WorkflowQueryService() {
    return query_service_.get();
}

IWorkflowOrchestrationCommandService* ExecutionDb::WorkflowCommandService() {
    return command_service_.get();
}

jobs::IJobEventCommandService* ExecutionDb::JobCommandService() {
    return job_command_service_.get();
}

std::optional<events::ExecutionWorkflowJobPayloadView> ExecutionDb::ResolveExecutionWorkflowJobPayload(
    const events::EventEnvelope& envelope) const {
    if (!events::ValidateExecutionWorkflowJobPayloadV1(envelope)) {
        return std::nullopt;
    }
    return ResolveExecutionWorkflowJobPayload(
        envelope.event_type,
        envelope.event_version,
        envelope.payload_ref_kind,
        envelope.payload_ref_id);
}

std::optional<events::ExecutionWorkflowJobPayloadView> ExecutionDb::ResolveExecutionWorkflowJobPayload(
    std::string_view event_type,
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    const auto contract = events::ResolvePayloadResolverContract(event_type, event_version);
    if (!contract.has_value() || contract.value() != events::PayloadResolverContract::ExecutionWorkflowJobV1) {
        return std::nullopt;
    }
    if (payload_ref_id <= 0 || db_ == nullptr) {
        return std::nullopt;
    }

    if (payload_ref_kind == "workflow_event") {
        return ResolveWorkflowEventPayload(db_, payload_ref_id);
    }
    if (payload_ref_kind == "job_set") {
        return ResolveJobSetPayload(db_, payload_ref_id);
    }
    if (payload_ref_kind == "job") {
        return ResolveJobPayload(db_, payload_ref_id);
    }

    return std::nullopt;
}

} // namespace simcore::db::execution::workflow
