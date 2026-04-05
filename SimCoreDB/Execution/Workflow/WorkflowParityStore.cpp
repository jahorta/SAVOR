#include "WorkflowParityStore.h"

#include <ctime>

namespace simcore::db::execution::workflow {

namespace {

bool Exec(sqlite3* db, const char* sql, std::string* error_out) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (error_out) {
            *error_out = err ? err : "sqlite3_exec failed";
        }
        sqlite3_free(err);
        return false;
    }
    return true;
}

std::int64_t NowUtc() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

} // namespace

WorkflowParityStore::WorkflowParityStore(sqlite3* db)
    : db_(db) {
}

bool WorkflowParityStore::EnsureSchema(std::string* error_out) const {
    constexpr const char* kSql = R"SQL(
CREATE TABLE IF NOT EXISTS exec_workflow_parity_result (
    parity_result_id INTEGER PRIMARY KEY,
    run_ref TEXT NOT NULL,
    workflow_instance_id INTEGER NOT NULL,
    compared_steps INTEGER NOT NULL,
    matched_steps INTEGER NOT NULL,
    mismatch_steps INTEGER NOT NULL,
    recorded_at_utc INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS exec_workflow_parity_mismatch (
    parity_mismatch_id INTEGER PRIMARY KEY,
    run_ref TEXT NOT NULL,
    workflow_instance_id INTEGER NOT NULL,
    step_key TEXT NOT NULL,
    legacy_outcome TEXT NULL,
    workflow_outcome TEXT NULL,
    category TEXT NOT NULL,
    recorded_at_utc INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS ix_exec_workflow_parity_result_run_instance
    ON exec_workflow_parity_result(run_ref, workflow_instance_id, recorded_at_utc DESC);

CREATE INDEX IF NOT EXISTS ix_exec_workflow_parity_mismatch_run_instance
    ON exec_workflow_parity_mismatch(run_ref, workflow_instance_id, category);
)SQL";

    return Exec(db_, kSql, error_out);
}

bool WorkflowParityStore::PersistReport(
    const std::string& run_ref,
    std::int64_t workflow_instance_id,
    const WorkflowParityReport& report,
    std::string* error_out) {
    if (run_ref.empty()) {
        if (error_out) *error_out = "run_ref is required";
        return false;
    }
    if (workflow_instance_id <= 0) {
        if (error_out) *error_out = "workflow_instance_id must be > 0";
        return false;
    }
    if (!EnsureSchema(error_out)) {
        return false;
    }
    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    sqlite3_stmt* result_stmt = nullptr;
    constexpr const char* kInsertResult =
        "INSERT INTO exec_workflow_parity_result(run_ref, workflow_instance_id, compared_steps, matched_steps, mismatch_steps, recorded_at_utc) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6);";
    if (sqlite3_prepare_v2(db_, kInsertResult, -1, &result_stmt, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    const auto now = NowUtc();
    sqlite3_bind_text(result_stmt, 1, run_ref.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(result_stmt, 2, workflow_instance_id);
    sqlite3_bind_int(result_stmt, 3, report.compared_steps);
    sqlite3_bind_int(result_stmt, 4, report.matched_steps);
    sqlite3_bind_int(result_stmt, 5, static_cast<int>(report.mismatches.size()));
    sqlite3_bind_int64(result_stmt, 6, now);
    if (sqlite3_step(result_stmt) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        sqlite3_finalize(result_stmt);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_finalize(result_stmt);

    sqlite3_stmt* mismatch_stmt = nullptr;
    constexpr const char* kInsertMismatch =
        "INSERT INTO exec_workflow_parity_mismatch("
        "run_ref, workflow_instance_id, step_key, legacy_outcome, workflow_outcome, category, recorded_at_utc) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7);";
    if (sqlite3_prepare_v2(db_, kInsertMismatch, -1, &mismatch_stmt, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    for (const auto& mismatch : report.mismatches) {
        sqlite3_reset(mismatch_stmt);
        sqlite3_clear_bindings(mismatch_stmt);
        sqlite3_bind_text(mismatch_stmt, 1, run_ref.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(mismatch_stmt, 2, workflow_instance_id);
        sqlite3_bind_text(mismatch_stmt, 3, mismatch.step_key.c_str(), -1, SQLITE_TRANSIENT);
        if (mismatch.legacy_outcome.empty()) {
            sqlite3_bind_null(mismatch_stmt, 4);
        } else {
            sqlite3_bind_text(mismatch_stmt, 4, mismatch.legacy_outcome.c_str(), -1, SQLITE_TRANSIENT);
        }
        if (mismatch.workflow_outcome.empty()) {
            sqlite3_bind_null(mismatch_stmt, 5);
        } else {
            sqlite3_bind_text(mismatch_stmt, 5, mismatch.workflow_outcome.c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_bind_text(mismatch_stmt, 6, mismatch.category.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(mismatch_stmt, 7, now);

        if (sqlite3_step(mismatch_stmt) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            sqlite3_finalize(mismatch_stmt);
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
    }
    sqlite3_finalize(mismatch_stmt);

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

std::vector<WorkflowParitySummary> WorkflowParityStore::ListSummaries(std::string* error_out) const {
    std::vector<WorkflowParitySummary> rows;
    if (!EnsureSchema(error_out)) {
        return rows;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT run_ref, workflow_instance_id, compared_steps, matched_steps, mismatch_steps "
        "FROM exec_workflow_parity_result ORDER BY recorded_at_utc DESC, parity_result_id DESC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return rows;
    }

    while (sqlite3_step(st) == SQLITE_ROW) {
        WorkflowParitySummary row;
        row.run_ref = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        row.workflow_instance_id = sqlite3_column_int64(st, 1);
        row.compared_steps = sqlite3_column_int(st, 2);
        row.matched_steps = sqlite3_column_int(st, 3);
        row.mismatch_steps = sqlite3_column_int(st, 4);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    return rows;
}

} // namespace simcore::db::execution::workflow
