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

    constexpr const char* kNonTerminalInTerminalInstanceSql =
        "SELECT COUNT(1) "
        "FROM exec_workflow_step s "
        "JOIN exec_workflow_instance i ON i.workflow_instance_id=s.workflow_instance_id "
        "WHERE i.state IN ('COMPLETED','FAILED','CANCELED') AND s.state NOT IN ('COMPLETED','FAILED','SKIPPED');";
    if (!QueryScalarInt(db, kNonTerminalInTerminalInstanceSql, &report.non_terminal_step_in_terminal_instance_count, error_out)) {
        return false;
    }

    constexpr const char* kCompletedStepMissingCompletionTsSql =
        "SELECT COUNT(1) "
        "FROM exec_workflow_step "
        "WHERE state='COMPLETED' AND completed_at_utc IS NULL;";
    if (!QueryScalarInt(db, kCompletedStepMissingCompletionTsSql, &report.completed_step_missing_completion_ts_count, error_out)) {
        return false;
    }

    constexpr const char* kMissingStepActivationSql =
        "SELECT COUNT(1) "
        "FROM exec_workflow_step s "
        "LEFT JOIN exec_workflow_unit_activation a ON a.workflow_unit_activation_id=s.workflow_unit_activation_id "
        "WHERE EXISTS ("
        "  SELECT 1 FROM exec_workflow_unit_activation ax "
        "  WHERE ax.workflow_instance_id=s.workflow_instance_id"
        ") "
        "AND (s.workflow_unit_activation_id IS NULL "
        "OR a.workflow_unit_activation_id IS NULL "
        "OR a.workflow_instance_id<>s.workflow_instance_id);";
    if (!QueryScalarInt(db, kMissingStepActivationSql, &report.missing_step_activation_count, error_out)) {
        return false;
    }

    constexpr const char* kDanglingActivationEdgesSql =
        "SELECT COUNT(1) "
        "FROM exec_workflow_unit_activation_edge e "
        "LEFT JOIN exec_workflow_unit_activation fa ON fa.workflow_unit_activation_id=e.from_workflow_unit_activation_id "
        "LEFT JOIN exec_workflow_unit_activation ta ON ta.workflow_unit_activation_id=e.to_workflow_unit_activation_id "
        "WHERE fa.workflow_unit_activation_id IS NULL OR ta.workflow_unit_activation_id IS NULL "
        "OR fa.workflow_instance_id<>e.workflow_instance_id OR ta.workflow_instance_id<>e.workflow_instance_id;";
    if (!QueryScalarInt(db, kDanglingActivationEdgesSql, &report.dangling_activation_edge_count, error_out)) {
        return false;
    }

    constexpr const char* kNonTerminalRootActivationInCompletedInstanceSql =
        "SELECT COUNT(1) "
        "FROM exec_workflow_unit_activation a "
        "JOIN exec_workflow_instance i ON i.workflow_instance_id=a.workflow_instance_id "
        "WHERE i.state='COMPLETED' "
        "AND a.parent_workflow_unit_activation_id IS NULL "
        "AND a.state NOT IN ('COMPLETED','FAILED','SKIPPED','CANCELED');";
    if (!QueryScalarInt(db, kNonTerminalRootActivationInCompletedInstanceSql, &report.non_terminal_root_activation_in_completed_instance_count, error_out)) {
        return false;
    }

    if (report_out) {
        *report_out = report;
    }
    return true;
}

} // namespace simcore::db::execution::workflow
