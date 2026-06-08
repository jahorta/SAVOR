#include "SqliteExecutionDb.h"

#include "../../Common/Events/EventPayloadDispatch.h"
#include "../../Common/Events/EventPayloadValidation.h"
#include "SqliteWorkflowOrchestration.h"
#include "WorkflowRecoveryService.h"

#include <sstream>

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


std::int64_t CurrentUtcMs(sqlite3* db) {
    Statement st;
    if (sqlite3_prepare_v2(db,
        "SELECT CAST(unixepoch('now') * 1000 AS INTEGER);",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return 0;
    }
    return sqlite3_column_int64(st.st, 0);
}


std::int64_t NowUtcMillis() {
    return types::UtcNow().time_since_epoch().count();
}

bool ExecuteSql(sqlite3* db, const char* sql, std::string* error_out) {
    char* sqlite_error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &sqlite_error);
    if (rc != SQLITE_OK) {
        if (error_out) {
            *error_out = sqlite_error ? sqlite_error : sqlite3_errmsg(db);
        }
        sqlite3_free(sqlite_error);
        return false;
    }
    sqlite3_free(sqlite_error);
    return true;
}

bool Commit(sqlite3* db, std::string* error_out) {
    return ExecuteSql(db, "COMMIT;", error_out);
}

void Rollback(sqlite3* db) {
    (void)sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
}

bool InsertJobActionEventAndOutbox(
    sqlite3* db,
    std::int64_t job_id,
    std::int64_t job_set_id,
    const char* event_type,
    const char* message,
    std::string* error_out) {
    const auto now = CurrentUtcMs(db);
    Statement job_event;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO exec_job_event(job_id,event_kind,event_ts_utc,message,artifact_id) "
            "VALUES(?1,?2,?3,?4,NULL);",
            -1,
            &job_event.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_int64(job_event.st, 1, job_id);
    sqlite3_bind_text(job_event.st, 2, event_type, -1, SQLITE_STATIC);
    sqlite3_bind_int64(job_event.st, 3, now);
    sqlite3_bind_text(job_event.st, 4, message, -1, SQLITE_STATIC);
    if (sqlite3_step(job_event.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    const auto job_event_id = sqlite3_last_insert_rowid(db);

    std::ostringstream event_id;
    event_id << "execution-" << event_type << "-" << job_id << "-" << job_event_id;
    const auto event_id_value = event_id.str();
    const auto aggregate_id = std::to_string(job_id);
    const auto correlation_id = "job-set-" + std::to_string(job_set_id);
    const auto causation_id = std::string("job-action-") + message + "-" + std::to_string(job_id);

    Statement outbox;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO exec_outbox_message("
            "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
            "VALUES(?1,?2,1,'Execution','job',?3,?4,?5,?6,'job',?7);",
            -1,
            &outbox.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_text(outbox.st, 1, event_id_value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outbox.st, 2, event_type, -1, SQLITE_STATIC);
    sqlite3_bind_text(outbox.st, 3, aggregate_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outbox.st, 4, correlation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outbox.st, 5, causation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(outbox.st, 6, now);
    sqlite3_bind_int64(outbox.st, 7, job_id);
    if (sqlite3_step(outbox.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

struct JobStateForAction {
    std::int64_t job_set_id = 0;
    std::string state;
};

std::optional<JobStateForAction> GetJobStateForAction(sqlite3* db, std::int64_t job_id, std::string* error_out) {
    Statement st;
    if (sqlite3_prepare_v2(db, "SELECT job_set_id,state FROM exec_job WHERE job_id=?1;", -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, job_id);
    const auto rc = sqlite3_step(st.st);
    if (rc != SQLITE_ROW) {
        if (error_out) *error_out = rc == SQLITE_DONE ? "job not found" : sqlite3_errmsg(db);
        return std::nullopt;
    }
    JobStateForAction row{};
    row.job_set_id = sqlite3_column_int64(st.st, 0);
    const auto* state = sqlite3_column_text(st.st, 1);
    row.state = state != nullptr ? reinterpret_cast<const char*>(state) : "";
    return row;
}

std::optional<events::ExecutionWorkflowJobPayloadView> ResolveWorkflowEventPayload(sqlite3* db, std::int64_t payload_ref_id) {
    Statement st;
    if (sqlite3_prepare_v2(db,
        "SELECT e.workflow_instance_id, COALESCE(e.workflow_step_id, 0), "
        "COALESCE(s.job_set_id, 0) "
        "FROM exec_workflow_event e "
        "LEFT JOIN exec_workflow_step s ON s.workflow_step_id=e.workflow_step_id "
        "WHERE e.workflow_event_id=?1;",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
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
    if (sqlite3_prepare_v2(db,
        "SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1;",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
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
    if (sqlite3_prepare_v2(db,
        "SELECT job_id, job_set_id "
        "FROM exec_job "
        "WHERE job_id=?1;",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
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

bool ResolveWorkflowStepForJobSetAncestry(
    sqlite3* db,
    std::int64_t job_set_id,
    ClaimedExecutionJob* claimed,
    std::string* error_out) {
    if (claimed == nullptr) {
        if (error_out) *error_out = "claimed job output is required";
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(db,
        "WITH RECURSIVE job_set_ancestry(job_set_id, parent_job_set_id, depth) AS ("
        "  SELECT js.job_set_id, js.parent_job_set_id, 0 "
        "  FROM exec_job_set js "
        "  WHERE js.job_set_id=?1 "
        "  UNION ALL "
        "  SELECT parent.job_set_id, parent.parent_job_set_id, job_set_ancestry.depth + 1 "
        "  FROM exec_job_set parent "
        "  JOIN job_set_ancestry ON parent.job_set_id=job_set_ancestry.parent_job_set_id "
        "  WHERE job_set_ancestry.parent_job_set_id IS NOT NULL "
        "    AND job_set_ancestry.depth < 64"
        ") "
        "SELECT s.workflow_instance_id, s.workflow_step_id, s.step_key, s.step_kind, s.priority "
        "FROM job_set_ancestry a "
        "JOIN exec_workflow_step s ON s.job_set_id=a.job_set_id "
        "ORDER BY a.depth ASC "
        "LIMIT 1;",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }

    sqlite3_bind_int64(st.st, 1, job_set_id);
    const auto rc = sqlite3_step(st.st);
    if (rc == SQLITE_ROW) {
        claimed->workflow_instance_id = sqlite3_column_int64(st.st, 0);
        claimed->workflow_step_id = sqlite3_column_int64(st.st, 1);
        const auto* step_key = sqlite3_column_text(st.st, 2);
        const auto* step_kind = sqlite3_column_text(st.st, 3);
        claimed->workflow_step_key = step_key ? reinterpret_cast<const char*>(step_key) : "";
        claimed->workflow_step_kind = step_kind ? reinterpret_cast<const char*>(step_kind) : "";
        claimed->workflow_step_priority = sqlite3_column_int(st.st, 4);
        return true;
    }
    if (rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }

    if (error_out) {
        *error_out = "workflow step not found for job_set ancestry job_set_id=" + std::to_string(job_set_id);
    }
    return false;
}

} // namespace

SqliteExecutionDb::SqliteExecutionDb(sqlite3* db)
    : db_(db)
    , query_service_(std::make_unique<SqliteWorkflowOrchestrationQueryService>(db_))
    , command_service_(std::make_unique<SqliteWorkflowOrchestrationCommandService>(db_))
    , job_command_service_(std::make_unique<jobs::SqliteJobEventCommandService>(db_)) {
}


bool SqliteExecutionDb::ValidationExecuteSql(std::string_view sql, std::string* error_out) const {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    char* sqlite_err = nullptr;
    const int rc = sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &sqlite_err);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite_err ? sqlite_err : sqlite3_errmsg(db_);
        sqlite3_free(sqlite_err);
        return false;
    }
    sqlite3_free(sqlite_err);
    return true;
}

bool SqliteExecutionDb::ValidationQueryInt(std::string_view sql, std::int64_t* value_out, std::string* error_out) const {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (value_out == nullptr) {
        if (error_out) *error_out = "value_out is required";
        return false;
    }
    Statement st;
    if (sqlite3_prepare_v2(db_, std::string(sql).c_str(), -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        if (error_out) *error_out = "query returned no row";
        return false;
    }
    *value_out = sqlite3_column_int64(st.st, 0);
    return true;
}

bool SqliteExecutionDb::ValidationQueryText(std::string_view sql, std::string* value_out, std::string* error_out) const {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (value_out == nullptr) {
        if (error_out) *error_out = "value_out is required";
        return false;
    }
    Statement st;
    if (sqlite3_prepare_v2(db_, std::string(sql).c_str(), -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        if (error_out) *error_out = "query returned no row";
        return false;
    }
    const auto* text = sqlite3_column_text(st.st, 0);
    *value_out = text ? reinterpret_cast<const char*>(text) : "";
    return true;
}

bool SqliteExecutionDb::ValidationExecuteInvariantRemediation(
    const WorkflowInvariantRemediationCommand& command,
    bool* reopened_out,
    std::string* error_out) {
    WorkflowRecoveryService recovery(db_);
    return recovery.ExecuteInvariantRemediation(command, command_service_.get(), reopened_out, error_out);
}

IWorkflowOrchestrationQueryService* SqliteExecutionDb::WorkflowQueryService() {
    return query_service_.get();
}

IWorkflowOrchestrationCommandService* SqliteExecutionDb::WorkflowCommandService() {
    return command_service_.get();
}

jobs::IJobEventCommandService* SqliteExecutionDb::JobCommandService() {
    return job_command_service_.get();
}

bool SqliteExecutionDb::CreateWorkflowInstance(
    const WorkflowCreateInstanceCommand& command,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (command_service_ == nullptr) {
        if (error_out) *error_out = "workflow command service unavailable";
        return false;
    }
    return command_service_->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
}

std::optional<ExecutionJobRecord> SqliteExecutionDb::GetJob(std::int64_t job_id) const {
    if (db_ == nullptr || job_id <= 0) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(db_,
        "SELECT job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, "
        "savestate_id, fingerprint, state, priority, attempts, max_attempts, queued_at_utc, input_ini "
        "FROM exec_job WHERE job_id=?1;",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    ExecutionJobRecord row{};
    row.job_id = sqlite3_column_int64(st.st, 0);
    row.job_set_id = sqlite3_column_int64(st.st, 1);
    row.program_kind = sqlite3_column_int(st.st, 2);
    row.program_version = sqlite3_column_int(st.st, 3);
    row.program_ref_kind = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 4));
    row.program_ref_id = sqlite3_column_int64(st.st, 5);
    if (sqlite3_column_type(st.st, 6) != SQLITE_NULL) {
        row.savestate_id = sqlite3_column_int64(st.st, 6);
    }
    row.fingerprint = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 7));
    row.state = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 8));
    row.priority = sqlite3_column_int(st.st, 9);
    row.attempts = sqlite3_column_int(st.st, 10);
    row.max_attempts = sqlite3_column_int(st.st, 11);
    row.queued_at_utc = sqlite3_column_int64(st.st, 12);
    const auto* input_ini = sqlite3_column_text(st.st, 13);
    row.input_ini = input_ini == nullptr ? "" : reinterpret_cast<const char*>(input_ini);
    return row;
}

std::vector<ExecutionJobEventRecord> SqliteExecutionDb::ListJobEvents(std::int64_t job_id, int limit) const {
    std::vector<ExecutionJobEventRecord> rows;
    if (db_ == nullptr || job_id <= 0) {
        return rows;
    }
    if (limit <= 0) {
        limit = 128;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT job_event_id,job_id,event_kind,event_ts_utc,COALESCE(message,''),artifact_id "
            "FROM exec_job_event "
            "WHERE job_id=?1 "
            "ORDER BY job_event_id DESC "
            "LIMIT ?2;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, job_id);
    sqlite3_bind_int(st.st, 2, limit);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        ExecutionJobEventRecord row{};
        row.job_event_id = sqlite3_column_int64(st.st, 0);
        row.job_id = sqlite3_column_int64(st.st, 1);
        const auto* kind = sqlite3_column_text(st.st, 2);
        const auto* message = sqlite3_column_text(st.st, 4);
        row.event_kind = kind != nullptr ? reinterpret_cast<const char*>(kind) : "";
        row.event_ts_utc = sqlite3_column_int64(st.st, 3);
        row.message = message != nullptr ? reinterpret_cast<const char*>(message) : "";
        if (sqlite3_column_type(st.st, 5) != SQLITE_NULL) {
            row.artifact_id = sqlite3_column_int64(st.st, 5);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::optional<std::string> SqliteExecutionDb::GetJobInputIni(std::int64_t job_id, std::string* error_out) const {
    const auto job = GetJob(job_id);
    if (!job.has_value()) {
        if (error_out) *error_out = "job not found";
        return std::nullopt;
    }
    if (error_out) error_out->clear();
    return job->input_ini;
}

bool SqliteExecutionDb::RequeueJob(std::int64_t job_id, std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (job_id <= 0) {
        if (error_out) *error_out = "job_id must be > 0";
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    const auto job = GetJobStateForAction(db_, job_id, error_out);
    if (!job.has_value()) {
        Rollback(db_);
        return false;
    }
    if (job->state == "QUEUED" || job->state == "CLAIMED" || job->state == "RUNNING" || job->state == "FAILED") {
        Rollback(db_);
        if (error_out) *error_out = "job cannot be requeued from state " + job->state;
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job "
            "SET state='QUEUED', claimed_by_token=NULL, lease_expires_at_utc=NULL, started_at_utc=NULL, ended_at_utc=NULL, error_code=NULL, error_text=NULL "
            "WHERE job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, job_id);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (!InsertJobActionEventAndOutbox(db_, job_id, job->job_set_id, "Execution.JobQueued.v1", "REQUEUE", error_out)) {
        Rollback(db_);
        return false;
    }
    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    return true;
}

bool SqliteExecutionDb::RestartFailedJob(std::int64_t job_id, std::optional<std::string> input_ini_override, std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (job_id <= 0) {
        if (error_out) *error_out = "job_id must be > 0";
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    const auto job = GetJobStateForAction(db_, job_id, error_out);
    if (!job.has_value()) {
        Rollback(db_);
        return false;
    }
    if (job->state != "FAILED") {
        Rollback(db_);
        if (error_out) *error_out = "restart requires FAILED job";
        return false;
    }

    const char* sql = input_ini_override.has_value()
        ? "UPDATE exec_job "
          "SET state='QUEUED', attempts=0, claimed_by_token=NULL, lease_expires_at_utc=NULL, started_at_utc=NULL, ended_at_utc=NULL, error_code=NULL, error_text=NULL, input_ini=?2 "
          "WHERE job_id=?1;"
        : "UPDATE exec_job "
          "SET state='QUEUED', attempts=0, claimed_by_token=NULL, lease_expires_at_utc=NULL, started_at_utc=NULL, ended_at_utc=NULL, error_code=NULL, error_text=NULL "
          "WHERE job_id=?1;";
    Statement st;
    if (sqlite3_prepare_v2(db_, sql, -1, &st.st, nullptr) != SQLITE_OK) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, job_id);
    if (input_ini_override.has_value()) {
        sqlite3_bind_text(st.st, 2, input_ini_override->c_str(), -1, SQLITE_TRANSIENT);
    }
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (!InsertJobActionEventAndOutbox(db_, job_id, job->job_set_id, "Execution.JobQueued.v1", "RESTART", error_out)) {
        Rollback(db_);
        return false;
    }
    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    return true;
}

bool SqliteExecutionDb::CancelQueuedOrClaimedJob(std::int64_t job_id, std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (job_id <= 0) {
        if (error_out) *error_out = "job_id must be > 0";
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    const auto job = GetJobStateForAction(db_, job_id, error_out);
    if (!job.has_value()) {
        Rollback(db_);
        return false;
    }
    if (job->state != "QUEUED" && job->state != "INTERRUPTED" && job->state != "CLAIMED") {
        Rollback(db_);
        if (error_out) *error_out = "job cannot be canceled from state " + job->state;
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job "
            "SET state='CANCELED', claimed_by_token=NULL, lease_expires_at_utc=NULL, ended_at_utc=?2 "
            "WHERE job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, job_id);
    sqlite3_bind_int64(st.st, 2, CurrentUtcMs(db_));
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (!InsertJobActionEventAndOutbox(db_, job_id, job->job_set_id, "Execution.JobCompleted.v1", "CANCEL", error_out)) {
        Rollback(db_);
        return false;
    }
    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    return true;
}

std::optional<ExecutionJobSetProgressDetails> SqliteExecutionDb::GetJobSetProgress(std::int64_t job_set_id) const {
    if (db_ == nullptr || job_set_id <= 0) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "WITH RECURSIVE job_set_descendants(job_set_id, depth) AS ("
            "  SELECT js.job_set_id, 0 FROM exec_job_set js WHERE js.job_set_id=?1 "
            "  UNION ALL "
            "  SELECT child.job_set_id, job_set_descendants.depth + 1 "
            "  FROM exec_job_set child "
            "  JOIN job_set_descendants ON child.parent_job_set_id=job_set_descendants.job_set_id "
            "  WHERE job_set_descendants.depth < 64"
            "), "
            "summary AS ("
            "  SELECT "
            "    COUNT(j.job_id) AS total_jobs, "
            "    COALESCE(SUM(CASE WHEN j.state IN ('COMPLETED','SUCCEEDED','SUCCEEDED_WINNER','SUPERSEDED','SUCCEEDED_DUPLICATE','FAILED','CANCELED') THEN 1 ELSE 0 END), 0) AS completed_jobs, "
            "    COALESCE(SUM(CASE WHEN j.state IN ('COMPLETED','SUCCEEDED','SUCCEEDED_WINNER','SUPERSEDED','SUCCEEDED_DUPLICATE') THEN 1 ELSE 0 END), 0) AS succeeded_jobs, "
            "    COALESCE(SUM(CASE WHEN j.state='FAILED' THEN 1 ELSE 0 END), 0) AS failed_jobs, "
            "    COALESCE(SUM(CASE WHEN j.state='CANCELED' THEN 1 ELSE 0 END), 0) AS canceled_jobs "
            "  FROM job_set_descendants d "
            "  LEFT JOIN exec_job j ON j.job_set_id=d.job_set_id"
            "), "
            "expected AS ("
            "  SELECT CASE "
            "    WHEN EXISTS(SELECT 1 FROM job_set_descendants WHERE depth > 0) "
            "      THEN (SELECT COALESCE(SUM(COALESCE(child.expected_total, 0)), 0) "
            "            FROM exec_job_set child JOIN job_set_descendants d ON d.job_set_id=child.job_set_id WHERE d.depth > 0) "
            "    ELSE COALESCE(js.expected_total, 0) "
            "  END AS expected_total "
            "  FROM exec_job_set js WHERE js.job_set_id=?1"
            ") "
            "SELECT ?1, summary.total_jobs, summary.completed_jobs, summary.succeeded_jobs, "
            "summary.failed_jobs, summary.canceled_jobs, expected.expected_total "
            "FROM summary, expected;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, job_set_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    ExecutionJobSetProgressDetails details{};
    details.job_set_id = sqlite3_column_int64(st.st, 0);
    details.total_jobs = sqlite3_column_int64(st.st, 1);
    details.completed_jobs = sqlite3_column_int64(st.st, 2);
    details.succeeded_jobs = sqlite3_column_int64(st.st, 3);
    details.failed_jobs = sqlite3_column_int64(st.st, 4);
    details.canceled_jobs = sqlite3_column_int64(st.st, 5);
    if (sqlite3_column_type(st.st, 6) != SQLITE_NULL) {
        details.expected_total = sqlite3_column_int64(st.st, 6);
    }
    return details;
}

std::vector<ExecutionChildJobSetProgressDetails> SqliteExecutionDb::GetChildJobSetProgress(std::int64_t parent_job_set_id) const {
    std::vector<ExecutionChildJobSetProgressDetails> rows;
    if (db_ == nullptr || parent_job_set_id <= 0) {
        return rows;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT js.job_set_id, js.purpose, COALESCE(js.meta_note, ''), "
            "COALESCE((SELECT COUNT(1) FROM exec_job j WHERE j.job_set_id=js.job_set_id), 0) AS total_jobs, "
            "COALESCE((SELECT COUNT(1) FROM exec_job j "
            "         WHERE j.job_set_id=js.job_set_id "
            "           AND j.state IN ('COMPLETED','SUCCEEDED','SUCCEEDED_WINNER','SUPERSEDED','SUCCEEDED_DUPLICATE','FAILED','CANCELED')), 0) AS completed_jobs, "
            "COALESCE((SELECT COUNT(1) FROM exec_job j "
            "         WHERE j.job_set_id=js.job_set_id "
            "           AND j.state IN ('COMPLETED','SUCCEEDED','SUCCEEDED_WINNER','SUPERSEDED','SUCCEEDED_DUPLICATE')), 0) AS succeeded_jobs, "
            "COALESCE((SELECT COUNT(1) FROM exec_job j WHERE j.job_set_id=js.job_set_id AND j.state='FAILED'), 0) AS failed_jobs, "
            "COALESCE((SELECT COUNT(1) FROM exec_job j WHERE j.job_set_id=js.job_set_id AND j.state='CANCELED'), 0) AS canceled_jobs, "
            "js.expected_total "
            "FROM exec_job_set js "
            "WHERE js.parent_job_set_id=?1 "
            "ORDER BY js.job_set_id ASC;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return rows;
    }

    sqlite3_bind_int64(st.st, 1, parent_job_set_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        ExecutionChildJobSetProgressDetails row{};
        row.job_set_id = sqlite3_column_int64(st.st, 0);
        const auto* purpose = sqlite3_column_text(st.st, 1);
        const auto* meta_note = sqlite3_column_text(st.st, 2);
        row.purpose = purpose ? reinterpret_cast<const char*>(purpose) : "";
        row.meta_note = meta_note ? reinterpret_cast<const char*>(meta_note) : "";
        row.total_jobs = sqlite3_column_int64(st.st, 3);
        row.completed_jobs = sqlite3_column_int64(st.st, 4);
        row.succeeded_jobs = sqlite3_column_int64(st.st, 5);
        row.failed_jobs = sqlite3_column_int64(st.st, 6);
        row.canceled_jobs = sqlite3_column_int64(st.st, 7);
        if (sqlite3_column_type(st.st, 8) != SQLITE_NULL) {
            row.expected_total = sqlite3_column_int64(st.st, 8);
        }
        constexpr std::string_view token = "expected_delta=";
        const auto pos = row.meta_note.find(token);
        if (pos != std::string::npos) {
            const auto start = pos + token.size();
            try {
                row.expected_delta = std::stoll(row.meta_note.substr(start));
            } catch (...) {
                row.expected_delta = std::nullopt;
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

bool SqliteExecutionDb::CreateJobSet(
    const CreateJobSetCommand& command,
    std::int64_t* job_set_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.program_kind <= 0) {
        if (error_out) *error_out = "program_kind must be > 0";
        return false;
    }
    if (command.purpose.empty()) {
        if (error_out) *error_out = "purpose is required";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto rollback = [&]() { (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr); };

    const auto now = command.created_at_utc > 0 ? command.created_at_utc : NowUtcMillis();
    Statement insert_set;
    if (sqlite3_prepare_v2(db_,
        "INSERT INTO exec_job_set(parent_job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);",
        -1,
        &insert_set.st,
        nullptr)
        != SQLITE_OK) {
        rollback();
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (command.parent_job_set_id.has_value()) sqlite3_bind_int64(insert_set.st, 1, *command.parent_job_set_id); else sqlite3_bind_null(insert_set.st, 1);
    sqlite3_bind_int(insert_set.st, 2, command.program_kind);
    sqlite3_bind_text(insert_set.st, 3, command.purpose.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_set.st, 4, command.created_by.value().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_set.st, 5, now);
    sqlite3_bind_int(insert_set.st, 6, command.priority_boost);
    if (command.expected_total.has_value()) sqlite3_bind_int(insert_set.st, 7, *command.expected_total); else sqlite3_bind_null(insert_set.st, 7);
    if (command.domain_ref_kind.has_value()) sqlite3_bind_text(insert_set.st, 8, command.domain_ref_kind->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(insert_set.st, 8);
    if (command.domain_ref_id.has_value()) sqlite3_bind_int64(insert_set.st, 9, *command.domain_ref_id); else sqlite3_bind_null(insert_set.st, 9);
    if (command.meta_note.has_value()) sqlite3_bind_text(insert_set.st, 10, command.meta_note->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(insert_set.st, 10);
    if (sqlite3_step(insert_set.st) != SQLITE_DONE) {
        rollback();
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto job_set_id = sqlite3_last_insert_rowid(db_);

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        rollback();
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    if (job_set_id_out) *job_set_id_out = job_set_id;

    if (job_command_service_) {
        std::string ignored;
        (void)job_command_service_->AppendLifecycleEvent(
            {
                .kind = jobs::JobLifecycleEventKind::JobSetCreated,
                .job_set_id = job_set_id,
                .message = command.meta_note,
                .requested_by = command.created_by,
            },
            &ignored);
    }

    return true;
}

bool SqliteExecutionDb::EnqueueJob(
    const EnqueueJobCommand& command,
    std::int64_t* job_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.job_set_id <= 0 || command.program_kind <= 0 || command.program_ref_kind.empty() || command.program_ref_id <= 0 || command.fingerprint.empty()) {
        if (error_out) *error_out = "invalid enqueue command";
        return false;
    }

    const auto queued_at_utc = CurrentUtcMs(db_);
    Statement insert_job;
    if (sqlite3_prepare_v2(db_,
        "INSERT INTO exec_job(job_set_id,parent_job_id,program_kind,program_version,program_ref_kind,program_ref_id,savestate_id,fingerprint,priority,state,attempts,max_attempts,claimed_by_token,lease_expires_at_utc,queued_at_utc,started_at_utc,ended_at_utc,error_code,error_text,input_ini) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,'QUEUED',?10,?11,NULL,NULL,?12,NULL,NULL,NULL,NULL,?);",
        -1,
        &insert_job.st,
        nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(insert_job.st, 1, command.job_set_id);
    if (command.parent_job_id.has_value()) sqlite3_bind_int64(insert_job.st, 2, *command.parent_job_id); else sqlite3_bind_null(insert_job.st, 2);
    sqlite3_bind_int(insert_job.st, 3, command.program_kind);
    sqlite3_bind_int(insert_job.st, 4, command.program_version);
    sqlite3_bind_text(insert_job.st, 5, command.program_ref_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_job.st, 6, command.program_ref_id);
    if (command.savestate_id.has_value()) sqlite3_bind_int64(insert_job.st, 7, *command.savestate_id); else sqlite3_bind_null(insert_job.st, 7);
    sqlite3_bind_text(insert_job.st, 8, command.fingerprint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insert_job.st, 9, command.priority);
    sqlite3_bind_int(insert_job.st, 10, 0);
    sqlite3_bind_int(insert_job.st, 11, command.max_attempts);
    sqlite3_bind_int64(insert_job.st, 12, queued_at_utc);
    sqlite3_bind_text(insert_job.st, 13, command.input_ini.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(insert_job.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    if (job_id_out) *job_id_out = sqlite3_last_insert_rowid(db_);
    return true;
}

std::optional<ClaimedExecutionJob> SqliteExecutionDb::ClaimNextReadyExecutionJob(
    std::string_view claimed_by_token,
    std::int64_t lease_duration_ms,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return std::nullopt;
    }
    if (claimed_by_token.empty()) {
        if (error_out) *error_out = "claimed_by_token is required";
        return std::nullopt;
    }
    if (lease_duration_ms <= 0) {
        if (error_out) *error_out = "lease_duration_ms must be > 0";
        return std::nullopt;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return std::nullopt;
    }
    const auto rollback = [&]() { (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr); };

    const auto now_utc = CurrentUtcMs(db_);
    const auto lease_expires_at_utc = now_utc + lease_duration_ms;

    ClaimedExecutionJob claimed{};
    {
        std::int64_t candidate_job_id = 0;
        {
            Statement candidate_st;
            if (sqlite3_prepare_v2(db_,
                "SELECT job_id, claimed_by_token, lease_expires_at_utc "
                "FROM exec_job "
                "WHERE state='QUEUED' "
                "  AND (claimed_by_token IS NULL OR claimed_by_token='' OR COALESCE(lease_expires_at_utc, 0) <= ?1) "
                "ORDER BY priority DESC, queued_at_utc ASC, job_id ASC "
                "LIMIT 1;",
                -1,
                &candidate_st.st,
                nullptr)
                != SQLITE_OK) {
                rollback();
                if (error_out) *error_out = sqlite3_errmsg(db_);
                return std::nullopt;
            }
            sqlite3_bind_int64(candidate_st.st, 1, now_utc);
            const auto rc = sqlite3_step(candidate_st.st);
            if (rc == SQLITE_ROW) {
                candidate_job_id = sqlite3_column_int64(candidate_st.st, 0);
                if (sqlite3_column_type(candidate_st.st, 1) != SQLITE_NULL) {
                    const auto* token = sqlite3_column_text(candidate_st.st, 1);
                    if (token != nullptr && token[0] != '\0') {
                        claimed.previous_claimed_by_token = reinterpret_cast<const char*>(token);
                    }
                }
                if (sqlite3_column_type(candidate_st.st, 2) != SQLITE_NULL) {
                    claimed.previous_lease_expires_at_utc = sqlite3_column_int64(candidate_st.st, 2);
                }
            } else if (rc != SQLITE_DONE) {
                rollback();
                if (error_out) *error_out = sqlite3_errmsg(db_);
                return std::nullopt;
            }
        }

        if (candidate_job_id <= 0) {
            char* commit_error = nullptr;
            const auto commit_rc = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &commit_error);
            if (commit_rc != SQLITE_OK) {
                const std::string message = commit_error ? commit_error : sqlite3_errmsg(db_);
                sqlite3_free(commit_error);
                rollback();
                if (error_out) *error_out = message;
                return std::nullopt;
            }
            sqlite3_free(commit_error);
            return std::nullopt;
        }

        Statement claim_st;
        if (sqlite3_prepare_v2(db_,
            "UPDATE exec_job "
                "SET claimed_by_token=?1, lease_expires_at_utc=?2 "
                "WHERE job_id=?3 "
                "AND state='QUEUED' "
                "AND (claimed_by_token IS NULL OR claimed_by_token='' OR COALESCE(lease_expires_at_utc, 0) <= ?4) "
                "RETURNING job_id, job_set_id, savestate_id, program_kind, program_ref_kind, program_ref_id;",
            -1,
            &claim_st.st,
            nullptr)
            != SQLITE_OK) {
            rollback();
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return std::nullopt;
        }

        const auto token = std::string(claimed_by_token);
        sqlite3_bind_text(claim_st.st, 1, token.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(claim_st.st, 2, lease_expires_at_utc);
        sqlite3_bind_int64(claim_st.st, 3, candidate_job_id);
        sqlite3_bind_int64(claim_st.st, 4, now_utc);

        const auto rc = sqlite3_step(claim_st.st);
        if (rc == SQLITE_ROW) {
            claimed.job_id = sqlite3_column_int64(claim_st.st, 0);
            claimed.job_set_id = sqlite3_column_int64(claim_st.st, 1);
            if (sqlite3_column_type(claim_st.st, 2) != SQLITE_NULL) {
                claimed.savestate_affinity_key = "savestate:" + std::to_string(sqlite3_column_int64(claim_st.st, 2));
            }
            const auto program_kind = sqlite3_column_int(claim_st.st, 3);
            const auto* program_ref_kind_text = sqlite3_column_text(claim_st.st, 4);
            const auto program_ref_kind = program_ref_kind_text ? reinterpret_cast<const char*>(program_ref_kind_text) : "";
            const auto program_ref_id = sqlite3_column_int64(claim_st.st, 5);
            claimed.program_runtime_affinity_key =
                std::to_string(program_kind) + ":" + program_ref_kind + ":" + std::to_string(program_ref_id);

            std::string step_error;
            if (!ResolveWorkflowStepForJobSetAncestry(db_, claimed.job_set_id, &claimed, &step_error)) {
                rollback();
                if (error_out) *error_out = step_error;
                return std::nullopt;
            }
        } else if (rc != SQLITE_DONE) {
            rollback();
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return std::nullopt;
        }
    }

    char* commit_error = nullptr;
    const auto commit_rc = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &commit_error);
    if (commit_rc != SQLITE_OK) {
        const std::string message = commit_error ? commit_error : sqlite3_errmsg(db_);
        sqlite3_free(commit_error);
        rollback();
        if (error_out) *error_out = message;
        return std::nullopt;
    }
    sqlite3_free(commit_error);

    if (claimed.job_id <= 0) {
        return std::nullopt;
    }
    return claimed;
}

std::vector<ClaimedExecutionJob> SqliteExecutionDb::ClaimBatchReadyExecutionJobs(
    std::string_view claimed_by_token,
    int requested_jobs,
    std::int64_t lease_duration_ms,
    std::string* error_out) {
    std::vector<ClaimedExecutionJob> claimed;
    if (requested_jobs <= 0) {
        return claimed;
    }

    claimed.reserve(static_cast<std::size_t>(requested_jobs));
    for (int i = 0; i < requested_jobs; ++i) {
        std::string claim_error;
        auto claimed_job = ClaimNextReadyExecutionJob(claimed_by_token, lease_duration_ms, &claim_error);
        if (!claim_error.empty()) {
            if (error_out) *error_out = claim_error;
            break;
        }
        if (!claimed_job.has_value()) {
            break;
        }
        claimed.push_back(std::move(*claimed_job));
    }
    return claimed;
}

bool SqliteExecutionDb::RenewExecutionJobLease(
    std::int64_t job_id,
    std::string_view claimed_by_token,
    std::int64_t lease_duration_ms,
    bool* renewed_out,
    std::string* error_out) {
    if (renewed_out) *renewed_out = false;
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (job_id <= 0) {
        if (error_out) *error_out = "job_id must be > 0";
        return false;
    }
    if (claimed_by_token.empty()) {
        if (error_out) *error_out = "claimed_by_token is required";
        return false;
    }
    if (lease_duration_ms <= 0) {
        if (error_out) *error_out = "lease_duration_ms must be > 0";
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(db_,
        "UPDATE exec_job "
            "SET lease_expires_at_utc=?1 "
            "WHERE job_id=?2 "
            "AND state IN ('QUEUED','RUNNING') "
            "AND claimed_by_token=?3 "
            "AND COALESCE(lease_expires_at_utc, 0) > ?4;",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    const auto now_utc = CurrentUtcMs(db_);
    const auto lease_expires_at_utc = now_utc + lease_duration_ms;
    const auto token = std::string(claimed_by_token);
    sqlite3_bind_int64(st.st, 1, lease_expires_at_utc);
    sqlite3_bind_int64(st.st, 2, job_id);
    sqlite3_bind_text(st.st, 3, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 4, now_utc);

    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (renewed_out) *renewed_out = sqlite3_changes(db_) > 0;
    return true;
}

bool SqliteExecutionDb::RequeueExpiredExecutionLeases(
    int* rows_requeued_out,
    std::string* error_out) {
    if (rows_requeued_out) *rows_requeued_out = 0;
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(db_,
        "UPDATE exec_job "
            "SET state='QUEUED', claimed_by_token=NULL, lease_expires_at_utc=NULL, started_at_utc=NULL "
            "WHERE state IN ('QUEUED','RUNNING') "
            "AND claimed_by_token IS NOT NULL "
            "AND claimed_by_token<>'' "
            "AND COALESCE(lease_expires_at_utc, 0) <= ?1;",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, CurrentUtcMs(db_));
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (rows_requeued_out) *rows_requeued_out = sqlite3_changes(db_);
    return true;
}

bool SqliteExecutionDb::MarkQueuedJobsSuperseded(
    std::int64_t job_set_id,
    std::int64_t except_job_id,
    std::string* error_out,
    int* rows_superseded_out) {
    if (rows_superseded_out != nullptr) {
        *rows_superseded_out = 0;
    }
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (job_set_id <= 0) {
        if (error_out) *error_out = "job_set_id must be > 0";
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(db_,
        "UPDATE exec_job "
            "SET state='SUPERSEDED', ended_at_utc=CAST(unixepoch('now') * 1000 AS INTEGER) "
            "WHERE job_set_id=?1 AND state='QUEUED' AND job_id<>?2;",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, job_set_id);
    sqlite3_bind_int64(st.st, 2, except_job_id);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (rows_superseded_out != nullptr) {
        *rows_superseded_out = sqlite3_changes(db_);
    }
    return true;
}

retention::OutboxRetentionPreview SqliteExecutionDb::PreviewOutboxRetention(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy) const {
    std::int64_t max_outbox_id = 0;
    Statement st;
    if (db_ != nullptr
        && sqlite3_prepare_v2(db_, "SELECT COALESCE(MAX(outbox_id), 0) FROM exec_outbox_message;", -1, &st.st, nullptr) == SQLITE_OK
        && sqlite3_step(st.st) == SQLITE_ROW) {
        max_outbox_id = sqlite3_column_int64(st.st, 0);
    }
    return retention::BuildOutboxRetentionPreview(max_outbox_id, subscriptions, now_utc, policy);
}

bool SqliteExecutionDb::PurgeOutboxThroughRetentionFloor(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy,
    int max_rows,
    int* rows_deleted_out,
    std::string* error_out) {
    if (rows_deleted_out) *rows_deleted_out = 0;
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (max_rows <= 0) {
        if (error_out) *error_out = "max_rows must be > 0";
        return false;
    }

    const auto preview = PreviewOutboxRetention(subscriptions, now_utc, policy);
    if (preview.IsPurgeBlocked()) {
        if (error_out) *error_out = "purge blocked by required paused/error subscriptions";
        return false;
    }
    if (!preview.safe_purge_floor_outbox_id.has_value()) {
        if (error_out) *error_out = "safe purge floor unavailable";
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(db_,
        "DELETE FROM exec_outbox_message "
            "WHERE outbox_id IN ("
            "  SELECT outbox_id FROM exec_outbox_message "
            "  WHERE published_at_utc IS NOT NULL AND outbox_id < ?1 "
            "  ORDER BY outbox_id ASC LIMIT ?2"
            ");",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, preview.safe_purge_floor_outbox_id.value());
    sqlite3_bind_int(st.st, 2, max_rows);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (rows_deleted_out) *rows_deleted_out = sqlite3_changes(db_);
    return true;
}

bool SqliteExecutionDb::PurgeWorkflowHandlerDedupeOlderThan(
    std::int64_t last_seen_at_utc_exclusive,
    int max_rows,
    int* rows_deleted_out,
    std::string* error_out) {
    if (rows_deleted_out) *rows_deleted_out = 0;
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (last_seen_at_utc_exclusive <= 0) {
        if (error_out) *error_out = "last_seen_at_utc_exclusive must be > 0";
        return false;
    }
    if (max_rows <= 0) {
        if (error_out) *error_out = "max_rows must be > 0";
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(db_,
            "DELETE FROM exec_handler_dedupe WHERE dedupe_id IN ("
            "SELECT dedupe_id FROM exec_handler_dedupe "
            "WHERE last_seen_at_utc < ?1 "
            "ORDER BY last_seen_at_utc ASC, dedupe_id ASC "
            "LIMIT ?2);",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, last_seen_at_utc_exclusive);
    sqlite3_bind_int(st.st, 2, max_rows);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (rows_deleted_out) *rows_deleted_out = sqlite3_changes(db_);
    return true;
}

std::optional<events::ExecutionWorkflowJobPayloadView> SqliteExecutionDb::ResolveExecutionWorkflowJobPayload(
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

std::optional<events::ExecutionWorkflowJobPayloadView> SqliteExecutionDb::ResolveExecutionWorkflowJobPayload(
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
