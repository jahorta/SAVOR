#include "WorkflowRecoveryService.h"
#include "WorkflowOrchestration.h"

#include <chrono>
#include <sstream>
#include <string_view>

namespace savor::db::execution::workflow {

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

struct Statement {
    sqlite3_stmt* st = nullptr;
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }
};

bool Prepare(sqlite3* db, const char* sql, Statement* stmt, std::string* error_out) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt->st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

std::int64_t NowUtc() {
    const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
    return now.time_since_epoch().count();
}

bool TryRecordHandlerDedupeSemantic(
    sqlite3* db,
    std::string_view handler_name,
    std::string_view semantic_key,
    std::int64_t ts_utc,
    bool* inserted_out,
    std::string* error_out) {
    Statement insert;
    if (!Prepare(
            db,
            "INSERT OR IGNORE INTO exec_handler_dedupe(handler_name, event_id, semantic_key, first_seen_at_utc, last_seen_at_utc) "
            "VALUES(?1, NULL, ?2, ?3, ?3);",
            &insert,
            error_out)) {
        return false;
    }
    sqlite3_bind_text(insert.st, 1, handler_name.data(), static_cast<int>(handler_name.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.st, 2, semantic_key.data(), static_cast<int>(semantic_key.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.st, 3, ts_utc);
    if (sqlite3_step(insert.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }

    const bool inserted = sqlite3_changes(db) == 1;
    if (!inserted) {
        Statement update;
        if (!Prepare(
                db,
                "UPDATE exec_handler_dedupe SET last_seen_at_utc=?3 WHERE handler_name=?1 AND semantic_key=?2;",
                &update,
                error_out)) {
            return false;
        }
        sqlite3_bind_text(update.st, 1, handler_name.data(), static_cast<int>(handler_name.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(update.st, 2, semantic_key.data(), static_cast<int>(semantic_key.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(update.st, 3, ts_utc);
        if (sqlite3_step(update.st) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db);
            return false;
        }
    }

    if (inserted_out != nullptr) {
        *inserted_out = inserted;
    }
    return true;
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
        "WITH RECURSIVE job_set_descendants(workflow_step_id, workflow_instance_id, root_job_set_id, job_set_id, depth) AS ("
        "  SELECT s.workflow_step_id, s.workflow_instance_id, s.job_set_id, s.job_set_id, 0 "
        "  FROM exec_workflow_step s "
        "  JOIN exec_workflow_instance i ON i.workflow_instance_id=s.workflow_instance_id "
        "  WHERE i.state IN ('PENDING','RUNNING') AND s.state IN ('MATERIALIZED','RUNNING') AND s.job_set_id IS NOT NULL "
        "  UNION ALL "
        "  SELECT d.workflow_step_id, d.workflow_instance_id, d.root_job_set_id, child.job_set_id, d.depth + 1 "
        "  FROM exec_job_set child "
        "  JOIN job_set_descendants d ON child.parent_job_set_id=d.job_set_id "
        "  WHERE d.depth < 64"
        ") "
        "SELECT d.workflow_step_id, d.workflow_instance_id, d.root_job_set_id, "
        "CASE "
        "  WHEN COALESCE(SUM(CASE WHEN j.state IN ('FAILED','CANCELED') THEN 1 ELSE 0 END), 0) > 0 THEN 'FAILED' "
        "  WHEN COUNT(j.job_id) > 0 "
        "       AND COALESCE(SUM(CASE WHEN j.state IN ('COMPLETED','SUCCEEDED','SUCCEEDED_WINNER','SUPERSEDED','SUCCEEDED_DUPLICATE') THEN 1 ELSE 0 END), 0) = COUNT(j.job_id) THEN 'COMPLETED' "
        "  ELSE NULL "
        "END AS terminal_state "
        "FROM job_set_descendants d "
        "LEFT JOIN exec_job j ON j.job_set_id=d.job_set_id "
        "GROUP BY d.workflow_step_id, d.workflow_instance_id, d.root_job_set_id;";

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
        const auto operation_ts = NowUtc();
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

        std::ostringstream semantic_key_builder;
        semantic_key_builder << "workflow_step:" << workflow_step_id << ":terminal:" << next_step_state;
        bool inserted = false;
        if (!TryRecordHandlerDedupeSemantic(
                db_,
                "workflow_recovery_reconcile",
                semantic_key_builder.str(),
                operation_ts,
                &inserted,
                error_out)) {
            sqlite3_finalize(st);
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
        if (!inserted) {
            if (terminal_state == "COMPLETED") {
                --local.completed_steps;
            } else if (terminal_state == "FAILED") {
                --local.failed_steps;
            }
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
        sqlite3_bind_int64(up, 3, operation_ts);
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
        sqlite3_bind_int64(ev, 4, operation_ts);
        sqlite3_bind_text(ev, 5, message, -1, SQLITE_STATIC);
        if (sqlite3_step(ev) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            sqlite3_finalize(ev);
            sqlite3_finalize(st);
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
        const auto workflow_event_id = sqlite3_last_insert_rowid(db_);
        sqlite3_finalize(ev);

        sqlite3_stmt* outbox = nullptr;
        constexpr const char* kOutboxInsert =
            "INSERT INTO exec_outbox_message("
            "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
            "VALUES(?1, ?2, 1, 'Execution', 'workflow_instance', ?3, ?4, ?5, ?6, 'workflow_event', ?7);";
        if (sqlite3_prepare_v2(db_, kOutboxInsert, -1, &outbox, nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            sqlite3_finalize(st);
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
        std::ostringstream event_id;
        event_id << "workflow-recovery-" << workflow_instance_id << "-" << workflow_event_id;
        const auto event_id_value = event_id.str();
        const auto aggregate_id = std::to_string(workflow_instance_id);
        const auto correlation_id = "workflow-instance-" + aggregate_id;
        const auto causation_id = "workflow-step-" + std::to_string(workflow_step_id);
        sqlite3_bind_text(outbox, 1, event_id_value.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(outbox, 2, event_kind, -1, SQLITE_STATIC);
        sqlite3_bind_text(outbox, 3, aggregate_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(outbox, 4, correlation_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(outbox, 5, causation_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(outbox, 6, operation_ts);
        sqlite3_bind_int64(outbox, 7, workflow_event_id);
        if (sqlite3_step(outbox) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            sqlite3_finalize(outbox);
            sqlite3_finalize(st);
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
        sqlite3_finalize(outbox);
    }
    sqlite3_finalize(st);

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (result_out) *result_out = local;
    return true;
}

bool WorkflowRecoveryService::PlanInvariantRemediation(
    const WorkflowInvariantRemediationCommand& command,
    WorkflowInvariantRemediationDecision* decision_out,
    std::string* error_out) {
    if (command.workflow_instance_id <= 0 || command.workflow_step_id <= 0) {
        if (error_out) *error_out = "workflow_instance_id and workflow_step_id must be > 0";
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "WITH RECURSIVE step_root AS ("
        "  SELECT s.job_set_id FROM exec_workflow_step s "
        "  WHERE s.workflow_instance_id=?1 AND s.workflow_step_id=?2 LIMIT 1"
        "), "
        "job_set_descendants(job_set_id, depth) AS ("
        "  SELECT job_set_id, 0 FROM step_root "
        "  UNION ALL "
        "  SELECT child.job_set_id, job_set_descendants.depth + 1 "
        "  FROM exec_job_set child "
        "  JOIN job_set_descendants ON child.parent_job_set_id=job_set_descendants.job_set_id "
        "  WHERE job_set_descendants.depth < 64"
        ") "
        "SELECT "
        "CASE WHEN EXISTS(SELECT 1 FROM job_set_descendants WHERE depth > 0) "
        "  THEN (SELECT COALESCE(SUM(COALESCE(js.expected_total, 0)), 0) FROM exec_job_set js JOIN job_set_descendants d ON d.job_set_id=js.job_set_id WHERE d.depth > 0) "
        "  ELSE (SELECT COALESCE(js.expected_total, 0) FROM exec_job_set js JOIN step_root r ON r.job_set_id=js.job_set_id) END, "
        "(SELECT COUNT(1) FROM exec_job j JOIN job_set_descendants d ON d.job_set_id=j.job_set_id), "
        "(SELECT COUNT(1) FROM exec_job j JOIN job_set_descendants d ON d.job_set_id=j.job_set_id "
        "  WHERE j.state IN ('COMPLETED','SUCCEEDED','SUCCEEDED_WINNER','SUPERSEDED','SUCCEEDED_DUPLICATE','FAILED','CANCELED')), "
        "(SELECT COUNT(1) FROM exec_job j JOIN job_set_descendants d ON d.job_set_id=j.job_set_id WHERE j.state='FAILED');";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st, 1, command.workflow_instance_id);
    sqlite3_bind_int64(st, 2, command.workflow_step_id);

    if (sqlite3_step(st) != SQLITE_ROW) {
        if (error_out) *error_out = "workflow step not found for remediation planning";
        sqlite3_finalize(st);
        return false;
    }

    const int expected_total = sqlite3_column_int(st, 0);
    const int discovered_total = sqlite3_column_int(st, 1);
    const int terminal_total = sqlite3_column_int(st, 2);
    const int failed_total = sqlite3_column_int(st, 3);
    sqlite3_finalize(st);

    WorkflowInvariantRemediationDecision decision{};
    if (discovered_total > 0
        && terminal_total == discovered_total
        && (expected_total == 0 || discovered_total == expected_total)) {
        std::ostringstream reason;
        reason << "repair_reopen expected=" << expected_total
               << " discovered=" << discovered_total
               << " terminal=" << terminal_total
               << " failed=" << failed_total;
        decision.can_reopen = true;
        decision.reason = reason.str();
    } else {
        std::ostringstream detail;
        detail << "repair_terminal_fail expected=" << expected_total
               << " discovered=" << discovered_total
               << " terminal=" << terminal_total
               << " failed=" << failed_total;
        decision.can_reopen = false;
        decision.failure_code = "WORKFLOW_INVARIANT_UNRECOVERABLE";
        decision.failure_message = detail.str();
    }

    if (decision_out != nullptr) {
        *decision_out = decision;
    }
    return true;
}

bool WorkflowRecoveryService::ExecuteInvariantRemediation(
    const WorkflowInvariantRemediationCommand& command,
    IWorkflowOrchestrationCommandService* command_service,
    bool* reopened_out,
    std::string* error_out) {
    if (reopened_out != nullptr) {
        *reopened_out = false;
    }
    if (command_service == nullptr) {
        if (error_out) *error_out = "command service is required";
        return false;
    }

    const std::string violation_reason = command.violation_reason.empty()
        ? "WORKFLOW_INVARIANT_VIOLATION"
        : command.violation_reason;
    const std::string requested_by = command.requested_by.empty()
        ? "workflow_recovery_service"
        : command.requested_by;

    std::string command_error;
    if (!command_service->PauseWorkflowInstance(
            {
                .workflow_instance_id = command.workflow_instance_id,
                .reason = violation_reason,
                .failure_code = "WORKFLOW_INVARIANT_VIOLATION",
                .requested_by = requested_by,
            },
            &command_error)) {
        Statement state;
        if (!Prepare(
                db_,
                "SELECT state FROM exec_workflow_instance WHERE workflow_instance_id=?1;",
                &state,
                error_out)) {
            return false;
        }
        sqlite3_bind_int64(state.st, 1, command.workflow_instance_id);
        if (sqlite3_step(state.st) != SQLITE_ROW) {
            if (error_out) *error_out = command_error;
            return false;
        }
        const unsigned char* state_text = sqlite3_column_text(state.st, 0);
        const std::string instance_state = state_text ? reinterpret_cast<const char*>(state_text) : "";
        if (instance_state != "FAILED") {
            if (error_out) *error_out = command_error;
            return false;
        }
    }

    WorkflowInvariantRemediationDecision decision{};
    if (!PlanInvariantRemediation(command, &decision, error_out)) {
        return false;
    }

    const std::optional<std::string> repair_message = decision.can_reopen
        ? std::optional<std::string>(decision.reason)
        : std::optional<std::string>(decision.failure_message);
    if (!command_service->AppendLifecycleEvent(
            {
                .workflow_instance_id = command.workflow_instance_id,
                .workflow_step_id = command.workflow_step_id,
                .event_kind = "Execution.WorkflowRemediationRepairExecuted.v1",
                .message = repair_message,
                .requested_by = requested_by,
            },
            &command_error)) {
        if (error_out) *error_out = command_error;
        return false;
    }

    if (decision.can_reopen) {
        if (!command_service->ResumeWorkflowInstance(
                {
                    .workflow_instance_id = command.workflow_instance_id,
                    .requested_by = requested_by,
                },
                &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        }

        if (!command_service->AppendLifecycleEvent(
                {
                    .workflow_instance_id = command.workflow_instance_id,
                    .workflow_step_id = command.workflow_step_id,
                    .event_kind = "Execution.WorkflowRemediationReopened.v1",
                    .message = std::optional<std::string>(decision.reason),
                    .requested_by = requested_by,
                },
                &command_error)) {
            if (error_out) *error_out = command_error;
            return false;
        }

        if (reopened_out != nullptr) {
            *reopened_out = true;
        }
        return true;
    }

    if (!command_service->TerminalFailWorkflowInstance(
            {
                .workflow_instance_id = command.workflow_instance_id,
                .failure_code = decision.failure_code,
                .failure_message = decision.failure_message,
                .requested_by = requested_by,
            },
            &command_error)) {
        if (error_out) *error_out = command_error;
        return false;
    }

    return true;
}

bool WorkflowRecoveryService::PurgeHandlerDedupeOlderThan(
    std::int64_t last_seen_at_utc_exclusive,
    int max_rows,
    int* rows_deleted_out,
    std::string* error_out) {
    if (rows_deleted_out != nullptr) {
        *rows_deleted_out = 0;
    }
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
    if (!Prepare(
            db_,
            "DELETE FROM exec_handler_dedupe WHERE dedupe_id IN ("
            "SELECT dedupe_id FROM exec_handler_dedupe "
            "WHERE last_seen_at_utc < ?1 "
            "ORDER BY last_seen_at_utc ASC, dedupe_id ASC "
            "LIMIT ?2);",
            &st,
            error_out)) {
        return false;
    }
    sqlite3_bind_int64(st.st, 1, last_seen_at_utc_exclusive);
    sqlite3_bind_int(st.st, 2, max_rows);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (rows_deleted_out != nullptr) {
        *rows_deleted_out = sqlite3_changes(db_);
    }
    return true;
}

} // namespace savor::db::execution::workflow
