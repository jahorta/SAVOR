#include "WorkflowIntegrityChecks.h"

namespace simcore::db::execution::workflow {

namespace {

bool QueryScalarInt(sqlite3* db, const char* sql, int* out, std::string* error_out) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }

    if (sqlite3_step(st) != SQLITE_ROW) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        sqlite3_finalize(st);
        return false;
    }

    *out = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return true;
}

} // namespace

bool RunWorkflowIntegrityChecks(sqlite3* db, WorkflowIntegrityReport* report_out, std::string* error_out) {
    if (db == nullptr) {
        if (error_out) *error_out = "db must not be null";
        return false;
    }

    WorkflowIntegrityReport report{};

    constexpr const char* kDanglingEdgesSql =
        "SELECT COUNT(1) "
        "FROM exec_workflow_edge e "
        "LEFT JOIN exec_workflow_step fs ON fs.workflow_step_id=e.from_step_id "
        "LEFT JOIN exec_workflow_step ts ON ts.workflow_step_id=e.to_step_id "
        "WHERE fs.workflow_step_id IS NULL OR ts.workflow_step_id IS NULL;";
    if (!QueryScalarInt(db, kDanglingEdgesSql, &report.dangling_edge_count, error_out)) {
        return false;
    }

    constexpr const char* kMissingJobSetLinkSql =
        "SELECT COUNT(1) "
        "FROM exec_workflow_step "
        "WHERE state IN ('MATERIALIZED','RUNNING','COMPLETED','FAILED') AND job_set_id IS NULL;";
    if (!QueryScalarInt(db, kMissingJobSetLinkSql, &report.missing_job_set_link_count, error_out)) {
        return false;
    }

    constexpr const char* kNonTerminalInCompletedInstanceSql =
        "SELECT COUNT(1) "
        "FROM exec_workflow_step s "
        "JOIN exec_workflow_instance i ON i.workflow_instance_id=s.workflow_instance_id "
        "WHERE i.state='COMPLETED' AND s.state NOT IN ('COMPLETED','FAILED','SKIPPED');";
    if (!QueryScalarInt(db, kNonTerminalInCompletedInstanceSql, &report.non_terminal_step_in_completed_instance_count, error_out)) {
        return false;
    }

    if (report_out) {
        *report_out = report;
    }
    return true;
}

} // namespace simcore::db::execution::workflow
