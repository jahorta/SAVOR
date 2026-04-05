#include "WorkflowRecoveryService.h"

#include <ctime>

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

std::int64_t NowUtc() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

} // namespace

WorkflowRecoveryService::WorkflowRecoveryService(sqlite3* db)
    : db_(db) {
}

bool WorkflowRecoveryService::ReconcileInFlightInstances(WorkflowRecoveryResult* result_out, std::string* error_out) {
    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSelect =
        "SELECT s.workflow_step_id, s.workflow_instance_id, s.job_set_id, "
        "(SELECT j.state FROM exec_job j WHERE j.job_set_id = s.job_set_id ORDER BY j.ended_at_utc DESC, j.job_id DESC LIMIT 1) AS terminal_state "
        "FROM exec_workflow_step s "
        "JOIN exec_workflow_instance i ON i.workflow_instance_id=s.workflow_instance_id "
        "WHERE i.state IN ('PENDING','RUNNING') AND s.state IN ('MATERIALIZED','RUNNING') AND s.job_set_id IS NOT NULL;";

    if (sqlite3_prepare_v2(db_, kSelect, -1, &st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    WorkflowRecoveryResult local{};
    while (sqlite3_step(st) == SQLITE_ROW) {
        const auto workflow_step_id = sqlite3_column_int64(st, 0);
        const auto workflow_instance_id = sqlite3_column_int64(st, 1);
        const unsigned char* terminal_state_text = sqlite3_column_text(st, 3);
        const std::string terminal_state = terminal_state_text ? reinterpret_cast<const char*>(terminal_state_text) : "";

        const char* next_step_state = nullptr;
        const char* event_kind = nullptr;
        const char* message = nullptr;
        if (terminal_state == "COMPLETED") {
            next_step_state = "COMPLETED";
            event_kind = "Execution.WorkflowStepCompleted.v1";
            message = "recovery_reconciled_completed";
            ++local.completed_steps;
        } else if (terminal_state == "FAILED") {
            next_step_state = "FAILED";
            event_kind = "Execution.WorkflowStepFailed.v1";
            message = "recovery_reconciled_failed";
            ++local.failed_steps;
        } else {
            continue;
        }

        sqlite3_stmt* up = nullptr;
        constexpr const char* kUpdate =
            "UPDATE exec_workflow_step SET state=?2, completed_at_utc=CASE WHEN ?2='COMPLETED' THEN ?3 ELSE completed_at_utc END, "
            "failed_at_utc=CASE WHEN ?2='FAILED' THEN ?3 ELSE failed_at_utc END WHERE workflow_step_id=?1;";
        if (sqlite3_prepare_v2(db_, kUpdate, -1, &up, nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            sqlite3_finalize(st);
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
        sqlite3_bind_int64(up, 1, workflow_step_id);
        sqlite3_bind_text(up, 2, next_step_state, -1, SQLITE_STATIC);
        sqlite3_bind_int64(up, 3, NowUtc());
        if (sqlite3_step(up) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            sqlite3_finalize(up);
            sqlite3_finalize(st);
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
        sqlite3_finalize(up);

        sqlite3_stmt* ev = nullptr;
        constexpr const char* kEventInsert =
            "INSERT INTO exec_workflow_event(workflow_instance_id, workflow_step_id, event_kind, event_ts_utc, message) "
            "VALUES(?1, ?2, ?3, ?4, ?5);";
        if (sqlite3_prepare_v2(db_, kEventInsert, -1, &ev, nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            sqlite3_finalize(st);
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
        sqlite3_bind_int64(ev, 1, workflow_instance_id);
        sqlite3_bind_int64(ev, 2, workflow_step_id);
        sqlite3_bind_text(ev, 3, event_kind, -1, SQLITE_STATIC);
        sqlite3_bind_int64(ev, 4, NowUtc());
        sqlite3_bind_text(ev, 5, message, -1, SQLITE_STATIC);
        if (sqlite3_step(ev) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            sqlite3_finalize(ev);
            sqlite3_finalize(st);
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
        sqlite3_finalize(ev);
    }
    sqlite3_finalize(st);

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (result_out) *result_out = local;
    return true;
}

} // namespace simcore::db::execution::workflow
