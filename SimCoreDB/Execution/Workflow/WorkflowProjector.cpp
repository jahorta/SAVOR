#include "WorkflowProjector.h"

#include <cstdlib>
#include <ctime>
#include <vector>

#include "../../Common/Events/OutboxRelay.h"
#include "../../UIRead/SqliteUiReadDb.h"

namespace simcore::db::execution::workflow {

namespace {

bool Exec(sqlite3* db, const char* sql, std::string* error_out) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (error_out) *error_out = err ? err : "sqlite3_exec failed";
        sqlite3_free(err);
        return false;
    }
    return true;
}

} // namespace

WorkflowProjector::WorkflowProjector(sqlite3* db)
    : db_(db) {
}

std::int64_t WorkflowProjector::GetCheckpoint(const std::string& projector_name, std::string* error_out) const {
    if (projector_name.empty()) {
        if (error_out) *error_out = "projector_name is required";
        return 0;
    }

    simcore::db::SqliteUiReadDb ui_read_db(db_);
    const auto checkpoint = ui_read_db.GetProjectionCheckpoint(projector_name);
    if (!checkpoint.has_value()) {
        return 0;
    }

    return checkpoint->last_outbox_id;
}

bool WorkflowProjector::ProjectInstance(std::int64_t workflow_instance_id, std::string* error_out) {
    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    sqlite3_stmt* inst = nullptr;
    constexpr const char* kInst =
        "INSERT INTO ui_workflow_instance("
        "workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,created_by,blocked_step_count,failed_step_count,created_at_utc,started_at_utc,completed_at_utc,failure_code,failure_text) "
        "SELECT i.workflow_instance_id,i.workflow_kind,i.state,i.root_scope_kind,i.root_scope_id,i.created_by,"
        "(SELECT COUNT(1) FROM exec_workflow_step s WHERE s.workflow_instance_id=i.workflow_instance_id AND s.blocked_reason IS NOT NULL),"
        "(SELECT COUNT(1) FROM exec_workflow_step s WHERE s.workflow_instance_id=i.workflow_instance_id AND s.state='FAILED'),"
        "i.created_at_utc,i.started_at_utc,i.completed_at_utc,i.failure_code,i.failure_text "
        "FROM exec_workflow_instance i WHERE i.workflow_instance_id=?1 "
        "ON CONFLICT(workflow_instance_id) DO UPDATE SET "
        "state=excluded.state,blocked_step_count=excluded.blocked_step_count,failed_step_count=excluded.failed_step_count,"
        "started_at_utc=excluded.started_at_utc,completed_at_utc=excluded.completed_at_utc,failure_code=excluded.failure_code,failure_text=excluded.failure_text;";
    if (sqlite3_prepare_v2(db_, kInst, -1, &inst, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(inst, 1, workflow_instance_id);
    if (sqlite3_step(inst) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        sqlite3_finalize(inst);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_finalize(inst);

    sqlite3_stmt* steps = nullptr;
    constexpr const char* kSteps =
        "INSERT INTO ui_workflow_step("
        "workflow_step_id,workflow_instance_id,step_key,step_kind,state,blocked_reason,job_set_id,job_count,job_completed_count,job_failed_count,priority,attempts,max_attempts,ready_at_utc,started_at_utc,completed_at_utc,failed_at_utc,created_at_utc) "
        "SELECT s.workflow_step_id,s.workflow_instance_id,s.step_key,s.step_kind,s.state,s.blocked_reason,s.job_set_id,"
        "(SELECT COUNT(1) FROM exec_job j WHERE j.job_set_id=s.job_set_id),"
        "(SELECT COUNT(1) FROM exec_job j WHERE j.job_set_id=s.job_set_id AND j.state='COMPLETED'),"
        "(SELECT COUNT(1) FROM exec_job j WHERE j.job_set_id=s.job_set_id AND j.state='FAILED'),"
        "s.priority,s.attempts,s.max_attempts,s.ready_at_utc,s.started_at_utc,s.completed_at_utc,s.failed_at_utc,s.created_at_utc "
        "FROM exec_workflow_step s WHERE s.workflow_instance_id=?1 "
        "ON CONFLICT(workflow_step_id) DO UPDATE SET "
        "state=excluded.state,blocked_reason=excluded.blocked_reason,job_set_id=excluded.job_set_id,"
        "job_count=excluded.job_count,job_completed_count=excluded.job_completed_count,job_failed_count=excluded.job_failed_count,"
        "attempts=excluded.attempts,max_attempts=excluded.max_attempts,ready_at_utc=excluded.ready_at_utc,"
        "started_at_utc=excluded.started_at_utc,completed_at_utc=excluded.completed_at_utc,failed_at_utc=excluded.failed_at_utc;";
    if (sqlite3_prepare_v2(db_, kSteps, -1, &steps, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(steps, 1, workflow_instance_id);
    if (sqlite3_step(steps) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        sqlite3_finalize(steps);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_finalize(steps);

    sqlite3_stmt* edges = nullptr;
    constexpr const char* kEdges =
        "INSERT INTO ui_workflow_edge(workflow_edge_id,workflow_instance_id,from_step_id,to_step_id,condition_kind,condition_value,created_at_utc) "
        "SELECT workflow_edge_id,workflow_instance_id,from_step_id,to_step_id,condition_kind,condition_value,created_at_utc "
        "FROM exec_workflow_edge WHERE workflow_instance_id=?1 "
        "ON CONFLICT(workflow_edge_id) DO UPDATE SET condition_kind=excluded.condition_kind, condition_value=excluded.condition_value;";
    if (sqlite3_prepare_v2(db_, kEdges, -1, &edges, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(edges, 1, workflow_instance_id);
    if (sqlite3_step(edges) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        sqlite3_finalize(edges);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_finalize(edges);

    const auto now_ts = static_cast<std::int64_t>(std::time(nullptr));

    sqlite3_stmt* clear_alerts = nullptr;
    constexpr const char* kClearAlerts =
        "UPDATE ui_workflow_alert "
        "SET is_active=0, cleared_at_utc=?2 "
        "WHERE workflow_instance_id=?1 AND is_active=1 AND workflow_alert_id NOT IN ("
        "    SELECT (workflow_step_id * 10) + 1 AS workflow_alert_id "
        "    FROM exec_workflow_step "
        "    WHERE workflow_instance_id=?1 AND blocked_reason IS NOT NULL "
        "    UNION ALL "
        "    SELECT (workflow_step_id * 10) + 2 AS workflow_alert_id "
        "    FROM exec_workflow_step "
        "    WHERE workflow_instance_id=?1 AND state='FAILED'"
        ");";
    if (sqlite3_prepare_v2(db_, kClearAlerts, -1, &clear_alerts, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(clear_alerts, 1, workflow_instance_id);
    sqlite3_bind_int64(clear_alerts, 2, now_ts);
    if (sqlite3_step(clear_alerts) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        sqlite3_finalize(clear_alerts);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_finalize(clear_alerts);

    sqlite3_stmt* upsert_alerts = nullptr;
    constexpr const char* kUpsertAlerts =
        "INSERT INTO ui_workflow_alert("
        "workflow_alert_id,workflow_instance_id,workflow_step_id,alert_kind,alert_code,message,is_active,first_seen_at_utc,last_seen_at_utc,cleared_at_utc) "
        "SELECT "
        "  src.workflow_alert_id,src.workflow_instance_id,src.workflow_step_id,src.alert_kind,src.alert_code,src.message,1,?2,?2,NULL "
        "FROM ("
        "  SELECT "
        "    (workflow_step_id * 10) + 1 AS workflow_alert_id,"
        "    workflow_instance_id,"
        "    workflow_step_id,"
        "    'BLOCKED_STEP' AS alert_kind,"
        "    'STEP_BLOCKED' AS alert_code,"
        "    blocked_reason AS message "
        "  FROM exec_workflow_step "
        "  WHERE workflow_instance_id=?1 AND blocked_reason IS NOT NULL "
        "  UNION ALL "
        "  SELECT "
        "    (workflow_step_id * 10) + 2 AS workflow_alert_id,"
        "    workflow_instance_id,"
        "    workflow_step_id,"
        "    'FAILED_STEP' AS alert_kind,"
        "    'STEP_FAILED' AS alert_code,"
        "    COALESCE(blocked_reason, 'step failed') AS message "
        "  FROM exec_workflow_step "
        "  WHERE workflow_instance_id=?1 AND state='FAILED'"
        ") AS src "
        "WHERE 1=1 "
        "ON CONFLICT(workflow_alert_id) DO UPDATE SET "
        "  alert_kind=excluded.alert_kind,"
        "  alert_code=excluded.alert_code,"
        "  message=excluded.message,"
        "  is_active=1,"
        "  last_seen_at_utc=excluded.last_seen_at_utc,"
        "  cleared_at_utc=NULL;";
    if (sqlite3_prepare_v2(db_, kUpsertAlerts, -1, &upsert_alerts, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(upsert_alerts, 1, workflow_instance_id);
    sqlite3_bind_int64(upsert_alerts, 2, now_ts);
    if (sqlite3_step(upsert_alerts) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        sqlite3_finalize(upsert_alerts);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_finalize(upsert_alerts);

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

bool WorkflowProjector::ProjectFromOutbox(
    const std::string& projector_name,
    int max_batch_size,
    std::string* error_out,
    int max_attempts) {
    if (projector_name.empty()) {
        if (error_out) *error_out = "projector_name is required";
        return false;
    }
    if (max_batch_size <= 0) {
        if (error_out) *error_out = "max_batch_size must be > 0";
        return false;
    }
    if (max_attempts <= 0) {
        if (error_out) *error_out = "max_attempts must be > 0";
        return false;
    }

    const auto checkpoint = GetCheckpoint(projector_name, error_out);

    events::OutboxRelay relay({
        .db = db_,
        .outbox_table = "exec_outbox_message",
        .context_name = "Execution",
        .aggregate_kind = "workflow_instance",
        .payload_ref_kind = "workflow_event",
        .max_attempts = max_attempts,
    });

    const auto project_workflow_instance = [this](const events::EventEnvelope& envelope, std::string* handler_error) {
        const auto workflow_instance_id = static_cast<std::int64_t>(
            std::strtoll(envelope.aggregate_id.c_str(), nullptr, 10));
        if (workflow_instance_id <= 0) {
            if (handler_error) *handler_error = "aggregate_id must parse to workflow_instance_id";
            return false;
        }

        return ProjectInstance(workflow_instance_id, handler_error);
    };

    const std::vector<events::OutboxRelayDispatchBinding> bindings{
        { { "Execution.WorkflowInstanceCreated.v1", 1 }, project_workflow_instance },
        { { "Execution.WorkflowStepReady.v1", 1 }, project_workflow_instance },
        { { "Execution.WorkflowStepMaterialized.v1", 1 }, project_workflow_instance },
        { { "Execution.WorkflowStepCompleted.v1", 1 }, project_workflow_instance },
        { { "Execution.WorkflowStepFailed.v1", 1 }, project_workflow_instance },
        { { "Execution.WorkflowInstanceCompleted.v1", 1 }, project_workflow_instance },
    };

    events::OutboxRelayResult relay_result{};
    if (!relay.RelayBatch(checkpoint, max_batch_size, bindings, &relay_result, error_out)) {
        return false;
    }

    if (relay_result.last_scanned_outbox_id > checkpoint) {
        simcore::db::SqliteUiReadDb ui_read_db(db_);
        const bool upserted = ui_read_db.UpsertProjectionCheckpoint({
            projector_name,
            std::string{},
            relay_result.last_scanned_outbox_id,
            simcore::db::types::UtcNow(),
        });

        if (!upserted) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
    }

    return true;
}

} // namespace simcore::db::execution::workflow
