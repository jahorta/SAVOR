#include "UiReadProjectionService.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace savor::db::uiread::projectors {
namespace {

struct Statement {
    sqlite3_stmt* st = nullptr;
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }
};

struct OutboxEvent {
    std::int64_t outbox_id = 0;
    std::string event_id;
    std::string event_type;
    int event_version = 0;
    std::string context_name;
    std::string aggregate_kind;
    std::string aggregate_id;
    std::int64_t occurred_at_utc = 0;
    std::string payload_ref_kind;
    std::int64_t payload_ref_id = 0;
};

struct DirtyEntity {
    std::string kind;
    std::int64_t id = 0;
    std::int64_t outbox_id = 0;
};

std::int64_t UtcNowMillis() {
    const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
    return now.time_since_epoch().count();
}

std::string Text(sqlite3_stmt* st, int column) {
    const auto* value = sqlite3_column_text(st, column);
    return value == nullptr ? std::string{} : reinterpret_cast<const char*>(value);
}

std::int64_t ParseInt64(const std::string& value) {
    if (value.empty()) {
        return 0;
    }
    return static_cast<std::int64_t>(std::strtoll(value.c_str(), nullptr, 10));
}

bool Exec(sqlite3* db, const char* sql, std::string* error_out) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) == SQLITE_OK) {
        return true;
    }
    if (error_out != nullptr) {
        *error_out = err != nullptr ? err : sqlite3_errmsg(db);
    }
    sqlite3_free(err);
    return false;
}

bool Prepare(sqlite3* db, const char* sql, Statement* stmt, std::string* error_out) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt->st, nullptr) == SQLITE_OK) {
        return true;
    }
    if (error_out != nullptr) {
        *error_out = sqlite3_errmsg(db);
    }
    return false;
}

bool StepDone(sqlite3* db, sqlite3_stmt* st, std::string* error_out) {
    if (sqlite3_step(st) == SQLITE_DONE) {
        return true;
    }
    if (error_out != nullptr) {
        *error_out = sqlite3_errmsg(db);
    }
    return false;
}

void BindColumn(sqlite3_stmt* dst, int dst_col, sqlite3_stmt* src, int src_col) {
    switch (sqlite3_column_type(src, src_col)) {
    case SQLITE_INTEGER:
        sqlite3_bind_int64(dst, dst_col, sqlite3_column_int64(src, src_col));
        break;
    case SQLITE_FLOAT:
        sqlite3_bind_double(dst, dst_col, sqlite3_column_double(src, src_col));
        break;
    case SQLITE_TEXT: {
        const auto* text = sqlite3_column_text(src, src_col);
        sqlite3_bind_text(dst, dst_col, text == nullptr ? "" : reinterpret_cast<const char*>(text), -1, SQLITE_TRANSIENT);
        break;
    }
    case SQLITE_NULL:
        sqlite3_bind_null(dst, dst_col);
        break;
    default:
        sqlite3_bind_null(dst, dst_col);
        break;
    }
}

std::string Basename(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool ConfigureSourceConnection(sqlite3* db, std::string* error_out) {
    return Exec(db, "PRAGMA foreign_keys=ON;", error_out)
        && Exec(db, "PRAGMA busy_timeout=2000;", error_out);
}

bool ConfigureUiConnection(sqlite3* db, std::string* error_out) {
    return Exec(db, "PRAGMA journal_mode=WAL;", error_out)
        && Exec(db, "PRAGMA foreign_keys=ON;", error_out)
        && Exec(db, "PRAGMA busy_timeout=2000;", error_out);
}

bool OpenDb(
    const std::filesystem::path& path,
    bool readonly,
    sqlite3** db,
    std::string* error_out) {
    *db = nullptr;
    const int flags = (readonly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE))
        | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path.string().c_str(), db, flags, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = *db != nullptr ? sqlite3_errmsg(*db) : "sqlite3_open_v2 failed";
        }
        if (*db != nullptr) {
            sqlite3_close(*db);
            *db = nullptr;
        }
        return false;
    }
    return readonly ? ConfigureSourceConnection(*db, error_out) : ConfigureUiConnection(*db, error_out);
}

bool AttachExecutionSource(
    sqlite3* db,
    const std::filesystem::path& execution_db_path,
    std::string* error_out) {
    Statement attach;
    if (!Prepare(
            db,
            "ATTACH DATABASE ?1 AS execution_source;",
            &attach,
            error_out)) {
        return false;
    }
    const auto path = execution_db_path.string();
    sqlite3_bind_text(
        attach.st,
        1,
        path.c_str(),
        -1,
        SQLITE_TRANSIENT);
    return StepDone(db, attach.st, error_out);
}

std::int64_t ScalarInt64(sqlite3* db, const std::string& sql, std::string* error_out) {
    Statement st;
    if (!Prepare(db, sql.c_str(), &st, error_out)) {
        return 0;
    }
    if (sqlite3_step(st.st) == SQLITE_ROW && sqlite3_column_type(st.st, 0) != SQLITE_NULL) {
        return sqlite3_column_int64(st.st, 0);
    }
    return 0;
}

std::int64_t SourceHighWater(sqlite3* db, const std::string& table, std::string* error_out) {
    return ScalarInt64(db, "SELECT COALESCE(MAX(outbox_id),0) FROM " + table + ";", error_out);
}

std::int64_t LagCount(sqlite3* db, const std::string& table, std::int64_t cursor, std::string* error_out) {
    Statement st;
    const auto sql = "SELECT COUNT(1) FROM " + table + " WHERE outbox_id>?1;";
    if (!Prepare(db, sql.c_str(), &st, error_out)) {
        return 0;
    }
    sqlite3_bind_int64(st.st, 1, cursor);
    if (sqlite3_step(st.st) == SQLITE_ROW) {
        return sqlite3_column_int64(st.st, 0);
    }
    return 0;
}

std::int64_t LagAgeMs(sqlite3* db, const std::string& table, std::int64_t cursor, std::string* error_out) {
    Statement st;
    const auto sql = "SELECT MIN(occurred_at_utc) FROM " + table + " WHERE outbox_id>?1;";
    if (!Prepare(db, sql.c_str(), &st, error_out)) {
        return 0;
    }
    sqlite3_bind_int64(st.st, 1, cursor);
    if (sqlite3_step(st.st) == SQLITE_ROW && sqlite3_column_type(st.st, 0) != SQLITE_NULL) {
        return std::max<std::int64_t>(0, UtcNowMillis() - sqlite3_column_int64(st.st, 0));
    }
    return 0;
}

std::vector<OutboxEvent> ReadOutboxBatch(
    sqlite3* db,
    const std::string& table,
    std::int64_t cursor,
    int limit,
    std::string* error_out) {
    std::vector<OutboxEvent> rows;
    Statement st;
    const auto sql =
        "SELECT outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,"
        "occurred_at_utc,payload_ref_kind,payload_ref_id FROM "
        + table + " WHERE outbox_id>?1 ORDER BY outbox_id ASC LIMIT ?2;";
    if (!Prepare(db, sql.c_str(), &st, error_out)) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, cursor);
    sqlite3_bind_int(st.st, 2, limit);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        rows.push_back({
            .outbox_id = sqlite3_column_int64(st.st, 0),
            .event_id = Text(st.st, 1),
            .event_type = Text(st.st, 2),
            .event_version = sqlite3_column_int(st.st, 3),
            .context_name = Text(st.st, 4),
            .aggregate_kind = Text(st.st, 5),
            .aggregate_id = Text(st.st, 6),
            .occurred_at_utc = sqlite3_column_int64(st.st, 7),
            .payload_ref_kind = Text(st.st, 8),
            .payload_ref_id = sqlite3_column_int64(st.st, 9),
        });
    }
    return rows;
}

bool IsExecutionJobEvent(const std::string& event_type) {
    return event_type == "Execution.JobSetCreated.v1"
        || event_type == "Execution.JobQueued.v1"
        || event_type == "Execution.JobClaimed.v1"
        || event_type == "Execution.JobStarted.v1"
        || event_type == "Execution.JobLeaseRenewed.v1"
        || event_type == "Execution.JobProgressed.v1"
        || event_type == "Execution.JobPendingWorkset.v1"
        || event_type == "Execution.JobCompleted.v1"
        || event_type == "Execution.JobExecutionFinished.v1"
        || event_type == "Execution.JobResultProcessingStarted.v1"
        || event_type == "Execution.JobResultParked.v1"
        || event_type == "Execution.JobResultProcessed.v1"
        || event_type == "Execution.JobCancellationRequested.v1"
        || event_type == "Execution.JobCancellationDelivered.v1"
        || event_type == "Execution.JobCancellationResolved.v1"
        || event_type == "Execution.JobEventArchived.v1"
        || event_type == "Execution.JobRestored.v1";
}

bool IsExecutionWorkflowEvent(const std::string& event_type) {
    return event_type == "Execution.WorkflowInstanceCreated.v1"
        || event_type == "Execution.WorkflowStepReady.v1"
        || event_type == "Execution.WorkflowStepMaterialized.v1"
        || event_type == "Execution.WorkflowStepBlocked.v1"
        || event_type == "Execution.WorkflowStepCompleted.v1"
        || event_type == "Execution.WorkflowStepFailed.v1"
        || event_type == "Execution.WorkflowInstanceCompleted.v1";
}

bool ProjectWorkflowInstance(sqlite3* source, sqlite3* ui, std::int64_t workflow_instance_id, std::string* error_out);
bool RefreshWorkflowBattleRollupsForWorkflow(sqlite3* ui, std::int64_t workflow_instance_id, std::string* error_out);
bool RefreshWorkflowBattleRollupsForJobSet(sqlite3* ui, std::int64_t job_set_id, std::string* error_out);
bool RefreshWorkflowBattleRollupsForExecJob(sqlite3* ui, std::int64_t exec_job_id, std::string* error_out);

bool StopRequested(const std::function<bool()>& stop_requested, std::string* error_out) {
    if (!stop_requested()) {
        return false;
    }
    if (error_out != nullptr) {
        *error_out = "projection stop requested";
    }
    return true;
}

bool ProjectJobRows(
    sqlite3* source,
    sqlite3* ui,
    std::int64_t job_id,
    const std::function<bool()>& stop_requested,
    std::string* error_out) {
    if (job_id <= 0) {
        return true;
    }
    if (StopRequested(stop_requested, error_out)) {
        return false;
    }
    Statement src;
    constexpr const char* kSelect =
        "SELECT job_id,job_set_id,program_kind,state,priority,queued_at_utc,started_at_utc,ended_at_utc,error_code,"
        "attempts,max_attempts,fingerprint,claimed_by_token,lease_expires_at_utc,error_text,"
        "result_processing_state,result_processing_attempts,result_processing_failures,"
        "result_processing_retry_after_utc,result_processing_error_code,result_processing_error_text "
        "FROM exec_job WHERE job_id=?1;";
    if (!Prepare(source, kSelect, &src, error_out)) {
        return false;
    }
    sqlite3_bind_int64(src.st, 1, job_id);
    const int src_rc = sqlite3_step(src.st);
    if (src_rc == SQLITE_DONE) {
        return true;
    }
    if (src_rc != SQLITE_ROW) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(source);
        }
        return false;
    }
    if (StopRequested(stop_requested, error_out)) {
        return false;
    }

    Statement summary;
    constexpr const char* kSummary =
        "INSERT INTO ui_job_summary(job_id,job_set_id,program_kind,state,priority,queued_at_utc,started_at_utc,ended_at_utc,error_code) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9) "
        "ON CONFLICT(job_id) DO UPDATE SET "
        "job_set_id=excluded.job_set_id,program_kind=excluded.program_kind,state=excluded.state,priority=excluded.priority,"
        "queued_at_utc=excluded.queued_at_utc,started_at_utc=excluded.started_at_utc,ended_at_utc=excluded.ended_at_utc,error_code=excluded.error_code;";
    if (!Prepare(ui, kSummary, &summary, error_out)) {
        return false;
    }
    for (int i = 0; i < 9; ++i) {
        BindColumn(summary.st, i + 1, src.st, i);
    }
    if (!StepDone(ui, summary.st, error_out)) {
        return false;
    }
    if (StopRequested(stop_requested, error_out)) {
        return false;
    }

    Statement detail;
    constexpr const char* kDetail =
        "INSERT INTO ui_job_detail(job_id,attempts,max_attempts,fingerprint,claimed_by_token,lease_expires_at_utc,error_text,"
        "result_processing_state,result_processing_attempts,result_processing_failures,result_processing_retry_after_utc,"
        "result_processing_error_code,"
        "result_processing_error_text) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13) "
        "ON CONFLICT(job_id) DO UPDATE SET "
        "attempts=excluded.attempts,max_attempts=excluded.max_attempts,fingerprint=excluded.fingerprint,"
        "claimed_by_token=excluded.claimed_by_token,lease_expires_at_utc=excluded.lease_expires_at_utc,error_text=excluded.error_text,"
        "result_processing_state=excluded.result_processing_state,result_processing_attempts=excluded.result_processing_attempts,"
        "result_processing_failures=excluded.result_processing_failures,"
        "result_processing_retry_after_utc=excluded.result_processing_retry_after_utc,"
        "result_processing_error_code=excluded.result_processing_error_code,"
        "result_processing_error_text=excluded.result_processing_error_text;";
    if (!Prepare(ui, kDetail, &detail, error_out)) {
        return false;
    }
    BindColumn(detail.st, 1, src.st, 0);
    for (int i = 9; i < 15; ++i) {
        BindColumn(detail.st, i - 7, src.st, i);
    }
    for (int i = 15; i < 21; ++i) {
        BindColumn(detail.st, i - 7, src.st, i);
    }
    if (!StepDone(ui, detail.st, error_out)) {
        return false;
    }
    if (StopRequested(stop_requested, error_out)) {
        return false;
    }

    Statement del;
    if (!Prepare(ui, "DELETE FROM ui_job_artifact WHERE job_id=?1;", &del, error_out)) {
        return false;
    }
    sqlite3_bind_int64(del.st, 1, job_id);
    if (!StepDone(ui, del.st, error_out)) {
        return false;
    }
    if (StopRequested(stop_requested, error_out)) {
        return false;
    }

    Statement events;
    constexpr const char* kEvents =
        "SELECT job_event_id,job_id,artifact_id,event_kind,event_ts_utc "
        "FROM exec_job_event WHERE job_id=?1 AND artifact_id IS NOT NULL ORDER BY job_event_id ASC;";
    if (!Prepare(source, kEvents, &events, error_out)) {
        return false;
    }
    sqlite3_bind_int64(events.st, 1, job_id);
    int events_rc = SQLITE_OK;
    while ((events_rc = sqlite3_step(events.st)) == SQLITE_ROW) {
        if (StopRequested(stop_requested, error_out)) {
            return false;
        }
        Statement ins;
        constexpr const char* kArtifact =
            "INSERT INTO ui_job_artifact(ui_job_artifact_id,job_id,artifact_id,role_kind,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5) "
            "ON CONFLICT(job_id,artifact_id,role_kind) DO UPDATE SET created_at_utc=excluded.created_at_utc;";
        if (!Prepare(ui, kArtifact, &ins, error_out)) {
            return false;
        }
        for (int i = 0; i < 5; ++i) {
            BindColumn(ins.st, i + 1, events.st, i);
        }
        if (!StepDone(ui, ins.st, error_out)) {
            return false;
        }
    }
    if (events_rc != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(source);
        }
        return false;
    }
    return true;
}

bool ProjectJob(
    sqlite3* source,
    sqlite3* ui,
    std::int64_t job_id,
    const std::function<bool()>& stop_requested,
    std::string* error_out) {
    if (!ProjectJobRows(source, ui, job_id, stop_requested, error_out)) {
        return false;
    }
    if (StopRequested(stop_requested, error_out)) {
        return false;
    }
    return RefreshWorkflowBattleRollupsForExecJob(ui, job_id, error_out);
}

bool ProjectJobSet(
    sqlite3* source,
    sqlite3* ui,
    std::int64_t job_set_id,
    const std::function<bool()>& stop_requested,
    std::string* error_out) {
    if (job_set_id <= 0) {
        return true;
    }
    if (StopRequested(stop_requested, error_out)) {
        return false;
    }
    Statement st;
    if (!Prepare(source, "SELECT job_id FROM exec_job WHERE job_set_id=?1;", &st, error_out)) {
        return false;
    }
    sqlite3_bind_int64(st.st, 1, job_set_id);
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(st.st)) == SQLITE_ROW) {
        if (StopRequested(stop_requested, error_out)) {
            return false;
        }
        if (!ProjectJobRows(source, ui, sqlite3_column_int64(st.st, 0), stop_requested, error_out)) {
            return false;
        }
    }
    if (rc != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(source);
        }
        return false;
    }
    if (StopRequested(stop_requested, error_out)) {
        return false;
    }
    return RefreshWorkflowBattleRollupsForJobSet(ui, job_set_id, error_out);
}

std::int64_t ResolveWorkflowInstanceForJobSet(sqlite3* source, std::int64_t job_set_id, std::string* error_out) {
    if (job_set_id <= 0) {
        return 0;
    }
    Statement st;
    if (!Prepare(source, "SELECT workflow_instance_id FROM exec_workflow_step WHERE job_set_id=?1 LIMIT 1;", &st, error_out)) {
        return 0;
    }
    sqlite3_bind_int64(st.st, 1, job_set_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return 0;
    }
    return sqlite3_column_int64(st.st, 0);
}

std::int64_t ResolveWorkflowInstanceForJob(sqlite3* source, std::int64_t job_id, std::string* error_out) {
    if (job_id <= 0) {
        return 0;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT s.workflow_instance_id "
        "FROM exec_job j JOIN exec_workflow_step s ON s.job_set_id=j.job_set_id "
        "WHERE j.job_id=?1 LIMIT 1;";
    if (!Prepare(source, kSql, &st, error_out)) {
        return 0;
    }
    sqlite3_bind_int64(st.st, 1, job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return 0;
    }
    return sqlite3_column_int64(st.st, 0);
}

bool ProjectWorkflowForJobSet(sqlite3* source, sqlite3* ui, std::int64_t job_set_id, std::string* error_out) {
    const auto workflow_instance_id = ResolveWorkflowInstanceForJobSet(source, job_set_id, error_out);
    return workflow_instance_id <= 0 || ProjectWorkflowInstance(source, ui, workflow_instance_id, error_out);
}

bool ProjectWorkflowForJob(sqlite3* source, sqlite3* ui, std::int64_t job_id, std::string* error_out) {
    const auto workflow_instance_id = ResolveWorkflowInstanceForJob(source, job_id, error_out);
    return workflow_instance_id <= 0 || ProjectWorkflowInstance(source, ui, workflow_instance_id, error_out);
}

bool ProjectWorkflowInstance(sqlite3* source, sqlite3* ui, std::int64_t workflow_instance_id, std::string* error_out) {
    if (workflow_instance_id <= 0) {
        return true;
    }

    Statement inst;
    constexpr const char* kInst =
        "SELECT i.workflow_instance_id,i.workflow_kind,i.state,"
        "CASE "
        "WHEN i.state IN ('COMPLETED','FAILED','CANCELED') THEN i.state "
        "WHEN EXISTS(SELECT 1 FROM exec_workflow_step s WHERE s.workflow_instance_id=i.workflow_instance_id AND s.state IN ('MATERIALIZED','RUNNING')) THEN 'RUNNING' "
        "WHEN EXISTS(SELECT 1 FROM exec_workflow_step s JOIN exec_job j ON j.job_set_id=s.job_set_id WHERE s.workflow_instance_id=i.workflow_instance_id AND j.state IN ('PENDING_MATERIALIZATION','QUEUED','CLAIMED','RUNNING','EXECUTION_FINISHED')) THEN 'RUNNING' "
        "WHEN EXISTS(SELECT 1 FROM exec_workflow_step s WHERE s.workflow_instance_id=i.workflow_instance_id AND s.state='READY') THEN 'QUEUED' "
        "ELSE 'WAITING' END,"
        "i.root_scope_kind,i.root_scope_id,i.created_by,"
        "(SELECT COUNT(1) FROM exec_workflow_step s WHERE s.workflow_instance_id=i.workflow_instance_id AND s.blocked_reason IS NOT NULL),"
        "(SELECT COUNT(1) FROM exec_workflow_step s WHERE s.workflow_instance_id=i.workflow_instance_id AND s.state='FAILED'),"
        "i.created_at_utc,i.started_at_utc,i.completed_at_utc,i.failure_code,i.failure_text "
        "FROM exec_workflow_instance i WHERE i.workflow_instance_id=?1;";
    if (!Prepare(source, kInst, &inst, error_out)) {
        return false;
    }
    sqlite3_bind_int64(inst.st, 1, workflow_instance_id);
    if (sqlite3_step(inst.st) != SQLITE_ROW) {
        return true;
    }

    Statement upsert_inst;
    constexpr const char* kUpsertInst =
        "INSERT INTO ui_workflow_instance("
        "workflow_instance_id,workflow_kind,state,display_state,root_scope_kind,root_scope_id,created_by,blocked_step_count,failed_step_count,created_at_utc,started_at_utc,completed_at_utc,failure_code,failure_text) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14) "
        "ON CONFLICT(workflow_instance_id) DO UPDATE SET "
        "workflow_kind=excluded.workflow_kind,state=excluded.state,display_state=excluded.display_state,root_scope_kind=excluded.root_scope_kind,root_scope_id=excluded.root_scope_id,"
        "created_by=excluded.created_by,blocked_step_count=excluded.blocked_step_count,failed_step_count=excluded.failed_step_count,"
        "started_at_utc=excluded.started_at_utc,completed_at_utc=excluded.completed_at_utc,failure_code=excluded.failure_code,failure_text=excluded.failure_text;";
    if (!Prepare(ui, kUpsertInst, &upsert_inst, error_out)) {
        return false;
    }
    for (int i = 0; i < 14; ++i) {
        BindColumn(upsert_inst.st, i + 1, inst.st, i);
    }
    if (!StepDone(ui, upsert_inst.st, error_out)) {
        return false;
    }

    Statement activations;
    constexpr const char* kActivations =
        "SELECT workflow_unit_activation_id,workflow_instance_id,parent_workflow_unit_activation_id,activation_key,graph_node_key,unit_kind,display_name,state,"
        "activation_params_json,authored_ref_kind,authored_ref_id,failure_code,failure_text,created_at_utc,ready_at_utc,started_at_utc,completed_at_utc,failed_at_utc "
        "FROM exec_workflow_unit_activation WHERE workflow_instance_id=?1;";
    if (Prepare(source, kActivations, &activations, nullptr)) {
        sqlite3_bind_int64(activations.st, 1, workflow_instance_id);
        while (sqlite3_step(activations.st) == SQLITE_ROW) {
            Statement upsert;
            constexpr const char* kUpsert =
                "INSERT INTO ui_workflow_unit_activation("
                "workflow_unit_activation_id,workflow_instance_id,parent_workflow_unit_activation_id,activation_key,graph_node_key,unit_kind,display_name,state,"
                "activation_params_json,authored_ref_kind,authored_ref_id,failure_code,failure_text,created_at_utc,ready_at_utc,started_at_utc,completed_at_utc,failed_at_utc) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18) "
                "ON CONFLICT(workflow_unit_activation_id) DO UPDATE SET "
                "parent_workflow_unit_activation_id=excluded.parent_workflow_unit_activation_id,state=excluded.state,"
                "activation_params_json=excluded.activation_params_json,failure_code=excluded.failure_code,failure_text=excluded.failure_text,"
                "ready_at_utc=excluded.ready_at_utc,started_at_utc=excluded.started_at_utc,completed_at_utc=excluded.completed_at_utc,failed_at_utc=excluded.failed_at_utc;";
            if (!Prepare(ui, kUpsert, &upsert, error_out)) return false;
            for (int i = 0; i < 18; ++i) BindColumn(upsert.st, i + 1, activations.st, i);
            if (!StepDone(ui, upsert.st, error_out)) return false;
        }
    }

    Statement activation_edges;
    constexpr const char* kActivationEdges =
        "SELECT workflow_unit_activation_edge_id,workflow_instance_id,from_workflow_unit_activation_id,to_workflow_unit_activation_id,"
        "output_key,input_key,condition_kind,condition_value,created_at_utc "
        "FROM exec_workflow_unit_activation_edge WHERE workflow_instance_id=?1;";
    if (Prepare(source, kActivationEdges, &activation_edges, nullptr)) {
        sqlite3_bind_int64(activation_edges.st, 1, workflow_instance_id);
        while (sqlite3_step(activation_edges.st) == SQLITE_ROW) {
            Statement upsert;
            constexpr const char* kUpsert =
                "INSERT INTO ui_workflow_unit_activation_edge("
                "workflow_unit_activation_edge_id,workflow_instance_id,from_workflow_unit_activation_id,to_workflow_unit_activation_id,output_key,input_key,condition_kind,condition_value,created_at_utc) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9) "
                "ON CONFLICT(workflow_unit_activation_edge_id) DO UPDATE SET "
                "output_key=excluded.output_key,input_key=excluded.input_key,condition_kind=excluded.condition_kind,condition_value=excluded.condition_value;";
            if (!Prepare(ui, kUpsert, &upsert, error_out)) return false;
            for (int i = 0; i < 9; ++i) BindColumn(upsert.st, i + 1, activation_edges.st, i);
            if (!StepDone(ui, upsert.st, error_out)) return false;
        }
    }

    Statement steps;
    constexpr const char* kSteps =
        "SELECT s.workflow_step_id,s.workflow_instance_id,s.workflow_unit_activation_id,s.step_key,s.step_kind,s.state,s.blocked_reason,s.job_set_id,"
        "(SELECT COUNT(1) FROM exec_job j WHERE j.job_set_id=s.job_set_id),"
        "(SELECT COUNT(1) FROM exec_job j WHERE j.job_set_id=s.job_set_id AND j.state IN ('COMPLETED','SUCCEEDED','SUCCEEDED_WINNER','SUPERSEDED','SUCCEEDED_DUPLICATE')),"
        "(SELECT COUNT(1) FROM exec_job j WHERE j.job_set_id=s.job_set_id AND j.state='FAILED'),"
        "s.priority,s.attempts,s.max_attempts,s.ready_at_utc,s.started_at_utc,s.completed_at_utc,s.failed_at_utc,s.created_at_utc "
        "FROM exec_workflow_step s WHERE s.workflow_instance_id=?1;";
    if (!Prepare(source, kSteps, &steps, error_out)) {
        return false;
    }
    sqlite3_bind_int64(steps.st, 1, workflow_instance_id);
    while (sqlite3_step(steps.st) == SQLITE_ROW) {
        Statement upsert;
        constexpr const char* kUpsert =
            "INSERT INTO ui_workflow_step("
            "workflow_step_id,workflow_instance_id,workflow_unit_activation_id,step_key,step_kind,state,blocked_reason,job_set_id,"
            "job_count,job_completed_count,job_failed_count,priority,attempts,max_attempts,ready_at_utc,started_at_utc,completed_at_utc,failed_at_utc,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19) "
            "ON CONFLICT(workflow_step_id) DO UPDATE SET "
            "workflow_unit_activation_id=excluded.workflow_unit_activation_id,state=excluded.state,blocked_reason=excluded.blocked_reason,job_set_id=excluded.job_set_id,"
            "job_count=excluded.job_count,job_completed_count=excluded.job_completed_count,job_failed_count=excluded.job_failed_count,"
            "attempts=excluded.attempts,max_attempts=excluded.max_attempts,ready_at_utc=excluded.ready_at_utc,started_at_utc=excluded.started_at_utc,"
            "completed_at_utc=excluded.completed_at_utc,failed_at_utc=excluded.failed_at_utc;";
        if (!Prepare(ui, kUpsert, &upsert, error_out)) return false;
        for (int i = 0; i < 19; ++i) BindColumn(upsert.st, i + 1, steps.st, i);
        if (!StepDone(ui, upsert.st, error_out)) return false;
    }

    Statement edges;
    constexpr const char* kEdges =
        "SELECT workflow_edge_id,workflow_instance_id,from_step_id,to_step_id,condition_kind,condition_value,created_at_utc "
        "FROM exec_workflow_edge WHERE workflow_instance_id=?1;";
    if (!Prepare(source, kEdges, &edges, error_out)) {
        return false;
    }
    sqlite3_bind_int64(edges.st, 1, workflow_instance_id);
    while (sqlite3_step(edges.st) == SQLITE_ROW) {
        Statement upsert;
        constexpr const char* kUpsert =
            "INSERT INTO ui_workflow_edge(workflow_edge_id,workflow_instance_id,from_step_id,to_step_id,condition_kind,condition_value,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7) "
            "ON CONFLICT(workflow_edge_id) DO UPDATE SET condition_kind=excluded.condition_kind,condition_value=excluded.condition_value;";
        if (!Prepare(ui, kUpsert, &upsert, error_out)) return false;
        for (int i = 0; i < 7; ++i) BindColumn(upsert.st, i + 1, edges.st, i);
        if (!StepDone(ui, upsert.st, error_out)) return false;
    }

    const auto now = UtcNowMillis();
    Statement clear_alerts;
    if (!Prepare(ui,
            "UPDATE ui_workflow_alert SET is_active=0,cleared_at_utc=?2 WHERE workflow_instance_id=?1 AND is_active=1;",
            &clear_alerts,
            error_out)) {
        return false;
    }
    sqlite3_bind_int64(clear_alerts.st, 1, workflow_instance_id);
    sqlite3_bind_int64(clear_alerts.st, 2, now);
    if (!StepDone(ui, clear_alerts.st, error_out)) return false;

    Statement alert_src;
    constexpr const char* kAlertSrc =
        "SELECT (workflow_step_id * 10) + CASE WHEN blocked_reason IS NOT NULL THEN 1 ELSE 2 END,"
        "workflow_instance_id,workflow_step_id,"
        "CASE WHEN blocked_reason IS NOT NULL THEN 'BLOCKED_STEP' ELSE 'FAILED_STEP' END,"
        "CASE WHEN blocked_reason='DESCRIPTOR_UNAVAILABLE' THEN 'DESCRIPTOR_UNAVAILABLE' "
        "WHEN blocked_reason IS NOT NULL THEN 'STEP_BLOCKED' ELSE 'STEP_FAILED' END,"
        "COALESCE(blocked_reason,'step failed') "
        "FROM exec_workflow_step WHERE workflow_instance_id=?1 AND (blocked_reason IS NOT NULL OR state='FAILED');";
    if (!Prepare(source, kAlertSrc, &alert_src, error_out)) return false;
    sqlite3_bind_int64(alert_src.st, 1, workflow_instance_id);
    while (sqlite3_step(alert_src.st) == SQLITE_ROW) {
        Statement upsert;
        constexpr const char* kUpsert =
            "INSERT INTO ui_workflow_alert(workflow_alert_id,workflow_instance_id,workflow_step_id,alert_kind,alert_code,message,is_active,first_seen_at_utc,last_seen_at_utc,cleared_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,1,?7,?7,NULL) "
            "ON CONFLICT(workflow_alert_id) DO UPDATE SET alert_kind=excluded.alert_kind,alert_code=excluded.alert_code,message=excluded.message,is_active=1,last_seen_at_utc=excluded.last_seen_at_utc,cleared_at_utc=NULL;";
        if (!Prepare(ui, kUpsert, &upsert, error_out)) return false;
        for (int i = 0; i < 6; ++i) BindColumn(upsert.st, i + 1, alert_src.st, i);
        sqlite3_bind_int64(upsert.st, 7, now);
        if (!StepDone(ui, upsert.st, error_out)) return false;
    }

    return RefreshWorkflowBattleRollupsForWorkflow(ui, workflow_instance_id, error_out);
}

bool ProjectArtifact(sqlite3* source, sqlite3* ui, std::int64_t artifact_id, std::string* error_out) {
    if (artifact_id <= 0) return true;
    Statement src;
    if (!Prepare(source,
            "SELECT artifact_id,sha256,size_bytes,artifact_kind,filename,created_at_utc FROM state_artifact WHERE artifact_id=?1;",
            &src,
            error_out)) return false;
    sqlite3_bind_int64(src.st, 1, artifact_id);
    if (sqlite3_step(src.st) != SQLITE_ROW) return true;

    Statement upsert;
    constexpr const char* kSql =
        "INSERT INTO ui_artifact_browser(artifact_id,sha256,size_bytes,artifact_kind,filename,created_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6) "
        "ON CONFLICT(artifact_id) DO UPDATE SET sha256=excluded.sha256,size_bytes=excluded.size_bytes,artifact_kind=excluded.artifact_kind,filename=excluded.filename,created_at_utc=excluded.created_at_utc;";
    if (!Prepare(ui, kSql, &upsert, error_out)) return false;
    BindColumn(upsert.st, 1, src.st, 0);
    BindColumn(upsert.st, 2, src.st, 1);
    BindColumn(upsert.st, 3, src.st, 2);
    BindColumn(upsert.st, 4, src.st, 3);
    const auto filename = Basename(Text(src.st, 4));
    sqlite3_bind_text(upsert.st, 5, filename.c_str(), -1, SQLITE_TRANSIENT);
    BindColumn(upsert.st, 6, src.st, 5);
    return StepDone(ui, upsert.st, error_out);
}

std::int64_t ResolveProbeRunId(sqlite3* source, const OutboxEvent& event, std::string* error_out) {
    if (event.aggregate_kind == "probe_run") return ParseInt64(event.aggregate_id);
    if (event.payload_ref_kind == "probe_run") return event.payload_ref_id;
    if (event.payload_ref_kind == "probe_result") {
        Statement st;
        if (!Prepare(source, "SELECT probe_run_id FROM sp_probe_result WHERE probe_result_id=?1;", &st, error_out)) return 0;
        sqlite3_bind_int64(st.st, 1, event.payload_ref_id);
        return sqlite3_step(st.st) == SQLITE_ROW ? sqlite3_column_int64(st.st, 0) : 0;
    }
    if (event.payload_ref_kind == "encounter_projection") {
        Statement st;
        if (!Prepare(source, "SELECT probe_run_id FROM sp_encounter_projection WHERE encounter_projection_id=?1;", &st, error_out)) return 0;
        sqlite3_bind_int64(st.st, 1, event.payload_ref_id);
        return sqlite3_step(st.st) == SQLITE_ROW ? sqlite3_column_int64(st.st, 0) : 0;
    }
    return 0;
}

bool ProjectSeedProbeRun(sqlite3* source, sqlite3* ui, std::int64_t probe_run_id, std::string* error_out) {
    if (probe_run_id <= 0) return true;
    Statement summary_src;
    constexpr const char* kSummarySrc =
        "WITH neutral(seed_value) AS ("
        " SELECT o.seed_value FROM sp_probe_result o "
        " JOIN execution_source.exec_job j ON j.job_id=o.source_job_id "
        " JOIN sp_input_frame f ON f.input_frame_id=o.input_frame_id "
        " JOIN sp_axis_xy m ON m.axis_xy_id=f.main_axis_xy_id "
        " JOIN sp_axis_xy c ON c.axis_xy_id=f.cstick_axis_xy_id "
        " JOIN sp_axis_xy t ON t.axis_xy_id=f.trigger_axis_xy_id "
        " WHERE o.probe_run_id=?1 AND o.confirmation_of_probe_result_id IS NULL "
        " AND o.evidence_state<>'REJECTED' "
        " AND instr(char(10)||replace(COALESCE(j.input_ini,''),char(13),'')||char(10),"
        " char(10)||'stage=SURVEY'||char(10))>0 "
        " AND m.x=128 AND m.y=128 AND c.x=128 AND c.y=128 AND t.x=0 AND t.y=0 "
        " ORDER BY CASE o.evidence_state WHEN 'CONFIRMED' THEN 0 WHEN 'PROVISIONAL' THEN 1 ELSE 2 END,"
        " o.probe_result_id LIMIT 1"
        ") "
        "SELECT r.probe_run_id,r.probe_set_id,r.entry_savestate_id,r.seed_probe_spec_id,r.codec_version,r.status,"
        "(SELECT seed_value FROM neutral),"
        "(SELECT COUNT(1) FROM sp_probe_result o "
        " JOIN execution_source.exec_job j ON j.job_id=o.source_job_id "
        " JOIN sp_input_frame f ON f.input_frame_id=o.input_frame_id "
        " JOIN sp_axis_xy m ON m.axis_xy_id=f.main_axis_xy_id "
        " JOIN sp_axis_xy c ON c.axis_xy_id=f.cstick_axis_xy_id "
        " JOIN sp_axis_xy t ON t.axis_xy_id=f.trigger_axis_xy_id "
        " WHERE o.probe_run_id=r.probe_run_id AND o.confirmation_of_probe_result_id IS NULL "
        " AND o.evidence_state IN ('PROVISIONAL','CONFIRMED') "
        " AND instr(char(10)||replace(COALESCE(j.input_ini,''),char(13),'')||char(10),"
        " char(10)||'stage=SURVEY'||char(10))>0 "
        " AND ((m.x<>128 OR m.y<>128) + (c.x<>128 OR c.y<>128) + (t.x<>0 OR t.y<>0))=1),"
        "(SELECT COUNT(1) FROM sp_probe_result o "
        " JOIN execution_source.exec_job j ON j.job_id=o.source_job_id "
        " WHERE o.probe_run_id=r.probe_run_id "
        " AND o.confirmation_of_probe_result_id IS NULL AND o.evidence_state='CONFIRMED' "
        " AND instr(char(10)||replace(COALESCE(j.input_ini,''),char(13),'')||char(10),"
        " char(10)||'stage=SEARCH'||char(10))>0),"
        "r.requested_at_utc,r.completed_at_utc "
        "FROM sp_probe_run r WHERE r.probe_run_id=?1;";
    if (!Prepare(source, kSummarySrc, &summary_src, error_out)) return false;
    sqlite3_bind_int64(summary_src.st, 1, probe_run_id);
    if (sqlite3_step(summary_src.st) != SQLITE_ROW) return true;

    Statement summary;
    constexpr const char* kSummary =
        "INSERT INTO ui_seed_probe_summary("
        "probe_run_id,probe_set_id,entry_savestate_id,seed_probe_spec_id,codec_version,status,neutral_seed_value,grid_count,unique_count,requested_at_utc,completed_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11) "
        "ON CONFLICT(probe_run_id) DO UPDATE SET "
        "probe_set_id=excluded.probe_set_id,entry_savestate_id=excluded.entry_savestate_id,seed_probe_spec_id=excluded.seed_probe_spec_id,"
        "codec_version=excluded.codec_version,status=excluded.status,neutral_seed_value=excluded.neutral_seed_value,"
        "grid_count=excluded.grid_count,unique_count=excluded.unique_count,requested_at_utc=excluded.requested_at_utc,completed_at_utc=excluded.completed_at_utc;";
    if (!Prepare(ui, kSummary, &summary, error_out)) return false;
    for (int i = 0; i < 11; ++i) BindColumn(summary.st, i + 1, summary_src.st, i);
    if (!StepDone(ui, summary.st, error_out)) return false;

    Statement del_delta;
    if (!Prepare(ui, "DELETE FROM ui_seed_probe_delta_point WHERE probe_run_id=?1;", &del_delta, error_out)) return false;
    sqlite3_bind_int64(del_delta.st, 1, probe_run_id);
    if (!StepDone(ui, del_delta.st, error_out)) return false;

    Statement delta_src;
    constexpr const char* kDeltaSrc =
        "WITH neutral(seed_value) AS ("
        " SELECT o.seed_value FROM sp_probe_result o "
        " JOIN execution_source.exec_job j ON j.job_id=o.source_job_id "
        " JOIN sp_input_frame f ON f.input_frame_id=o.input_frame_id "
        " JOIN sp_axis_xy m ON m.axis_xy_id=f.main_axis_xy_id "
        " JOIN sp_axis_xy c ON c.axis_xy_id=f.cstick_axis_xy_id "
        " JOIN sp_axis_xy t ON t.axis_xy_id=f.trigger_axis_xy_id "
        " WHERE o.probe_run_id=?1 AND o.confirmation_of_probe_result_id IS NULL "
        " AND o.evidence_state<>'REJECTED' "
        " AND instr(char(10)||replace(COALESCE(j.input_ini,''),char(13),'')||char(10),"
        " char(10)||'stage=SURVEY'||char(10))>0 "
        " AND m.x=128 AND m.y=128 AND c.x=128 AND c.y=128 AND t.x=0 AND t.y=0 "
        " ORDER BY CASE o.evidence_state WHEN 'CONFIRMED' THEN 0 WHEN 'PROVISIONAL' THEN 1 ELSE 2 END,"
        " o.probe_result_id LIMIT 1"
        ") "
        "SELECT o.probe_result_id,o.probe_run_id,"
        "CASE WHEN (m.x<>128 OR m.y<>128) THEN 'MAIN' "
        " WHEN (c.x<>128 OR c.y<>128) THEN 'CSTICK' ELSE 'TRIGGER' END,"
        "CASE WHEN (m.x<>128 OR m.y<>128) THEN m.x "
        " WHEN (c.x<>128 OR c.y<>128) THEN c.x ELSE t.x END,"
        "CASE WHEN (m.x<>128 OR m.y<>128) THEN m.y "
        " WHEN (c.x<>128 OR c.y<>128) THEN c.y ELSE t.y END,"
        "o.seed_value,"
        "((((o.seed_value-neutral.seed_value)+2147483648) & 4294967295)-2147483648) "
        "FROM sp_probe_result o "
        "JOIN execution_source.exec_job j ON j.job_id=o.source_job_id "
        "JOIN sp_input_frame f ON f.input_frame_id=o.input_frame_id "
        "JOIN sp_axis_xy m ON m.axis_xy_id=f.main_axis_xy_id "
        "JOIN sp_axis_xy c ON c.axis_xy_id=f.cstick_axis_xy_id "
        "JOIN sp_axis_xy t ON t.axis_xy_id=f.trigger_axis_xy_id "
        "CROSS JOIN neutral "
        "WHERE o.probe_run_id=?1 AND o.confirmation_of_probe_result_id IS NULL "
        "AND o.evidence_state IN ('PROVISIONAL','CONFIRMED') "
        "AND instr(char(10)||replace(COALESCE(j.input_ini,''),char(13),'')||char(10),"
        " char(10)||'stage=SURVEY'||char(10))>0 "
        "AND ((m.x<>128 OR m.y<>128) + (c.x<>128 OR c.y<>128) + (t.x<>0 OR t.y<>0))=1;";
    if (!Prepare(source, kDeltaSrc, &delta_src, error_out)) return false;
    sqlite3_bind_int64(delta_src.st, 1, probe_run_id);
    while (sqlite3_step(delta_src.st) == SQLITE_ROW) {
        Statement ins;
        constexpr const char* kIns =
            "INSERT INTO ui_seed_probe_delta_point(delta_point_id,probe_run_id,source_family,axis_x,axis_y,seed_value,seed_delta) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7) "
            "ON CONFLICT(probe_run_id,source_family,axis_x,axis_y,seed_value) DO UPDATE SET seed_delta=excluded.seed_delta;";
        if (!Prepare(ui, kIns, &ins, error_out)) return false;
        for (int i = 0; i < 7; ++i) BindColumn(ins.st, i + 1, delta_src.st, i);
        if (!StepDone(ui, ins.st, error_out)) return false;
    }

    Statement del_unique;
    if (!Prepare(ui, "DELETE FROM ui_seed_probe_unique_value WHERE probe_run_id=?1;", &del_unique, error_out)) return false;
    sqlite3_bind_int64(del_unique.st, 1, probe_run_id);
    if (!StepDone(ui, del_unique.st, error_out)) return false;

    Statement unique_src;
    constexpr const char* kUniqueSrc =
        "WITH neutral(seed_value) AS ("
        " SELECT o.seed_value FROM sp_probe_result o "
        " JOIN execution_source.exec_job j ON j.job_id=o.source_job_id "
        " JOIN sp_input_frame f ON f.input_frame_id=o.input_frame_id "
        " JOIN sp_axis_xy m ON m.axis_xy_id=f.main_axis_xy_id "
        " JOIN sp_axis_xy c ON c.axis_xy_id=f.cstick_axis_xy_id "
        " JOIN sp_axis_xy t ON t.axis_xy_id=f.trigger_axis_xy_id "
        " WHERE o.probe_run_id=?1 AND o.confirmation_of_probe_result_id IS NULL "
        " AND o.evidence_state='CONFIRMED' "
        " AND instr(char(10)||replace(COALESCE(j.input_ini,''),char(13),'')||char(10),"
        " char(10)||'stage=SURVEY'||char(10))>0 "
        " AND m.x=128 AND m.y=128 AND c.x=128 AND c.y=128 AND t.x=0 AND t.y=0 "
        " ORDER BY o.probe_result_id LIMIT 1"
        ") "
        "SELECT o.probe_result_id,o.probe_run_id,o.seed_value,"
        "((((o.seed_value-neutral.seed_value)+2147483648) & 4294967295)-2147483648),"
        "m.x,m.y,c.x,c.y,t.x,t.y "
        "FROM sp_probe_result o "
        "JOIN execution_source.exec_job j ON j.job_id=o.source_job_id "
        "JOIN sp_input_frame f ON f.input_frame_id=o.input_frame_id "
        "JOIN sp_axis_xy m ON m.axis_xy_id=f.main_axis_xy_id "
        "JOIN sp_axis_xy c ON c.axis_xy_id=f.cstick_axis_xy_id "
        "JOIN sp_axis_xy t ON t.axis_xy_id=f.trigger_axis_xy_id "
        "CROSS JOIN neutral "
        "WHERE o.probe_run_id=?1 AND o.confirmation_of_probe_result_id IS NULL "
        "AND o.evidence_state='CONFIRMED' "
        "AND instr(char(10)||replace(COALESCE(j.input_ini,''),char(13),'')||char(10),"
        " char(10)||'stage=SEARCH'||char(10))>0 "
        "ORDER BY o.probe_result_id;";
    if (!Prepare(source, kUniqueSrc, &unique_src, error_out)) return false;
    sqlite3_bind_int64(unique_src.st, 1, probe_run_id);
    while (sqlite3_step(unique_src.st) == SQLITE_ROW) {
        Statement ins;
        constexpr const char* kIns =
            "INSERT INTO ui_seed_probe_unique_value(unique_value_id,probe_run_id,seed_value,seed_delta,main_x,main_y,cstick_x,cstick_y,trigger_x,trigger_y) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);";
        if (!Prepare(ui, kIns, &ins, error_out)) return false;
        for (int i = 0; i < 10; ++i) BindColumn(ins.st, i + 1, unique_src.st, i);
        if (!StepDone(ui, ins.st, error_out)) return false;
    }

    return true;
}

std::int64_t ResolveBattleSetId(sqlite3* source, const OutboxEvent& event, std::string* error_out) {
    if (event.aggregate_kind == "battle_set") return ParseInt64(event.aggregate_id);
    if (event.payload_ref_kind == "battle_set") return event.payload_ref_id;
    const char* sql = nullptr;
    if (event.payload_ref_kind == "seed_candidate") sql = "SELECT battle_set_id FROM ab_seed_candidate WHERE seed_candidate_id=?1;";
    else if (event.payload_ref_kind == "turn_wave") sql = "SELECT battle_set_id FROM ab_turn_wave WHERE wave_id=?1;";
    else if (event.payload_ref_kind == "turn_job") sql = "SELECT w.battle_set_id FROM ab_turn_job j JOIN ab_turn_wave w ON w.wave_id=j.wave_id WHERE j.turn_job_id=?1;";
    else if (event.payload_ref_kind == "battle_advancement_pool") sql = "SELECT battle_set_id FROM ab_battle_advancement_pool WHERE battle_advancement_pool_id=?1;";
    else if (event.payload_ref_kind == "battle_advancement_decision") sql = "SELECT p.battle_set_id FROM ab_battle_advancement_decision d JOIN ab_battle_advancement_pool p ON p.battle_advancement_pool_id=d.battle_advancement_pool_id WHERE d.battle_advancement_decision_id=?1;";
    else if (event.payload_ref_kind == "manual_followup") sql = "SELECT w.battle_set_id FROM ab_manual_followup f JOIN ab_turn_job j ON j.turn_job_id=f.turn_job_id JOIN ab_turn_wave w ON w.wave_id=j.wave_id WHERE f.manual_followup_id=?1;";
    else if (event.payload_ref_kind == "context_probe") sql = "SELECT w.battle_set_id FROM ab_battle_context_probe p JOIN ab_turn_wave w ON w.wave_id=p.wave_id WHERE p.context_probe_id=?1;";
    if (sql == nullptr) return 0;
    Statement st;
    if (!Prepare(source, sql, &st, error_out)) return 0;
    sqlite3_bind_int64(st.st, 1, event.payload_ref_id);
    return sqlite3_step(st.st) == SQLITE_ROW ? sqlite3_column_int64(st.st, 0) : 0;
}

std::int64_t ResolveTurnJobId(sqlite3* source, const OutboxEvent& event, std::string* error_out) {
    if (event.payload_ref_kind == "turn_job") return event.payload_ref_id;
    if (event.aggregate_kind == "turn_job") return ParseInt64(event.aggregate_id);
    const char* sql = nullptr;
    if (event.payload_ref_kind == "manual_followup") {
        sql = "SELECT turn_job_id FROM ab_manual_followup WHERE manual_followup_id=?1;";
    } else if (event.payload_ref_kind == "battle_advancement_decision") {
        sql = "SELECT turn_job_id FROM ab_battle_advancement_decision WHERE battle_advancement_decision_id=?1;";
    }
    if (sql == nullptr) return 0;

    Statement st;
    if (!Prepare(source, sql, &st, error_out)) return 0;
    sqlite3_bind_int64(st.st, 1, event.payload_ref_id);
    return sqlite3_step(st.st) == SQLITE_ROW ? sqlite3_column_int64(st.st, 0) : 0;
}

std::int64_t ResolveWaveIdForTurnJob(sqlite3* source, std::int64_t turn_job_id, std::string* error_out) {
    if (turn_job_id <= 0) return 0;
    Statement st;
    if (!Prepare(source, "SELECT wave_id FROM ab_turn_job WHERE turn_job_id=?1;", &st, error_out)) return 0;
    sqlite3_bind_int64(st.st, 1, turn_job_id);
    return sqlite3_step(st.st) == SQLITE_ROW ? sqlite3_column_int64(st.st, 0) : 0;
}

std::int64_t ResolveBattleSetIdForWave(sqlite3* source, std::int64_t wave_id, std::string* error_out) {
    if (wave_id <= 0) return 0;
    Statement st;
    if (!Prepare(source, "SELECT battle_set_id FROM ab_turn_wave WHERE wave_id=?1;", &st, error_out)) return 0;
    sqlite3_bind_int64(st.st, 1, wave_id);
    return sqlite3_step(st.st) == SQLITE_ROW ? sqlite3_column_int64(st.st, 0) : 0;
}

bool RefreshBattleWaveRollup(sqlite3* ui, std::int64_t wave_id, std::string* error_out) {
    if (wave_id <= 0) return true;
    Statement st;
    constexpr const char* kSql =
        "UPDATE ui_battle_wave SET "
        "job_count=(SELECT COUNT(1) FROM ui_battle_turn_job WHERE wave_id=?1),"
        "selected_count=(SELECT COALESCE(SUM(CASE WHEN selected_for_advancement=1 THEN 1 ELSE 0 END),0) FROM ui_battle_turn_job WHERE wave_id=?1),"
        "desired_outcome_count=(SELECT COALESCE(SUM(CASE WHEN has_desired_outcome=1 THEN 1 ELSE 0 END),0) FROM ui_battle_turn_job WHERE wave_id=?1),"
        "final_victory_count=(SELECT COALESCE(SUM(CASE WHEN has_final_victory_outcome=1 THEN 1 ELSE 0 END),0) FROM ui_battle_turn_job WHERE wave_id=?1),"
        "failed_count=(SELECT COALESCE(SUM(CASE WHEN job_state='FAILED' THEN 1 ELSE 0 END),0) FROM ui_battle_turn_job WHERE wave_id=?1),"
        "advancement_rank=(SELECT COALESCE(MAX(advancement_rank),0) FROM ui_battle_turn_job WHERE wave_id=?1) "
        "WHERE wave_id=?1;";
    if (!Prepare(ui, kSql, &st, error_out)) return false;
    sqlite3_bind_int64(st.st, 1, wave_id);
    return StepDone(ui, st.st, error_out);
}

bool RefreshBattleGroupRollup(sqlite3* ui, std::int64_t battle_set_id, std::string* error_out) {
    if (battle_set_id <= 0) return true;
    Statement st;
    constexpr const char* kSql =
        "UPDATE ui_battle_group SET "
        "wave_count=(SELECT COUNT(1) FROM ui_battle_wave WHERE battle_set_id=?1),"
        "turn_job_count=(SELECT COALESCE(SUM(job_count),0) FROM ui_battle_wave WHERE battle_set_id=?1),"
        "selected_count=(SELECT COALESCE(SUM(selected_count),0) FROM ui_battle_wave WHERE battle_set_id=?1),"
        "desired_outcome_count=(SELECT COALESCE(SUM(desired_outcome_count),0) FROM ui_battle_wave WHERE battle_set_id=?1),"
        "final_victory_count=(SELECT COALESCE(SUM(final_victory_count),0) FROM ui_battle_wave WHERE battle_set_id=?1),"
        "failed_count=(SELECT COALESCE(SUM(failed_count),0) FROM ui_battle_wave WHERE battle_set_id=?1),"
        "manual_followup_count=("
        "SELECT COUNT(1) FROM ui_battle_manual_followup f "
        "JOIN ui_battle_turn_job j ON j.turn_job_id=f.turn_job_id "
        "JOIN ui_battle_wave w ON w.wave_id=j.wave_id WHERE w.battle_set_id=?1),"
        "advancement_rank=(SELECT COALESCE(MAX(advancement_rank),0) FROM ui_battle_wave WHERE battle_set_id=?1) "
        "WHERE battle_set_id=?1;";
    if (!Prepare(ui, kSql, &st, error_out)) return false;
    sqlite3_bind_int64(st.st, 1, battle_set_id);
    return StepDone(ui, st.st, error_out);
}

bool RefreshWorkflowInstanceBattleRollup(sqlite3* ui, std::int64_t workflow_instance_id, std::string* error_out) {
    if (workflow_instance_id <= 0) return true;
    Statement st;
    constexpr const char* kSql =
        "UPDATE ui_workflow_instance SET "
        "battle_advancement_rank=(SELECT COALESCE(MAX(battle_advancement_rank),0) FROM ui_workflow_step WHERE workflow_instance_id=?1),"
        "battle_desired_outcome_count=(SELECT COALESCE(SUM(battle_desired_outcome_count),0) FROM ui_workflow_step WHERE workflow_instance_id=?1),"
        "battle_final_victory_count=(SELECT COALESCE(SUM(battle_final_victory_count),0) FROM ui_workflow_step WHERE workflow_instance_id=?1),"
        "battle_selected_count=(SELECT COALESCE(SUM(battle_selected_count),0) FROM ui_workflow_step WHERE workflow_instance_id=?1) "
        "WHERE workflow_instance_id=?1;";
    if (!Prepare(ui, kSql, &st, error_out)) return false;
    sqlite3_bind_int64(st.st, 1, workflow_instance_id);
    return StepDone(ui, st.st, error_out);
}

bool RefreshWorkflowStepBattleRollup(sqlite3* ui, std::int64_t job_set_id, std::string* error_out) {
    if (job_set_id <= 0) return true;
    Statement st;
    constexpr const char* kSql =
        "UPDATE ui_workflow_step SET "
        "battle_advancement_rank=("
        "SELECT COALESCE(MAX(b.advancement_rank),0) FROM ui_job_summary j INDEXED BY ix_ui_job_summary_job_set_job "
        "JOIN ui_battle_turn_job b ON b.exec_job_id=j.job_id WHERE j.job_set_id=?1),"
        "battle_desired_outcome_count=("
        "SELECT COALESCE(SUM(CASE WHEN b.has_desired_outcome=1 THEN 1 ELSE 0 END),0) FROM ui_job_summary j INDEXED BY ix_ui_job_summary_job_set_job "
        "JOIN ui_battle_turn_job b ON b.exec_job_id=j.job_id WHERE j.job_set_id=?1),"
        "battle_final_victory_count=("
        "SELECT COALESCE(SUM(CASE WHEN b.has_final_victory_outcome=1 THEN 1 ELSE 0 END),0) FROM ui_job_summary j INDEXED BY ix_ui_job_summary_job_set_job "
        "JOIN ui_battle_turn_job b ON b.exec_job_id=j.job_id WHERE j.job_set_id=?1),"
        "battle_selected_count=("
        "SELECT COALESCE(SUM(CASE WHEN b.selected_for_advancement=1 THEN 1 ELSE 0 END),0) FROM ui_job_summary j INDEXED BY ix_ui_job_summary_job_set_job "
        "JOIN ui_battle_turn_job b ON b.exec_job_id=j.job_id WHERE j.job_set_id=?1) "
        "WHERE job_set_id=?1;";
    if (!Prepare(ui, kSql, &st, error_out)) return false;
    sqlite3_bind_int64(st.st, 1, job_set_id);
    return StepDone(ui, st.st, error_out);
}

bool RefreshWorkflowBattleRollupsForJobSet(sqlite3* ui, std::int64_t job_set_id, std::string* error_out) {
    if (job_set_id <= 0) return true;
    if (!RefreshWorkflowStepBattleRollup(ui, job_set_id, error_out)) return false;

    Statement workflows;
    if (!Prepare(ui, "SELECT workflow_instance_id FROM ui_workflow_step WHERE job_set_id=?1;", &workflows, error_out)) return false;
    sqlite3_bind_int64(workflows.st, 1, job_set_id);
    while (sqlite3_step(workflows.st) == SQLITE_ROW) {
        if (!RefreshWorkflowInstanceBattleRollup(ui, sqlite3_column_int64(workflows.st, 0), error_out)) return false;
    }
    return true;
}

bool RefreshWorkflowBattleRollupsForWorkflow(sqlite3* ui, std::int64_t workflow_instance_id, std::string* error_out) {
    if (workflow_instance_id <= 0) return true;
    Statement steps;
    if (!Prepare(ui, "SELECT DISTINCT job_set_id FROM ui_workflow_step WHERE workflow_instance_id=?1 AND job_set_id IS NOT NULL;", &steps, error_out)) return false;
    sqlite3_bind_int64(steps.st, 1, workflow_instance_id);
    while (sqlite3_step(steps.st) == SQLITE_ROW) {
        if (!RefreshWorkflowStepBattleRollup(ui, sqlite3_column_int64(steps.st, 0), error_out)) return false;
    }
    return RefreshWorkflowInstanceBattleRollup(ui, workflow_instance_id, error_out);
}

bool RefreshWorkflowBattleRollupsForExecJob(sqlite3* ui, std::int64_t exec_job_id, std::string* error_out) {
    if (exec_job_id <= 0) return true;
    Statement job_sets;
    constexpr const char* kSql =
        "SELECT DISTINCT s.job_set_id "
        "FROM ui_job_summary j JOIN ui_workflow_step s INDEXED BY ix_ui_workflow_step_job_set_instance ON s.job_set_id=j.job_set_id "
        "WHERE j.job_id=?1 AND s.job_set_id IS NOT NULL;";
    if (!Prepare(ui, kSql, &job_sets, error_out)) return false;
    sqlite3_bind_int64(job_sets.st, 1, exec_job_id);
    while (sqlite3_step(job_sets.st) == SQLITE_ROW) {
        if (!RefreshWorkflowBattleRollupsForJobSet(ui, sqlite3_column_int64(job_sets.st, 0), error_out)) return false;
    }
    return true;
}

bool ProjectBattleGroup(sqlite3* source, sqlite3* ui, std::int64_t battle_set_id, std::string* error_out) {
    if (battle_set_id <= 0) return true;
    Statement group_src;
    if (!Prepare(source, "SELECT battle_set_id,name,status,created_at_utc,completed_at_utc FROM ab_battle_set WHERE battle_set_id=?1;", &group_src, error_out)) return false;
    sqlite3_bind_int64(group_src.st, 1, battle_set_id);
    if (sqlite3_step(group_src.st) == SQLITE_ROW) {
        Statement upsert;
        constexpr const char* kSql =
            "INSERT INTO ui_battle_group(battle_set_id,name,status,created_at_utc,completed_at_utc) VALUES(?1,?2,?3,?4,?5) "
            "ON CONFLICT(battle_set_id) DO UPDATE SET name=excluded.name,status=excluded.status,created_at_utc=excluded.created_at_utc,completed_at_utc=excluded.completed_at_utc;";
        if (!Prepare(ui, kSql, &upsert, error_out)) return false;
        for (int i = 0; i < 5; ++i) BindColumn(upsert.st, i + 1, group_src.st, i);
        if (!StepDone(ui, upsert.st, error_out)) return false;
    }
    return RefreshBattleGroupRollup(ui, battle_set_id, error_out);
}

bool ProjectBattleWave(sqlite3* source, sqlite3* ui, std::int64_t wave_id, std::string* error_out) {
    if (wave_id <= 0) return true;
    Statement waves;
    if (!Prepare(source, "SELECT wave_id,battle_set_id,parent_wave_id,parent_turn_job_id,turn_index,status,created_at_utc,completed_at_utc FROM ab_turn_wave WHERE wave_id=?1;", &waves, error_out)) return false;
    sqlite3_bind_int64(waves.st, 1, wave_id);
    while (sqlite3_step(waves.st) == SQLITE_ROW) {
        const std::int64_t battle_set_id = sqlite3_column_int64(waves.st, 1);
        Statement upsert;
        constexpr const char* kSql =
            "INSERT INTO ui_battle_wave(wave_id,battle_set_id,parent_wave_id,parent_turn_job_id,turn_index,status,created_at_utc,completed_at_utc) VALUES(?1,?2,?3,?4,?5,?6,?7,?8) "
            "ON CONFLICT(wave_id) DO UPDATE SET battle_set_id=excluded.battle_set_id,parent_wave_id=excluded.parent_wave_id,parent_turn_job_id=excluded.parent_turn_job_id,turn_index=excluded.turn_index,status=excluded.status,created_at_utc=excluded.created_at_utc,completed_at_utc=excluded.completed_at_utc;";
        if (!Prepare(ui, kSql, &upsert, error_out)) return false;
        for (int i = 0; i < 8; ++i) BindColumn(upsert.st, i + 1, waves.st, i);
        if (!StepDone(ui, upsert.st, error_out)) return false;
        if (!RefreshBattleWaveRollup(ui, wave_id, error_out)) return false;
        if (!RefreshBattleGroupRollup(ui, battle_set_id, error_out)) return false;
    }
    return true;
}

bool ProjectBattleTurnJob(sqlite3* source, sqlite3* ui, std::int64_t turn_job_id, std::string* error_out, bool refresh_rollups = true) {
    if (turn_job_id <= 0) return true;
    Statement jobs;
    constexpr const char* kJobs =
        "SELECT j.turn_job_id,j.exec_job_id,j.wave_id,j.job_state,j.fake_attacks_this_turn,j.fake_attacks_used_before,j.rng_seed,j.delta_vi,j.pred_passed,j.pred_total,j.battle_outcome,"
        "CASE WHEN j.battle_outcome IN (0,6) THEN 1 ELSE 0 END,"
        "CASE WHEN j.battle_outcome=0 THEN 1 ELSE 0 END,"
        "CASE WHEN EXISTS (SELECT 1 FROM ab_battle_advancement_decision d WHERE d.turn_job_id=j.turn_job_id AND d.decision_kind='SELECTED') THEN 1 ELSE 0 END,"
        "(SELECT d.decision_kind FROM ab_battle_advancement_decision d WHERE d.turn_job_id=j.turn_job_id ORDER BY CASE d.decision_kind WHEN 'SELECTED' THEN 0 WHEN 'NOT_SELECTED' THEN 1 ELSE 2 END,d.battle_advancement_decision_id DESC LIMIT 1),"
        "CASE WHEN EXISTS (SELECT 1 FROM ab_battle_advancement_decision d WHERE d.turn_job_id=j.turn_job_id AND d.decision_kind='SELECTED') THEN 2 WHEN j.battle_outcome IN (0,6) THEN 1 ELSE 0 END,"
        "j.started_at_utc,j.ended_at_utc,w.battle_set_id,w.parent_wave_id,w.parent_turn_job_id,"
        "j.source_savestate_id,j.seed_candidate_id,j.authored_plan_id,j.authored_turn_index,j.resolved_turn_commands_blob,j.resolved_turn_variant_key,j.output_savestate_id,j.input_trace_artifact_id "
        "FROM ab_turn_job j JOIN ab_turn_wave w ON w.wave_id=j.wave_id WHERE j.turn_job_id=?1;";
    if (!Prepare(source, kJobs, &jobs, error_out)) return false;
    sqlite3_bind_int64(jobs.st, 1, turn_job_id);
    while (sqlite3_step(jobs.st) == SQLITE_ROW) {
        const std::int64_t exec_job_id = sqlite3_column_type(jobs.st, 1) == SQLITE_NULL ? 0 : sqlite3_column_int64(jobs.st, 1);
        const std::int64_t wave_id = sqlite3_column_int64(jobs.st, 2);
        const std::int64_t battle_set_id = ResolveBattleSetIdForWave(source, wave_id, error_out);
        Statement upsert;
        constexpr const char* kSql =
            "INSERT INTO ui_battle_turn_job(turn_job_id,exec_job_id,wave_id,job_state,fake_attacks_this_turn,fake_attacks_used_before,rng_seed,delta_vi,pred_passed,pred_total,battle_outcome,has_desired_outcome,has_final_victory_outcome,selected_for_advancement,advancement_decision_kind,advancement_rank,started_at_utc,ended_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18) "
            "ON CONFLICT(turn_job_id) DO UPDATE SET exec_job_id=excluded.exec_job_id,wave_id=excluded.wave_id,job_state=excluded.job_state,fake_attacks_this_turn=excluded.fake_attacks_this_turn,"
            "fake_attacks_used_before=excluded.fake_attacks_used_before,rng_seed=excluded.rng_seed,delta_vi=excluded.delta_vi,pred_passed=excluded.pred_passed,pred_total=excluded.pred_total,"
            "battle_outcome=excluded.battle_outcome,has_desired_outcome=excluded.has_desired_outcome,has_final_victory_outcome=excluded.has_final_victory_outcome,selected_for_advancement=excluded.selected_for_advancement,"
            "advancement_decision_kind=excluded.advancement_decision_kind,advancement_rank=excluded.advancement_rank,started_at_utc=excluded.started_at_utc,ended_at_utc=excluded.ended_at_utc;";
        if (!Prepare(ui, kSql, &upsert, error_out)) return false;
        for (int i = 0; i < 18; ++i) BindColumn(upsert.st, i + 1, jobs.st, i);
        if (!StepDone(ui, upsert.st, error_out)) return false;
        Statement replication;
        constexpr const char* kReplicationSql =
            "INSERT INTO ui_battle_turn_job_replication(turn_job_id,battle_set_id,wave_id,parent_wave_id,parent_turn_job_id,exec_job_id,source_savestate_id,seed_candidate_id,authored_plan_id,authored_turn_index,resolved_turn_commands_blob,resolved_turn_variant_key,fake_attacks_used_before,fake_attacks_this_turn,output_savestate_id,input_trace_artifact_id) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16) "
            "ON CONFLICT(turn_job_id) DO UPDATE SET battle_set_id=excluded.battle_set_id,wave_id=excluded.wave_id,parent_wave_id=excluded.parent_wave_id,parent_turn_job_id=excluded.parent_turn_job_id,exec_job_id=excluded.exec_job_id,"
            "source_savestate_id=excluded.source_savestate_id,seed_candidate_id=excluded.seed_candidate_id,authored_plan_id=excluded.authored_plan_id,authored_turn_index=excluded.authored_turn_index,"
            "resolved_turn_commands_blob=excluded.resolved_turn_commands_blob,resolved_turn_variant_key=excluded.resolved_turn_variant_key,fake_attacks_used_before=excluded.fake_attacks_used_before,"
            "fake_attacks_this_turn=excluded.fake_attacks_this_turn,output_savestate_id=excluded.output_savestate_id,input_trace_artifact_id=excluded.input_trace_artifact_id;";
        if (!Prepare(ui, kReplicationSql, &replication, error_out)) return false;
        BindColumn(replication.st, 1, jobs.st, 0);
        BindColumn(replication.st, 2, jobs.st, 18);
        BindColumn(replication.st, 3, jobs.st, 2);
        BindColumn(replication.st, 4, jobs.st, 19);
        BindColumn(replication.st, 5, jobs.st, 20);
        BindColumn(replication.st, 6, jobs.st, 1);
        BindColumn(replication.st, 7, jobs.st, 21);
        BindColumn(replication.st, 8, jobs.st, 22);
        BindColumn(replication.st, 9, jobs.st, 23);
        BindColumn(replication.st, 10, jobs.st, 24);
        BindColumn(replication.st, 11, jobs.st, 25);
        BindColumn(replication.st, 12, jobs.st, 26);
        BindColumn(replication.st, 13, jobs.st, 5);
        BindColumn(replication.st, 14, jobs.st, 4);
        BindColumn(replication.st, 15, jobs.st, 27);
        BindColumn(replication.st, 16, jobs.st, 28);
        if (!StepDone(ui, replication.st, error_out)) return false;
        if (refresh_rollups) {
            if (!ProjectBattleWave(source, ui, wave_id, error_out)) return false;
            if (!RefreshBattleWaveRollup(ui, wave_id, error_out)) return false;
            if (!RefreshBattleGroupRollup(ui, battle_set_id, error_out)) return false;
        }
        if (!RefreshWorkflowBattleRollupsForExecJob(ui, exec_job_id, error_out)) return false;
    }
    return true;
}

bool ProjectBattleAdvancementDecision(sqlite3* source, sqlite3* ui, std::int64_t battle_advancement_decision_id, std::string* error_out) {
    if (battle_advancement_decision_id <= 0) return true;
    Statement decisions;
    constexpr const char* kDecisions =
        "SELECT battle_advancement_decision_id,battle_advancement_pool_id,turn_job_id,decision_kind,decision_reason,created_at_utc "
        "FROM ab_battle_advancement_decision WHERE battle_advancement_decision_id=?1;";
    if (!Prepare(source, kDecisions, &decisions, error_out)) return false;
    sqlite3_bind_int64(decisions.st, 1, battle_advancement_decision_id);
    while (sqlite3_step(decisions.st) == SQLITE_ROW) {
        const std::int64_t turn_job_id = sqlite3_column_int64(decisions.st, 2);
        Statement upsert;
        constexpr const char* kSql =
            "INSERT INTO ui_battle_advancement_decision(battle_advancement_decision_id,battle_advancement_pool_id,turn_job_id,decision_kind,decision_reason,created_at_utc) VALUES(?1,?2,?3,?4,?5,?6) "
            "ON CONFLICT(battle_advancement_decision_id) DO UPDATE SET battle_advancement_pool_id=excluded.battle_advancement_pool_id,turn_job_id=excluded.turn_job_id,decision_kind=excluded.decision_kind,decision_reason=excluded.decision_reason,created_at_utc=excluded.created_at_utc;";
        if (!Prepare(ui, kSql, &upsert, error_out)) return false;
        for (int i = 0; i < 6; ++i) BindColumn(upsert.st, i + 1, decisions.st, i);
        if (!StepDone(ui, upsert.st, error_out)) return false;
        if (!ProjectBattleTurnJob(source, ui, turn_job_id, error_out)) return false;
    }
    return true;
}

bool ProjectBattleManualFollowupForTurnJob(sqlite3* source, sqlite3* ui, std::int64_t turn_job_id, std::string* error_out) {
    if (turn_job_id <= 0) return true;
    Statement followups;
    constexpr const char* kFollowups =
        "SELECT f.turn_job_id,f.manual_followup_status,f.recorded_dtm_artifact_id,f.note,f.updated_at_utc "
        "FROM ab_manual_followup f WHERE f.turn_job_id=?1;";
    if (!Prepare(source, kFollowups, &followups, error_out)) return false;
    sqlite3_bind_int64(followups.st, 1, turn_job_id);
    while (sqlite3_step(followups.st) == SQLITE_ROW) {
        const auto wave_id = ResolveWaveIdForTurnJob(source, turn_job_id, error_out);
        const auto battle_set_id = ResolveBattleSetIdForWave(source, wave_id, error_out);
        Statement upsert;
        constexpr const char* kSql =
            "INSERT INTO ui_battle_manual_followup(turn_job_id,manual_followup_status,recorded_dtm_artifact_id,note,updated_at_utc) VALUES(?1,?2,?3,?4,?5) "
            "ON CONFLICT(turn_job_id) DO UPDATE SET manual_followup_status=excluded.manual_followup_status,recorded_dtm_artifact_id=excluded.recorded_dtm_artifact_id,note=excluded.note,updated_at_utc=excluded.updated_at_utc;";
        if (!Prepare(ui, kSql, &upsert, error_out)) return false;
        for (int i = 0; i < 5; ++i) BindColumn(upsert.st, i + 1, followups.st, i);
        if (!StepDone(ui, upsert.st, error_out)) return false;
        if (!RefreshBattleGroupRollup(ui, battle_set_id, error_out)) return false;
    }
    return true;
}

bool ProjectBattleSet(sqlite3* source, sqlite3* ui, std::int64_t battle_set_id, std::string* error_out) {
    if (battle_set_id <= 0) return true;
    if (!ProjectBattleGroup(source, ui, battle_set_id, error_out)) return false;

    Statement waves;
    if (!Prepare(source, "SELECT wave_id FROM ab_turn_wave WHERE battle_set_id=?1;", &waves, error_out)) return false;
    sqlite3_bind_int64(waves.st, 1, battle_set_id);
    while (sqlite3_step(waves.st) == SQLITE_ROW) {
        if (!ProjectBattleWave(source, ui, sqlite3_column_int64(waves.st, 0), error_out)) return false;
    }

    Statement jobs;
    constexpr const char* kJobs =
        "SELECT j.turn_job_id "
        "FROM ab_turn_job j JOIN ab_turn_wave w ON w.wave_id=j.wave_id WHERE w.battle_set_id=?1;";
    if (!Prepare(source, kJobs, &jobs, error_out)) return false;
    sqlite3_bind_int64(jobs.st, 1, battle_set_id);
    while (sqlite3_step(jobs.st) == SQLITE_ROW) {
        if (!ProjectBattleTurnJob(source, ui, sqlite3_column_int64(jobs.st, 0), error_out, false)) return false;
    }

    sqlite3_reset(waves.st);
    sqlite3_clear_bindings(waves.st);
    sqlite3_bind_int64(waves.st, 1, battle_set_id);
    while (sqlite3_step(waves.st) == SQLITE_ROW) {
        if (!RefreshBattleWaveRollup(ui, sqlite3_column_int64(waves.st, 0), error_out)) return false;
    }

    Statement decisions;
    constexpr const char* kDecisions =
        "SELECT d.battle_advancement_decision_id "
        "FROM ab_battle_advancement_decision d "
        "JOIN ab_battle_advancement_pool p ON p.battle_advancement_pool_id=d.battle_advancement_pool_id "
        "WHERE p.battle_set_id=?1;";
    if (!Prepare(source, kDecisions, &decisions, error_out)) return false;
    sqlite3_bind_int64(decisions.st, 1, battle_set_id);
    while (sqlite3_step(decisions.st) == SQLITE_ROW) {
        if (!ProjectBattleAdvancementDecision(source, ui, sqlite3_column_int64(decisions.st, 0), error_out)) return false;
    }

    Statement followups;
    constexpr const char* kFollowups =
        "SELECT f.turn_job_id "
        "FROM ab_manual_followup f JOIN ab_turn_job j ON j.turn_job_id=f.turn_job_id JOIN ab_turn_wave w ON w.wave_id=j.wave_id WHERE w.battle_set_id=?1;";
    if (!Prepare(source, kFollowups, &followups, error_out)) return false;
    sqlite3_bind_int64(followups.st, 1, battle_set_id);
    while (sqlite3_step(followups.st) == SQLITE_ROW) {
        if (!ProjectBattleManualFollowupForTurnJob(source, ui, sqlite3_column_int64(followups.st, 0), error_out)) return false;
    }
    return RefreshBattleGroupRollup(ui, battle_set_id, error_out);
}

bool ProjectArchive(sqlite3* source, sqlite3* ui, const OutboxEvent& event, std::string* error_out) {
    std::int64_t package_id = 0;
    if (event.aggregate_kind == "archive_package" || event.payload_ref_kind == "archive_package") {
        package_id = event.payload_ref_kind == "archive_package" ? event.payload_ref_id : ParseInt64(event.aggregate_id);
    } else if (event.payload_ref_kind == "rehydrate_request") {
        Statement st;
        if (!Prepare(source, "SELECT archive_package_id FROM ar_rehydrate_request WHERE rehydrate_request_id=?1;", &st, error_out)) return false;
        sqlite3_bind_int64(st.st, 1, event.payload_ref_id);
        if (sqlite3_step(st.st) == SQLITE_ROW) package_id = sqlite3_column_int64(st.st, 0);
    }

    if (package_id > 0) {
        Statement pkg;
        constexpr const char* kPkg =
            "SELECT archive_package_id,source_context,source_root_job_set_id,source_scope_kind,source_workflow_count,selection_summary,archive_name,archive_notes,created_at_utc,schema_version,event_catalog_version,time_range_start_utc,time_range_end_utc,checksum_status "
            "FROM ar_archive_package WHERE archive_package_id=?1;";
        if (!Prepare(source, kPkg, &pkg, error_out)) return false;
        sqlite3_bind_int64(pkg.st, 1, package_id);
        if (sqlite3_step(pkg.st) == SQLITE_ROW) {
            Statement upsert;
            constexpr const char* kSql =
                "INSERT INTO ui_archive_catalog(archive_package_id,source_context,source_root_job_set_id,source_scope_kind,source_workflow_count,selection_summary,archive_name,archive_notes,created_at_utc,schema_version,event_catalog_version,time_range_start_utc,time_range_end_utc,checksum_status) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14) "
                "ON CONFLICT(archive_package_id) DO UPDATE SET source_context=excluded.source_context,source_root_job_set_id=excluded.source_root_job_set_id,source_scope_kind=excluded.source_scope_kind,source_workflow_count=excluded.source_workflow_count,selection_summary=excluded.selection_summary,archive_name=excluded.archive_name,archive_notes=excluded.archive_notes,created_at_utc=excluded.created_at_utc,"
                "schema_version=excluded.schema_version,event_catalog_version=excluded.event_catalog_version,time_range_start_utc=excluded.time_range_start_utc,time_range_end_utc=excluded.time_range_end_utc,checksum_status=excluded.checksum_status;";
            if (!Prepare(ui, kSql, &upsert, error_out)) return false;
            for (int i = 0; i < 14; ++i) BindColumn(upsert.st, i + 1, pkg.st, i);
            if (!StepDone(ui, upsert.st, error_out)) return false;
        }
    }

    const std::int64_t request_filter = event.payload_ref_kind == "rehydrate_request" ? event.payload_ref_id : 0;
    Statement req;
    const char* kReqByPackage =
        "SELECT rehydrate_request_id,archive_package_id,status,target_namespace,requested_at_utc,completed_at_utc,error_text FROM ar_rehydrate_request WHERE archive_package_id=?1;";
    const char* kReqById =
        "SELECT rehydrate_request_id,archive_package_id,status,target_namespace,requested_at_utc,completed_at_utc,error_text FROM ar_rehydrate_request WHERE rehydrate_request_id=?1;";
    if (!Prepare(source, request_filter > 0 ? kReqById : kReqByPackage, &req, error_out)) return false;
    sqlite3_bind_int64(req.st, 1, request_filter > 0 ? request_filter : package_id);
    while (sqlite3_step(req.st) == SQLITE_ROW) {
        Statement upsert;
        constexpr const char* kSql =
            "INSERT INTO ui_archive_rehydrate_request(rehydrate_request_id,archive_package_id,status,target_namespace,requested_at_utc,completed_at_utc,error_text) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7) "
            "ON CONFLICT(rehydrate_request_id) DO UPDATE SET archive_package_id=excluded.archive_package_id,status=excluded.status,target_namespace=excluded.target_namespace,requested_at_utc=excluded.requested_at_utc,completed_at_utc=excluded.completed_at_utc,error_text=excluded.error_text;";
        if (!Prepare(ui, kSql, &upsert, error_out)) return false;
        for (int i = 0; i < 7; ++i) BindColumn(upsert.st, i + 1, req.st, i);
        if (!StepDone(ui, upsert.st, error_out)) return false;
    }
    return true;
}

enum class StreamKind {
    Execution,
    State,
    AnalysisSeedProbe,
    AnalysisBattle,
    Archive,
};

bool AddDirty(
    std::vector<DirtyEntity>* dirty,
    std::string kind,
    std::int64_t id,
    const OutboxEvent& event,
    std::string* error_out) {
    if (id <= 0) {
        if (error_out != nullptr) {
            *error_out = "projection event did not resolve required entity id: " + event.event_type;
        }
        return false;
    }
    dirty->push_back(DirtyEntity{
        .kind = std::move(kind),
        .id = id,
        .outbox_id = event.outbox_id,
    });
    return true;
}

bool ClassifyOutboxEvent(
    StreamKind kind,
    sqlite3* source,
    const OutboxEvent& event,
    std::vector<DirtyEntity>* dirty,
    std::string* error_out) {
    switch (kind) {
    case StreamKind::Execution:
        if (IsExecutionWorkflowEvent(event.event_type)
            || event.aggregate_kind == "workflow"
            || event.aggregate_kind == "workflow_instance") {
            return AddDirty(dirty, "workflow", ParseInt64(event.aggregate_id), event, error_out);
        }
        if (IsExecutionJobEvent(event.event_type)
            || event.aggregate_kind == "job"
            || event.aggregate_kind == "job_set") {
            if (event.event_type == "Execution.JobSetCreated.v1" || event.aggregate_kind == "job_set") {
                const auto job_set_id = ParseInt64(event.aggregate_id);
                if (!AddDirty(dirty, "job_set", job_set_id, event, error_out)) return false;
                const auto workflow_instance_id = ResolveWorkflowInstanceForJobSet(source, job_set_id, error_out);
                return workflow_instance_id <= 0 || AddDirty(dirty, "workflow", workflow_instance_id, event, error_out);
            }
            const auto job_id = ParseInt64(event.aggregate_id);
            if (!AddDirty(dirty, "job", job_id, event, error_out)) return false;
            if (event.event_type == "Execution.JobQueued.v1"
                || event.event_type == "Execution.JobCompleted.v1"
                || event.event_type == "Execution.JobRestored.v1") {
                const auto workflow_instance_id = ResolveWorkflowInstanceForJob(source, job_id, error_out);
                return workflow_instance_id <= 0 || AddDirty(dirty, "workflow", workflow_instance_id, event, error_out);
            }
            return true;
        }
        break;
    case StreamKind::State:
        if (event.event_type == "State.ArtifactStored.v1") {
            return AddDirty(dirty, "artifact", event.payload_ref_id, event, error_out);
        }
        break;
    case StreamKind::AnalysisSeedProbe:
        if (event.event_type == "AnalysisSeedProbe.SetCreated.v1") {
            return true;
        }
        if (event.event_type == "AnalysisSeedProbe.RunRequested.v1"
            || event.event_type == "AnalysisSeedProbe.ObservationRecorded.v1"
            || event.event_type == "AnalysisSeedProbe.EvidenceStateChanged.v1"
            || event.event_type == "AnalysisSeedProbe.AcceptedInputFramesReplaced.v1"
            || event.event_type == "AnalysisSeedProbe.RunStatusChanged.v1"
            || event.event_type == "AnalysisSeedProbe.EncounterProjectionRecorded.v1"
            || event.event_type == "AnalysisSeedProbe.RunCompleted.v1"
            || event.event_type == "AnalysisSeedProbe.RunFailed.v1") {
            return AddDirty(dirty, "seed_probe_run", ResolveProbeRunId(source, event, error_out), event, error_out);
        }
        break;
    case StreamKind::AnalysisBattle:
        if (event.event_type == "AnalysisBattle.BattleSetCreated.v1") {
            return AddDirty(dirty, "battle_group", ResolveBattleSetId(source, event, error_out), event, error_out);
        }
        if (event.event_type == "AnalysisBattle.TurnWaveCreated.v1") {
            const auto wave_id = event.payload_ref_kind == "turn_wave" ? event.payload_ref_id : ParseInt64(event.aggregate_id);
            if (!AddDirty(dirty, "battle_wave", wave_id, event, error_out)) return false;
            const auto battle_set_id = ResolveBattleSetId(source, event, error_out);
            return battle_set_id <= 0 || AddDirty(dirty, "battle_group", battle_set_id, event, error_out);
        }
        if (event.event_type == "AnalysisBattle.TurnJobRecorded.v1") {
            const auto turn_job_id = ResolveTurnJobId(source, event, error_out);
            if (!AddDirty(dirty, "battle_turn_job", turn_job_id, event, error_out)) return false;
            const auto wave_id = ResolveWaveIdForTurnJob(source, turn_job_id, error_out);
            if (wave_id > 0 && !AddDirty(dirty, "battle_wave", wave_id, event, error_out)) return false;
            const auto battle_set_id = ResolveBattleSetId(source, event, error_out);
            return battle_set_id <= 0 || AddDirty(dirty, "battle_group", battle_set_id, event, error_out);
        }
        if (event.event_type == "AnalysisBattle.TurnJobResultUpdated.v1") {
            const auto turn_job_id = ResolveTurnJobId(source, event, error_out);
            if (!AddDirty(dirty, "battle_turn_job", turn_job_id, event, error_out)) return false;
            const auto wave_id = ResolveWaveIdForTurnJob(source, turn_job_id, error_out);
            if (wave_id > 0 && !AddDirty(dirty, "battle_wave", wave_id, event, error_out)) return false;
            const auto battle_set_id = ResolveBattleSetId(source, event, error_out);
            return battle_set_id <= 0 || AddDirty(dirty, "battle_group", battle_set_id, event, error_out);
        }
        if (event.event_type == "AnalysisBattle.TurnWaveStatusUpdated.v1") {
            const auto wave_id = event.payload_ref_kind == "turn_wave" ? event.payload_ref_id : ParseInt64(event.aggregate_id);
            if (!AddDirty(dirty, "battle_wave", wave_id, event, error_out)) return false;
            const auto battle_set_id = ResolveBattleSetId(source, event, error_out);
            return battle_set_id <= 0 || AddDirty(dirty, "battle_group", battle_set_id, event, error_out);
        }
        if (event.event_type == "AnalysisBattle.BattleSetStatusUpdated.v1") {
            return AddDirty(dirty, "battle_group", ResolveBattleSetId(source, event, error_out), event, error_out);
        }
        if (event.event_type == "AnalysisBattle.ManualFollowupUpdated.v1") {
            const auto turn_job_id = ResolveTurnJobId(source, event, error_out);
            if (!AddDirty(dirty, "battle_manual_followup", turn_job_id, event, error_out)) return false;
            const auto battle_set_id = ResolveBattleSetId(source, event, error_out);
            return battle_set_id <= 0 || AddDirty(dirty, "battle_group", battle_set_id, event, error_out);
        }
        if (event.event_type == "AnalysisBattle.BattleAdvancementDecisionRecorded.v1") {
            const auto turn_job_id = ResolveTurnJobId(source, event, error_out);
            if (!AddDirty(dirty, "battle_advancement_decision", event.payload_ref_id, event, error_out)) return false;
            if (!AddDirty(dirty, "battle_turn_job", turn_job_id, event, error_out)) return false;
            const auto wave_id = ResolveWaveIdForTurnJob(source, turn_job_id, error_out);
            if (wave_id > 0 && !AddDirty(dirty, "battle_wave", wave_id, event, error_out)) return false;
            const auto battle_set_id = ResolveBattleSetId(source, event, error_out);
            return battle_set_id <= 0 || AddDirty(dirty, "battle_group", battle_set_id, event, error_out);
        }
        if (event.event_type == "AnalysisBattle.SeedCandidateAdded.v1"
            || event.event_type == "AnalysisBattle.ContextProbeCreated.v1"
            || event.event_type == "AnalysisBattle.BattleAdvancementPoolCreated.v1") {
            return true;
        }
        break;
    case StreamKind::Archive:
        if (event.event_type == "Archive.PackageCreated.v1"
            || event.event_type == "Archive.PackageIndexed.v1"
            || event.event_type == "Archive.RehydrateRequested.v1"
            || event.event_type == "Archive.RehydrateCompleted.v1"
            || event.event_type == "Archive.RehydrateFailed.v1") {
            if (event.payload_ref_kind == "rehydrate_request") {
                return AddDirty(dirty, "rehydrate_request", event.payload_ref_id, event, error_out);
            }
            const auto package_id = event.payload_ref_kind == "archive_package" ? event.payload_ref_id : ParseInt64(event.aggregate_id);
            return AddDirty(dirty, "archive_package", package_id, event, error_out);
        }
        break;
    }

    return true;
}

} // namespace

struct UiReadProjectionService::StreamRuntime {
    StreamKind kind = StreamKind::Execution;
    std::string stream_id;
    std::string projector_name = "UiReadProjector";
    std::string source_context;
    std::string source_outbox_table;
    std::filesystem::path source_db_path;
    sqlite3* source_db = nullptr;
    sqlite3* ui_db = nullptr;
    std::thread worker;
    mutable std::mutex mtx;
    std::uint64_t run_once_count = 0;
    std::uint64_t succeeded_run_once_count = 0;
    std::uint64_t failed_run_once_count = 0;
    std::uint64_t processed_event_count = 0;
    std::uint64_t dead_letter_count = 0;
    std::uint64_t last_run_duration_ms = 0;
    std::uint64_t max_run_duration_ms = 0;
    std::int64_t last_outbox_id = 0;
    std::int64_t source_high_water_outbox_id = 0;
    std::int64_t lag_count = 0;
    std::int64_t lag_age_ms = 0;
    std::int64_t dirty_count = 0;
    std::string last_error;
    bool running = false;
};

namespace {

std::optional<std::int64_t> ReadSubscriptionCursor(UiReadProjectionService::StreamRuntime& stream, int* failures_out, std::string* error_out) {
    Statement insert;
    constexpr const char* kInsert =
        "INSERT INTO ui_projection_subscription("
        "projector_name,source_context,source_outbox_table,last_outbox_id,last_event_id,updated_at_utc,status,last_error,stream_id) "
        "VALUES(?1,?2,?3,0,NULL,?4,'ACTIVE',NULL,?5) "
        "ON CONFLICT(projector_name,source_context,source_outbox_table) DO UPDATE SET stream_id=excluded.stream_id;";
    if (!Prepare(stream.ui_db, kInsert, &insert, error_out)) return std::nullopt;
    sqlite3_bind_text(insert.st, 1, stream.projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.st, 2, stream.source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.st, 3, stream.source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.st, 4, UtcNowMillis());
    sqlite3_bind_text(insert.st, 5, stream.stream_id.c_str(), -1, SQLITE_TRANSIENT);
    if (!StepDone(stream.ui_db, insert.st, error_out)) return std::nullopt;

    Statement select;
    constexpr const char* kSelect =
        "SELECT last_outbox_id,consecutive_failures FROM ui_projection_subscription "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";
    if (!Prepare(stream.ui_db, kSelect, &select, error_out)) return std::nullopt;
    sqlite3_bind_text(select.st, 1, stream.projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(select.st, 2, stream.source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(select.st, 3, stream.source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(select.st) != SQLITE_ROW) {
        if (error_out != nullptr) *error_out = "projection subscription row not found";
        return std::nullopt;
    }
    if (failures_out != nullptr) {
        *failures_out = sqlite3_column_int(select.st, 1);
    }
    return sqlite3_column_int64(select.st, 0);
}

bool UpdateIdleSubscription(
    UiReadProjectionService::StreamRuntime& stream,
    std::int64_t high_water,
    std::int64_t lag_count,
    std::int64_t lag_age_ms,
    std::uint64_t duration_ms,
    std::string* error_out) {
    Statement st;
    constexpr const char* kSql =
        "UPDATE ui_projection_subscription "
        "SET stream_id=?4,source_high_water_outbox_id=?5,lag_count=?6,lag_age_ms=?7,last_batch_size=0,last_run_duration_ms=?8,updated_at_utc=?9 "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";
    if (!Prepare(stream.ui_db, kSql, &st, error_out)) return false;
    sqlite3_bind_text(st.st, 1, stream.projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 2, stream.source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 3, stream.source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 4, stream.stream_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 5, high_water);
    sqlite3_bind_int64(st.st, 6, lag_count);
    sqlite3_bind_int64(st.st, 7, lag_age_ms);
    sqlite3_bind_int64(st.st, 8, static_cast<std::int64_t>(duration_ms));
    sqlite3_bind_int64(st.st, 9, UtcNowMillis());
    return StepDone(stream.ui_db, st.st, error_out);
}

bool AdvanceCursor(
    UiReadProjectionService::StreamRuntime& stream,
    const OutboxEvent& event,
    std::int64_t high_water,
    std::int64_t lag_count,
    std::int64_t lag_age_ms,
    int batch_size,
    int processed_count,
    std::int64_t from_outbox_id,
    std::uint64_t duration_ms,
    std::string* error_out) {
    Statement st;
    constexpr const char* kSql =
        "UPDATE ui_projection_subscription "
        "SET stream_id=?4,last_outbox_id=?5,last_event_id=?6,updated_at_utc=?7,status='ACTIVE',last_error=NULL,"
        "source_high_water_outbox_id=?8,lag_count=?9,lag_age_ms=?10,last_batch_size=?11,last_run_duration_ms=?12,consecutive_failures=0 "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";
    if (!Prepare(stream.ui_db, kSql, &st, error_out)) return false;
    sqlite3_bind_text(st.st, 1, stream.projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 2, stream.source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 3, stream.source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 4, stream.stream_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 5, event.outbox_id);
    sqlite3_bind_text(st.st, 6, event.event_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 7, UtcNowMillis());
    sqlite3_bind_int64(st.st, 8, high_water);
    sqlite3_bind_int64(st.st, 9, lag_count);
    sqlite3_bind_int64(st.st, 10, lag_age_ms);
    sqlite3_bind_int(st.st, 11, batch_size);
    sqlite3_bind_int64(st.st, 12, static_cast<std::int64_t>(duration_ms));
    if (!StepDone(stream.ui_db, st.st, error_out)) return false;

    Statement audit;
    constexpr const char* kAudit =
        "INSERT INTO ui_projection_subscription_audit(projector_name,source_context,source_outbox_table,from_outbox_id,to_outbox_id,processed_count,failed_count,recorded_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6,0,?7);";
    if (!Prepare(stream.ui_db, kAudit, &audit, error_out)) return false;
    sqlite3_bind_text(audit.st, 1, stream.projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(audit.st, 2, stream.source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(audit.st, 3, stream.source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(audit.st, 4, from_outbox_id);
    sqlite3_bind_int64(audit.st, 5, event.outbox_id);
    sqlite3_bind_int(audit.st, 6, processed_count);
    sqlite3_bind_int64(audit.st, 7, UtcNowMillis());
    return StepDone(stream.ui_db, audit.st, error_out);
}

bool RecordFailure(
    UiReadProjectionService::StreamRuntime& stream,
    const OutboxEvent& event,
    const std::string& failure,
    int max_attempts,
    std::string* error_out) {
    Statement read;
    int consecutive = 0;
    int dead_letters = 0;
    if (Prepare(stream.ui_db,
            "SELECT consecutive_failures,dead_letter_count FROM ui_projection_subscription WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;",
            &read,
            error_out)) {
        sqlite3_bind_text(read.st, 1, stream.projector_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(read.st, 2, stream.source_context.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(read.st, 3, stream.source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(read.st) == SQLITE_ROW) {
            consecutive = sqlite3_column_int(read.st, 0);
            dead_letters = sqlite3_column_int(read.st, 1);
        }
    }

    const int next_failures = consecutive + 1;
    const bool dead_letter = next_failures >= std::max(1, max_attempts);
    {
        Statement diagnostic;
        constexpr const char* kDiagnostic =
            "INSERT INTO ui_projection_dead_letter("
            "stream_id,projector_name,source_context,source_outbox_table,outbox_id,event_id,event_type,event_version,"
            "error_text,recorded_at_utc,payload_ref_kind,payload_ref_id,is_dead_letter,failure_count) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14) "
            "ON CONFLICT(stream_id,outbox_id) DO UPDATE SET "
            "event_id=excluded.event_id,event_type=excluded.event_type,event_version=excluded.event_version,"
            "error_text=excluded.error_text,recorded_at_utc=excluded.recorded_at_utc,"
            "payload_ref_kind=excluded.payload_ref_kind,payload_ref_id=excluded.payload_ref_id,"
            "is_dead_letter=excluded.is_dead_letter,failure_count=excluded.failure_count;";
        if (!Prepare(stream.ui_db, kDiagnostic, &diagnostic, error_out)) return false;
        sqlite3_bind_text(diagnostic.st, 1, stream.stream_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(diagnostic.st, 2, stream.projector_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(diagnostic.st, 3, stream.source_context.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(diagnostic.st, 4, stream.source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(diagnostic.st, 5, event.outbox_id);
        sqlite3_bind_text(diagnostic.st, 6, event.event_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(diagnostic.st, 7, event.event_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(diagnostic.st, 8, event.event_version);
        sqlite3_bind_text(diagnostic.st, 9, failure.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(diagnostic.st, 10, UtcNowMillis());
        sqlite3_bind_text(diagnostic.st, 11, event.payload_ref_kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(diagnostic.st, 12, event.payload_ref_id);
        sqlite3_bind_int(diagnostic.st, 13, dead_letter ? 1 : 0);
        sqlite3_bind_int(diagnostic.st, 14, next_failures);
        if (!StepDone(stream.ui_db, diagnostic.st, error_out)) return false;
    }
    if (dead_letter) {
        dead_letters += 1;
        stream.dead_letter_count += 1;
    }

    Statement update;
    constexpr const char* kUpdate =
        "UPDATE ui_projection_subscription "
        "SET stream_id=?4,status='ERROR',last_error=?5,updated_at_utc=?6,consecutive_failures=?7,dead_letter_count=?8 "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";
    if (!Prepare(stream.ui_db, kUpdate, &update, error_out)) return false;
    sqlite3_bind_text(update.st, 1, stream.projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.st, 2, stream.source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.st, 3, stream.source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.st, 4, stream.stream_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.st, 5, failure.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update.st, 6, UtcNowMillis());
    sqlite3_bind_int(update.st, 7, next_failures);
    sqlite3_bind_int(update.st, 8, dead_letters);
    return StepDone(stream.ui_db, update.st, error_out);
}

bool UpsertDirtyEntities(
    UiReadProjectionService::StreamRuntime& stream,
    const std::vector<DirtyEntity>& dirty,
    std::string* error_out) {
    if (dirty.empty()) {
        return true;
    }

    Statement st;
    constexpr const char* kSql =
        "INSERT INTO ui_projection_dirty_entity("
        "stream_id,source_context,source_outbox_table,entity_kind,entity_id,first_outbox_id,last_outbox_id,event_count,updated_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6,?6,1,?7) "
        "ON CONFLICT(stream_id,entity_kind,entity_id) DO UPDATE SET "
        "first_outbox_id=MIN(first_outbox_id,excluded.first_outbox_id),"
        "last_outbox_id=MAX(last_outbox_id,excluded.last_outbox_id),"
        "event_count=event_count+1,updated_at_utc=excluded.updated_at_utc;";
    if (!Prepare(stream.ui_db, kSql, &st, error_out)) {
        return false;
    }

    const auto now = UtcNowMillis();
    for (const auto& entity : dirty) {
        sqlite3_reset(st.st);
        sqlite3_clear_bindings(st.st);
        sqlite3_bind_text(st.st, 1, stream.stream_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 2, stream.source_context.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 3, stream.source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 4, entity.kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st.st, 5, entity.id);
        sqlite3_bind_int64(st.st, 6, entity.outbox_id);
        sqlite3_bind_int64(st.st, 7, now);
        if (!StepDone(stream.ui_db, st.st, error_out)) {
            return false;
        }
    }
    return true;
}

std::int64_t DirtyCount(UiReadProjectionService::StreamRuntime& stream, std::string* error_out) {
    Statement st;
    if (!Prepare(
            stream.ui_db,
            "SELECT COUNT(1) FROM ui_projection_dirty_entity WHERE stream_id=?1;",
            &st,
            error_out)) {
        return 0;
    }
    sqlite3_bind_text(st.st, 1, stream.stream_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) == SQLITE_ROW) {
        return sqlite3_column_int64(st.st, 0);
    }
    return 0;
}

std::vector<DirtyEntity> ReadDirtyEntities(
    UiReadProjectionService::StreamRuntime& stream,
    int limit,
    std::string* error_out) {
    std::vector<DirtyEntity> rows;
    Statement st;
    constexpr const char* kSql =
        "SELECT entity_kind,entity_id,last_outbox_id "
        "FROM ui_projection_dirty_entity WHERE stream_id=?1 "
        "ORDER BY updated_at_utc ASC,last_outbox_id ASC LIMIT ?2;";
    if (!Prepare(stream.ui_db, kSql, &st, error_out)) {
        return rows;
    }
    sqlite3_bind_text(st.st, 1, stream.stream_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st.st, 2, limit);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        rows.push_back(DirtyEntity{
            .kind = Text(st.st, 0),
            .id = sqlite3_column_int64(st.st, 1),
            .outbox_id = sqlite3_column_int64(st.st, 2),
        });
    }
    return rows;
}

void AppendDirtyEntitiesForKind(
    UiReadProjectionService::StreamRuntime& stream,
    const std::string& kind,
    int limit,
    std::vector<DirtyEntity>* rows,
    std::string* error_out) {
    if (rows == nullptr || limit <= 0) {
        return;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT entity_kind,entity_id,last_outbox_id "
        "FROM ui_projection_dirty_entity WHERE stream_id=?1 AND entity_kind=?2 "
        "ORDER BY updated_at_utc ASC,last_outbox_id ASC LIMIT ?3;";
    if (!Prepare(stream.ui_db, kSql, &st, error_out)) {
        return;
    }
    sqlite3_bind_text(st.st, 1, stream.stream_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st.st, 3, limit);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        rows->push_back(DirtyEntity{
            .kind = Text(st.st, 0),
            .id = sqlite3_column_int64(st.st, 1),
            .outbox_id = sqlite3_column_int64(st.st, 2),
        });
    }
}

void AppendUnknownExecutionDirtyEntities(
    UiReadProjectionService::StreamRuntime& stream,
    int limit,
    std::vector<DirtyEntity>* rows,
    std::string* error_out) {
    if (rows == nullptr || limit <= 0) {
        return;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT entity_kind,entity_id,last_outbox_id "
        "FROM ui_projection_dirty_entity WHERE stream_id=?1 "
        "AND entity_kind NOT IN ('workflow','job_set','job') "
        "ORDER BY updated_at_utc ASC,last_outbox_id ASC LIMIT ?2;";
    if (!Prepare(stream.ui_db, kSql, &st, error_out)) {
        return;
    }
    sqlite3_bind_text(st.st, 1, stream.stream_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st.st, 2, limit);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        rows->push_back(DirtyEntity{
            .kind = Text(st.st, 0),
            .id = sqlite3_column_int64(st.st, 1),
            .outbox_id = sqlite3_column_int64(st.st, 2),
        });
    }
}

std::vector<DirtyEntity> ReadExecutionDirtyEntities(
    UiReadProjectionService::StreamRuntime& stream,
    int limit,
    std::string* error_out) {
    std::vector<DirtyEntity> rows;
    rows.reserve(static_cast<std::size_t>(std::max(limit, 0)));
    constexpr std::array<const char*, 3> kExecutionDirtyPriority{ "workflow", "job_set", "job" };
    for (const char* kind : kExecutionDirtyPriority) {
        AppendDirtyEntitiesForKind(stream, kind, limit - static_cast<int>(rows.size()), &rows, error_out);
        if (static_cast<int>(rows.size()) >= limit) {
            return rows;
        }
    }
    AppendUnknownExecutionDirtyEntities(stream, limit - static_cast<int>(rows.size()), &rows, error_out);
    return rows;
}

bool ClearDirtyEntity(
    UiReadProjectionService::StreamRuntime& stream,
    const DirtyEntity& entity,
    std::string* error_out) {
    Statement st;
    constexpr const char* kSql =
        "DELETE FROM ui_projection_dirty_entity "
        "WHERE stream_id=?1 AND entity_kind=?2 AND entity_id=?3 AND last_outbox_id<=?4;";
    if (!Prepare(stream.ui_db, kSql, &st, error_out)) {
        return false;
    }
    sqlite3_bind_text(st.st, 1, stream.stream_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 2, entity.kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 3, entity.id);
    sqlite3_bind_int64(st.st, 4, entity.outbox_id);
    return StepDone(stream.ui_db, st.st, error_out);
}

bool MaterializeDirtyEntity(
    UiReadProjectionService::StreamRuntime& stream,
    const DirtyEntity& entity,
    const std::function<bool()>& stop_requested,
    std::string* error_out) {
    if (stream.kind == StreamKind::Execution) {
        if (entity.kind == "job") return ProjectJob(stream.source_db, stream.ui_db, entity.id, stop_requested, error_out);
        if (entity.kind == "job_set") return ProjectJobSet(stream.source_db, stream.ui_db, entity.id, stop_requested, error_out);
        if (entity.kind == "workflow") return ProjectWorkflowInstance(stream.source_db, stream.ui_db, entity.id, error_out);
    }
    if (stream.kind == StreamKind::State && entity.kind == "artifact") {
        return ProjectArtifact(stream.source_db, stream.ui_db, entity.id, error_out);
    }
    if (stream.kind == StreamKind::AnalysisSeedProbe && entity.kind == "seed_probe_run") {
        return ProjectSeedProbeRun(stream.source_db, stream.ui_db, entity.id, error_out);
    }
    if (stream.kind == StreamKind::AnalysisBattle) {
        if (entity.kind == "battle_group") return ProjectBattleSet(stream.source_db, stream.ui_db, entity.id, error_out);
        if (entity.kind == "battle_wave") return ProjectBattleWave(stream.source_db, stream.ui_db, entity.id, error_out);
        if (entity.kind == "battle_turn_job") return ProjectBattleTurnJob(stream.source_db, stream.ui_db, entity.id, error_out);
        if (entity.kind == "battle_advancement_decision") return ProjectBattleAdvancementDecision(stream.source_db, stream.ui_db, entity.id, error_out);
        if (entity.kind == "battle_manual_followup") return ProjectBattleManualFollowupForTurnJob(stream.source_db, stream.ui_db, entity.id, error_out);
    }
    if (stream.kind == StreamKind::Archive) {
        OutboxEvent event{};
        if (entity.kind == "archive_package") {
            event.aggregate_kind = "archive_package";
            event.aggregate_id = std::to_string(entity.id);
            return ProjectArchive(stream.source_db, stream.ui_db, event, error_out);
        }
        if (entity.kind == "rehydrate_request") {
            event.payload_ref_kind = "rehydrate_request";
            event.payload_ref_id = entity.id;
            return ProjectArchive(stream.source_db, stream.ui_db, event, error_out);
        }
    }
    return true;
}

bool MaterializeDirtyEntities(
    UiReadProjectionService::StreamRuntime& stream,
    int limit,
    const std::function<bool()>& stop_requested,
    int* materialized_count_out,
    DirtyEntity* failed_entity_out,
    bool* stopped_out,
    std::string* error_out) {
    if (materialized_count_out != nullptr) {
        *materialized_count_out = 0;
    }
    if (failed_entity_out != nullptr) {
        *failed_entity_out = {};
    }
    if (stopped_out != nullptr) {
        *stopped_out = false;
    }
    if (stop_requested()) {
        if (stopped_out != nullptr) {
            *stopped_out = true;
        }
        return true;
    }
    const auto dirty = stream.kind == StreamKind::Execution
        ? ReadExecutionDirtyEntities(stream, limit, error_out)
        : ReadDirtyEntities(stream, limit, error_out);
    if (dirty.empty()) {
        return true;
    }
    if (stop_requested()) {
        if (stopped_out != nullptr) {
            *stopped_out = true;
        }
        return true;
    }
    if (!Exec(stream.ui_db, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }
    for (const auto& entity : dirty) {
        if (stop_requested()) {
            if (stopped_out != nullptr) {
                *stopped_out = true;
            }
            Exec(stream.ui_db, "ROLLBACK;", nullptr);
            return true;
        }
        if (!MaterializeDirtyEntity(stream, entity, stop_requested, error_out)) {
            if (stop_requested()) {
                if (stopped_out != nullptr) {
                    *stopped_out = true;
                }
                Exec(stream.ui_db, "ROLLBACK;", nullptr);
                return true;
            }
            if (failed_entity_out != nullptr) {
                *failed_entity_out = entity;
            }
            Exec(stream.ui_db, "ROLLBACK;", nullptr);
            return false;
        }
        if (stop_requested()) {
            if (stopped_out != nullptr) {
                *stopped_out = true;
            }
            Exec(stream.ui_db, "ROLLBACK;", nullptr);
            return true;
        }
        if (!ClearDirtyEntity(stream, entity, error_out)) {
            if (failed_entity_out != nullptr) {
                *failed_entity_out = entity;
            }
            Exec(stream.ui_db, "ROLLBACK;", nullptr);
            return false;
        }
        if (materialized_count_out != nullptr) {
            *materialized_count_out += 1;
        }
    }
    if (stop_requested()) {
        if (stopped_out != nullptr) {
            *stopped_out = true;
        }
        Exec(stream.ui_db, "ROLLBACK;", nullptr);
        return true;
    }
    if (!Exec(stream.ui_db, "COMMIT;", error_out)) {
        Exec(stream.ui_db, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

OutboxEvent DirtyMaterializationDiagnosticEvent(
    const UiReadProjectionService::StreamRuntime& stream,
    const DirtyEntity& entity) {
    OutboxEvent event{};
    event.outbox_id = entity.outbox_id > 0 ? entity.outbox_id : stream.last_outbox_id;
    event.event_id = "dirty-materialization:" + stream.stream_id + ":" + entity.kind + ":" + std::to_string(entity.id);
    event.event_type = "UiReadProjection.DirtyMaterialization.v1";
    event.event_version = 1;
    event.context_name = stream.source_context;
    event.aggregate_kind = entity.kind;
    event.aggregate_id = std::to_string(entity.id);
    event.payload_ref_kind = entity.kind;
    event.payload_ref_id = entity.id;
    if (stream.kind == StreamKind::State && entity.kind == "artifact") {
        event.event_type = "State.ArtifactStored.v1";
    }
    return event;
}

} // namespace

UiReadProjectionService::UiReadProjectionService(UiReadProjectionConfig config)
    : config_(std::move(config)) {
    auto stream_enabled = [this](const std::string& stream_id) {
        return config_.enabled_stream_ids.empty()
            || std::find(config_.enabled_stream_ids.begin(), config_.enabled_stream_ids.end(), stream_id) != config_.enabled_stream_ids.end();
    };
    auto add_stream = [this](StreamKind kind, std::string stream_id, std::string context, std::string table, std::filesystem::path path) {
        auto stream = std::make_unique<StreamRuntime>();
        stream->kind = kind;
        stream->stream_id = std::move(stream_id);
        stream->source_context = std::move(context);
        stream->source_outbox_table = std::move(table);
        stream->source_db_path = std::move(path);
        streams_.push_back(std::move(stream));
    };

    if (stream_enabled("execution")) add_stream(StreamKind::Execution, "execution", "Execution", "exec_outbox_message", config_.execution_db_path);
    if (stream_enabled("state")) add_stream(StreamKind::State, "state", "State", "state_outbox_message", config_.state_db_path);
    if (stream_enabled("analysis-seedprobe")) add_stream(StreamKind::AnalysisSeedProbe, "analysis-seedprobe", "AnalysisSeedProbe", "sp_outbox_message", config_.analysis_db_path);
    if (stream_enabled("analysis-battle")) add_stream(StreamKind::AnalysisBattle, "analysis-battle", "AnalysisBattle", "ab_outbox_message", config_.analysis_db_path);
    if (stream_enabled("archive")) add_stream(StreamKind::Archive, "archive", "Archive", "ar_outbox_message", config_.archive_db_path);
}

UiReadProjectionService::~UiReadProjectionService() {
    Stop();
}

bool UiReadProjectionService::Start(std::string* error_out) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (running_) return true;
    }
    if (config_.poll_interval <= std::chrono::milliseconds::zero()) {
        if (error_out) *error_out = "UIRead projection poll interval must be positive";
        return false;
    }
    if (config_.max_batch_size <= 0 || config_.max_dirty_materialization_batch_size <= 0 || config_.max_attempts <= 0) {
        if (error_out) *error_out = "UIRead projection batch size, dirty materialization batch size, and attempts must be positive";
        return false;
    }
    for (auto& stream : streams_) {
        if (!OpenStream(*stream, error_out)) {
            for (auto& opened_stream : streams_) {
                CloseStream(*opened_stream);
            }
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stopping_ = false;
        running_ = true;
    }
    for (auto& stream : streams_) {
        stream->running = true;
        stream->worker = std::thread([this, stream = stream.get()]() { WorkerLoop(stream); });
    }
    return true;
}

void UiReadProjectionService::Stop() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stopping_ = true;
    }
    InterruptStreams();
    cv_.notify_all();
    for (auto& stream : streams_) {
        if (stream->worker.joinable()) {
            stream->worker.join();
        }
        stream->running = false;
        CloseStream(*stream);
    }
    std::lock_guard<std::mutex> lock(mtx_);
    running_ = false;
}

bool UiReadProjectionService::IsRunning() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return running_ && !stopping_;
}

bool UiReadProjectionService::IsStoppingRequested() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return stopping_;
}

void UiReadProjectionService::InterruptStreams() const {
    for (const auto& stream : streams_) {
        if (stream == nullptr) {
            continue;
        }
        if (stream->source_db != nullptr) {
            sqlite3_interrupt(stream->source_db);
        }
        if (stream->ui_db != nullptr) {
            sqlite3_interrupt(stream->ui_db);
        }
    }
}

bool UiReadProjectionService::OpenStream(StreamRuntime& stream, std::string* error_out) {
    if (!OpenDb(stream.source_db_path, true, &stream.source_db, error_out)) {
        if (error_out) *error_out = "failed opening projection source " + stream.stream_id + ": " + *error_out;
        return false;
    }
    if (stream.kind == StreamKind::AnalysisSeedProbe
        && !AttachExecutionSource(
            stream.source_db,
            config_.execution_db_path,
            error_out)) {
        if (error_out) {
            *error_out =
                "failed attaching Execution source for "
                + stream.stream_id + ": " + *error_out;
        }
        return false;
    }
    if (!OpenDb(config_.ui_read_db_path, false, &stream.ui_db, error_out)) {
        if (error_out) *error_out = "failed opening projection UI DB for " + stream.stream_id + ": " + *error_out;
        return false;
    }
    int ignored_failures = 0;
    auto cursor = ReadSubscriptionCursor(stream, &ignored_failures, error_out);
    if (!cursor.has_value()) {
        return false;
    }
    stream.last_outbox_id = *cursor;
    return true;
}

void UiReadProjectionService::CloseStream(StreamRuntime& stream) {
    if (stream.source_db != nullptr) {
        sqlite3_close(stream.source_db);
        stream.source_db = nullptr;
    }
    if (stream.ui_db != nullptr) {
        sqlite3_close(stream.ui_db);
        stream.ui_db = nullptr;
    }
}

bool UiReadProjectionService::RunOnce(std::string* error_out) {
    if (config_.max_batch_size <= 0 || config_.max_dirty_materialization_batch_size <= 0 || config_.max_attempts <= 0) {
        if (error_out) *error_out = "UIRead projection batch size, dirty materialization batch size, and attempts must be positive";
        return false;
    }

    bool all_ok = true;
    std::string combined_error;
    for (auto& stream : streams_) {
        std::string stream_error;
        if (stream->source_db == nullptr || stream->ui_db == nullptr) {
            CloseStream(*stream);
            if (!OpenStream(*stream, &stream_error)) {
                all_ok = false;
                if (combined_error.empty()) {
                    combined_error = stream->stream_id + ": " + stream_error;
                }
                continue;
            }
        }
        if (!RunStreamOnce(*stream, &stream_error)) {
            all_ok = false;
            if (combined_error.empty()) {
                combined_error = stream->stream_id + ": " + stream_error;
            }
        }
    }
    if (!all_ok && error_out != nullptr) {
        *error_out = combined_error.empty() ? "one or more UIRead projection streams failed" : combined_error;
    }
    return all_ok;
}

bool UiReadProjectionService::RunStreamOnce(StreamRuntime& stream, std::string* error_out) {
    std::lock_guard<std::mutex> lock(stream.mtx);
    if (stream.source_db == nullptr || stream.ui_db == nullptr) {
        if (error_out) *error_out = "projection stream database is not open";
        return false;
    }
    if (IsStoppingRequested()) {
        return true;
    }

    const auto started_at = std::chrono::steady_clock::now();
    int consecutive_failures = 0;
    auto cursor = ReadSubscriptionCursor(stream, &consecutive_failures, error_out);
    if (!cursor.has_value()) {
        if (IsStoppingRequested()) {
            return true;
        }
        stream.failed_run_once_count += 1;
        return false;
    }
    if (IsStoppingRequested()) {
        return true;
    }

    const auto high_water = SourceHighWater(stream.source_db, stream.source_outbox_table, error_out);
    if (IsStoppingRequested()) {
        return true;
    }
    const auto batch = ReadOutboxBatch(stream.source_db, stream.source_outbox_table, *cursor, config_.max_batch_size, error_out);
    if (IsStoppingRequested()) {
        return true;
    }
    bool ok = true;
    bool stopped = false;
    std::string failure;
    int processed_count = 0;

    if (!batch.empty()) {
        std::vector<DirtyEntity> dirty;
        dirty.reserve(batch.size());
        const OutboxEvent* failure_event = nullptr;
        std::string handler_error;
        for (const auto& event : batch) {
            if (IsStoppingRequested()) {
                stopped = true;
                break;
            }
            if (!ClassifyOutboxEvent(stream.kind, stream.source_db, event, &dirty, &handler_error)) {
                failure_event = &event;
                ok = false;
                failure = handler_error.empty() ? "projection event classification failed" : handler_error;
                break;
            }
            processed_count += 1;
        }

        if (!stopped && ok) {
            const auto& last_event = batch[static_cast<std::size_t>(processed_count - 1)];
            const auto duration_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count());
            const auto batch_lag_count = LagCount(stream.source_db, stream.source_outbox_table, last_event.outbox_id, error_out);
            const auto batch_lag_age = LagAgeMs(stream.source_db, stream.source_outbox_table, last_event.outbox_id, error_out);
            if (!Exec(stream.ui_db, "BEGIN IMMEDIATE;", error_out)) {
                ok = false;
                failure = error_out != nullptr ? *error_out : "failed to begin UI projection ingest transaction";
                failure_event = &last_event;
            } else if (!UpsertDirtyEntities(stream, dirty, &handler_error)
                || !AdvanceCursor(
                    stream,
                    last_event,
                    high_water,
                    batch_lag_count,
                    batch_lag_age,
                    static_cast<int>(batch.size()),
                    processed_count,
                    *cursor,
                    duration_ms,
                    &handler_error)) {
                Exec(stream.ui_db, "ROLLBACK;", nullptr);
                ok = false;
                failure = handler_error.empty() ? "failed to ingest projection dirty entities" : handler_error;
                failure_event = &last_event;
            } else if (!Exec(stream.ui_db, "COMMIT;", error_out)) {
                Exec(stream.ui_db, "ROLLBACK;", nullptr);
                ok = false;
                failure = error_out != nullptr ? *error_out : "failed to commit UI projection ingest transaction";
                failure_event = &last_event;
            } else {
                stream.processed_event_count += static_cast<std::uint64_t>(processed_count);
                stream.last_outbox_id = last_event.outbox_id;
            }
        }

        if (!stopped && !ok && failure_event != nullptr) {
            Exec(stream.ui_db, "BEGIN IMMEDIATE;", nullptr);
            RecordFailure(stream, *failure_event, failure.empty() ? "projection handler failed" : failure, config_.max_attempts, nullptr);
            Exec(stream.ui_db, "COMMIT;", nullptr);
        }
    }

    int materialized_count = 0;
    DirtyEntity failed_entity;
    bool materialization_stopped = false;
    if (ok && !stopped && !IsStoppingRequested()) {
        std::string materialize_error;
        if (!MaterializeDirtyEntities(
                stream,
                config_.max_dirty_materialization_batch_size,
                [this]() { return IsStoppingRequested(); },
                &materialized_count,
                &failed_entity,
                &materialization_stopped,
                &materialize_error)) {
            ok = false;
            failure = materialize_error.empty() ? "failed to materialize dirty projection entities" : materialize_error;
            Exec(stream.ui_db, "BEGIN IMMEDIATE;", nullptr);
            const auto diagnostic = DirtyMaterializationDiagnosticEvent(stream, failed_entity);
            RecordFailure(stream, diagnostic, failure, config_.max_attempts, nullptr);
            Exec(stream.ui_db, "COMMIT;", nullptr);
        } else if (materialization_stopped) {
            stopped = true;
        }
    }

    const auto duration_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count());
    std::int64_t lag_count = stream.lag_count;
    std::int64_t lag_age = stream.lag_age_ms;
    if (ok && !stopped && !IsStoppingRequested() && batch.empty()) {
        lag_count = LagCount(stream.source_db, stream.source_outbox_table, stream.last_outbox_id, nullptr);
        if (!IsStoppingRequested()) {
            lag_age = LagAgeMs(stream.source_db, stream.source_outbox_table, stream.last_outbox_id, nullptr);
            if (!IsStoppingRequested()) {
                UpdateIdleSubscription(stream, high_water, lag_count, lag_age, duration_ms, nullptr);
            }
        }
    } else if (ok && !stopped) {
        lag_count = stream.last_outbox_id > 0
            ? LagCount(stream.source_db, stream.source_outbox_table, stream.last_outbox_id, nullptr)
            : stream.lag_count;
        lag_age = stream.last_outbox_id > 0
            ? LagAgeMs(stream.source_db, stream.source_outbox_table, stream.last_outbox_id, nullptr)
            : stream.lag_age_ms;
    } else if (IsStoppingRequested()) {
        stopped = true;
    }

    stream.run_once_count += 1;
    stream.last_run_duration_ms = duration_ms;
    stream.max_run_duration_ms = std::max(stream.max_run_duration_ms, duration_ms);
    stream.source_high_water_outbox_id = high_water;
    stream.lag_count = lag_count;
    stream.lag_age_ms = lag_age;
    stream.dirty_count = DirtyCount(stream, nullptr);
    if (stopped) {
        stream.succeeded_run_once_count += 1;
        stream.last_error.clear();
        if (error_out != nullptr) {
            error_out->clear();
        }
    } else if (ok) {
        stream.succeeded_run_once_count += 1;
        stream.last_error.clear();
    } else {
        stream.failed_run_once_count += 1;
        stream.last_error = failure;
        if (error_out != nullptr) {
            *error_out = failure;
        }
    }
    return stopped || ok;
}

void UiReadProjectionService::WorkerLoop(StreamRuntime* stream) {
    if (stream == nullptr) return;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mtx_);
            if (stopping_) break;
        }
        std::string ignored_error;
        const bool ok = RunStreamOnce(*stream, &ignored_error);
        bool has_backlog = false;
        if (ok) {
            std::lock_guard<std::mutex> stream_lock(stream->mtx);
            has_backlog = (stream->lag_count > 0 && stream->last_outbox_id < stream->source_high_water_outbox_id)
                || stream->dirty_count > 0;
        }
        if (has_backlog) {
            continue;
        }
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait_for(lock, config_.poll_interval, [this]() { return stopping_; });
        if (stopping_) break;
    }
}

void UiReadProjectionService::Wake() {
    cv_.notify_all();
}

UiReadProjectionTelemetrySnapshot UiReadProjectionService::SnapshotTelemetry() const {
    UiReadProjectionTelemetrySnapshot snapshot{};
    {
        std::lock_guard<std::mutex> lock(mtx_);
        snapshot.running = running_ && !stopping_;
    }
    snapshot.configured_max_batch_size = config_.max_batch_size;
    snapshot.configured_max_dirty_materialization_batch_size = config_.max_dirty_materialization_batch_size;
    snapshot.configured_max_attempts = config_.max_attempts;
    for (const auto& stream : streams_) {
        std::lock_guard<std::mutex> stream_lock(stream->mtx);
        UiReadProjectionStreamTelemetrySnapshot row{};
        row.stream_id = stream->stream_id;
        row.source_context = stream->source_context;
        row.source_outbox_table = stream->source_outbox_table;
        row.running = stream->running;
        row.run_once_count = stream->run_once_count;
        row.succeeded_run_once_count = stream->succeeded_run_once_count;
        row.failed_run_once_count = stream->failed_run_once_count;
        row.processed_event_count = stream->processed_event_count;
        row.dead_letter_count = stream->dead_letter_count;
        row.last_run_duration_ms = stream->last_run_duration_ms;
        row.max_run_duration_ms = stream->max_run_duration_ms;
        row.last_outbox_id = stream->last_outbox_id;
        row.source_high_water_outbox_id = stream->source_high_water_outbox_id;
        row.lag_count = stream->lag_count;
        row.lag_age_ms = stream->lag_age_ms;
        row.dirty_count = stream->dirty_count;
        row.last_error = stream->last_error;
        snapshot.run_once_count += row.run_once_count;
        snapshot.succeeded_run_once_count += row.succeeded_run_once_count;
        snapshot.failed_run_once_count += row.failed_run_once_count;
        snapshot.last_run_duration_ms = std::max(snapshot.last_run_duration_ms, row.last_run_duration_ms);
        snapshot.max_run_duration_ms = std::max(snapshot.max_run_duration_ms, row.max_run_duration_ms);
        snapshot.streams.push_back(std::move(row));
    }
    return snapshot;
}

} // namespace savor::db::uiread::projectors
