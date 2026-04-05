#include "WorkflowProjector.h"

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

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

} // namespace simcore::db::execution::workflow
