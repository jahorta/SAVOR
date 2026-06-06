#include "SqliteWorkflowOrchestration.h"

#include <chrono>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "../../Common/Events/EventCatalog.h"

namespace simcore::db::execution::workflow {

namespace {

struct Statement {
    sqlite3_stmt* st = nullptr;
    ~Statement() {
        if (st) sqlite3_finalize(st);
    }
};

bool Exec(sqlite3* db, const char* sql, std::string* error_out) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (error_out) *error_out = err ? err : "sqlite3_exec failed";
        sqlite3_free(err);
        return false;
    }
    return true;
}

const char* ToDb(WorkflowInstanceState state) {
    switch (state) {
    case WorkflowInstanceState::Pending: return "PENDING";
    case WorkflowInstanceState::Running: return "RUNNING";
    case WorkflowInstanceState::Completed: return "COMPLETED";
    case WorkflowInstanceState::Failed: return "FAILED";
    case WorkflowInstanceState::Canceled: return "CANCELED";
    }
    return "PENDING";
}

const char* ToDb(WorkflowStepState state) {
    switch (state) {
    case WorkflowStepState::Waiting: return "WAITING";
    case WorkflowStepState::Ready: return "READY";
    case WorkflowStepState::Materialized: return "MATERIALIZED";
    case WorkflowStepState::Running: return "RUNNING";
    case WorkflowStepState::Completed: return "COMPLETED";
    case WorkflowStepState::Failed: return "FAILED";
    case WorkflowStepState::Skipped: return "SKIPPED";
    }
    return "WAITING";
}

WorkflowInstanceState ParseInstanceState(const unsigned char* state_text) {
    const std::string value = state_text ? reinterpret_cast<const char*>(state_text) : "";
    if (value == "RUNNING") return WorkflowInstanceState::Running;
    if (value == "COMPLETED") return WorkflowInstanceState::Completed;
    if (value == "FAILED") return WorkflowInstanceState::Failed;
    if (value == "CANCELED") return WorkflowInstanceState::Canceled;
    return WorkflowInstanceState::Pending;
}

WorkflowStepState ParseStepState(const unsigned char* state_text) {
    const std::string value = state_text ? reinterpret_cast<const char*>(state_text) : "";
    if (value == "READY") return WorkflowStepState::Ready;
    if (value == "MATERIALIZED") return WorkflowStepState::Materialized;
    if (value == "RUNNING") return WorkflowStepState::Running;
    if (value == "COMPLETED") return WorkflowStepState::Completed;
    if (value == "FAILED") return WorkflowStepState::Failed;
    if (value == "SKIPPED") return WorkflowStepState::Skipped;
    return WorkflowStepState::Waiting;
}

std::int64_t NowUtc() {
    const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
    return now.time_since_epoch().count();
}

bool Prepare(sqlite3* db, const char* sql, Statement* stmt, std::string* error_out) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt->st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

std::optional<std::int64_t> ColumnInt64Optional(sqlite3_stmt* st, int index) {
    if (sqlite3_column_type(st, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st, index);
}

std::optional<std::string> ColumnTextOptional(sqlite3_stmt* st, int index) {
    if (sqlite3_column_type(st, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    const unsigned char* text = sqlite3_column_text(st, index);
    return text ? std::optional<std::string>(reinterpret_cast<const char*>(text)) : std::optional<std::string>("");
}

std::optional<WorkflowStepTerminalSnapshot> ReadStepTerminalSnapshot(sqlite3_stmt* st) {
    WorkflowStepTerminalSnapshot snapshot{};
    snapshot.workflow_instance_id = sqlite3_column_int64(st, 0);
    snapshot.workflow_step_id = sqlite3_column_int64(st, 1);
    snapshot.job_set_id = sqlite3_column_int64(st, 2);

    const auto* workflow_kind = sqlite3_column_text(st, 3);
    const auto* step_key = sqlite3_column_text(st, 4);
    const auto* step_kind = sqlite3_column_text(st, 5);
    snapshot.workflow_kind = workflow_kind ? reinterpret_cast<const char*>(workflow_kind) : "";
    snapshot.step_key = step_key ? reinterpret_cast<const char*>(step_key) : "";
    snapshot.step_kind = step_kind ? reinterpret_cast<const char*>(step_kind) : "";
    snapshot.input_ref_kind = ColumnTextOptional(st, 6);
    snapshot.input_ref_id = ColumnInt64Optional(st, 7);

    snapshot.expected_total = sqlite3_column_int(st, 8);
    snapshot.discovered_total = sqlite3_column_int(st, 9);
    snapshot.terminal_total = sqlite3_column_int(st, 10);
    snapshot.failed_total = sqlite3_column_int(st, 11);
    return snapshot;
}

} // namespace

SqliteWorkflowOrchestrationQueryService::SqliteWorkflowOrchestrationQueryService(sqlite3* db)
    : db_(db) {
}

std::vector<WorkflowInstanceRecord> SqliteWorkflowOrchestrationQueryService::ListWorkflowInstances(
    WorkflowInstanceState state,
    std::int64_t created_at_utc_start,
    std::int64_t created_at_utc_end) const {
    std::vector<WorkflowInstanceRecord> rows;

    Statement st;
    if (!Prepare(db_,
        "SELECT workflow_instance_id, workflow_kind, state, root_scope_kind, root_scope_id, input_ref_kind, input_ref_id, workflow_graph_revision_id "
        "FROM exec_workflow_instance WHERE state=?1 AND created_at_utc BETWEEN ?2 AND ?3 "
        "ORDER BY created_at_utc DESC, workflow_instance_id DESC;",
        &st,
        nullptr)) {
        return rows;
    }

    sqlite3_bind_text(st.st, 1, ToDb(state), -1, SQLITE_STATIC);
    sqlite3_bind_int64(st.st, 2, created_at_utc_start);
    sqlite3_bind_int64(st.st, 3, created_at_utc_end);

    while (sqlite3_step(st.st) == SQLITE_ROW) {
        WorkflowInstanceRecord row;
        row.workflow_instance_id = sqlite3_column_int64(st.st, 0);
        row.workflow_kind = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 1));
        row.state = ParseInstanceState(sqlite3_column_text(st.st, 2));
        row.root_scope_kind = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 3));
        row.root_scope_id = ColumnInt64Optional(st.st, 4);
        row.input_ref_kind = ColumnTextOptional(st.st, 5);
        row.input_ref_id = ColumnInt64Optional(st.st, 6);
        row.workflow_graph_revision_id = ColumnInt64Optional(st.st, 7);
        rows.push_back(std::move(row));
    }

    return rows;
}

std::vector<WorkflowReadyStepRecord> SqliteWorkflowOrchestrationQueryService::ListReadySteps(std::size_t limit) const {
    std::vector<WorkflowReadyStepRecord> rows;
    if (limit == 0) {
        return rows;
    }

    Statement st;
    if (!Prepare(db_,
        "SELECT s.workflow_instance_id, s.workflow_step_id, s.step_key, s.step_kind, s.priority, s.input_ref_id "
        "FROM exec_workflow_step s "
        "JOIN exec_workflow_instance i ON i.workflow_instance_id=s.workflow_instance_id "
        "WHERE i.state='RUNNING' AND s.state='READY' AND s.job_set_id IS NULL "
        "ORDER BY s.priority DESC, s.ready_at_utc ASC, s.workflow_step_id ASC "
        "LIMIT ?1;",
        &st,
        nullptr)) {
        return rows;
    }

    sqlite3_bind_int64(st.st, 1, static_cast<sqlite3_int64>(limit));
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        WorkflowReadyStepRecord row;
        row.workflow_instance_id = sqlite3_column_int64(st.st, 0);
        row.workflow_step_id = sqlite3_column_int64(st.st, 1);
        row.step_key = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 2));
        row.step_kind = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 3));
        row.priority = sqlite3_column_int(st.st, 4);
        row.input_ref_id = ColumnInt64Optional(st.st, 5);
        rows.push_back(std::move(row));
    }

    return rows;
}

std::optional<WorkflowStepTerminalSnapshot> SqliteWorkflowOrchestrationQueryService::GetStepTerminalSnapshotForJob(
    std::int64_t job_id) const {
    if (job_id <= 0) {
        return std::nullopt;
    }

    Statement st;
    if (!Prepare(db_,
        "WITH RECURSIVE job_set_ancestry(job_set_id, parent_job_set_id, depth) AS ("
        "  SELECT js.job_set_id, js.parent_job_set_id, 0 "
        "  FROM exec_job source "
        "  JOIN exec_job_set js ON js.job_set_id=source.job_set_id "
        "  WHERE source.job_id=?1 "
        "  UNION ALL "
        "  SELECT parent.job_set_id, parent.parent_job_set_id, job_set_ancestry.depth + 1 "
        "  FROM exec_job_set parent "
        "  JOIN job_set_ancestry ON parent.job_set_id=job_set_ancestry.parent_job_set_id "
        "  WHERE job_set_ancestry.parent_job_set_id IS NOT NULL "
        "    AND job_set_ancestry.depth < 64"
        "), "
        "step_root AS ("
        "  SELECT i.workflow_instance_id, s.workflow_step_id, s.job_set_id, i.workflow_kind, s.step_key, s.step_kind, s.input_ref_kind, s.input_ref_id "
        "  FROM job_set_ancestry a "
        "  JOIN exec_workflow_step s ON s.job_set_id=a.job_set_id "
        "  JOIN exec_workflow_instance i ON i.workflow_instance_id=s.workflow_instance_id "
        "  ORDER BY a.depth ASC LIMIT 1"
        "), "
        "job_set_descendants(job_set_id, depth) AS ("
        "  SELECT job_set_id, 0 FROM step_root "
        "  UNION ALL "
        "  SELECT child.job_set_id, job_set_descendants.depth + 1 "
        "  FROM exec_job_set child "
        "  JOIN job_set_descendants ON child.parent_job_set_id=job_set_descendants.job_set_id "
        "  WHERE job_set_descendants.depth < 64"
        ") "
        "SELECT r.workflow_instance_id, r.workflow_step_id, r.job_set_id, r.workflow_kind, r.step_key, r.step_kind, r.input_ref_kind, r.input_ref_id, "
        "CASE WHEN EXISTS(SELECT 1 FROM job_set_descendants WHERE depth > 0) "
        "  THEN (SELECT COALESCE(SUM(COALESCE(js.expected_total, 0)), 0) FROM exec_job_set js JOIN job_set_descendants d ON d.job_set_id=js.job_set_id WHERE d.depth > 0) "
        "  ELSE COALESCE(root_js.expected_total, 0) END, "
        "(SELECT COUNT(1) FROM exec_job j JOIN job_set_descendants d ON d.job_set_id=j.job_set_id), "
        "(SELECT COUNT(1) FROM exec_job j JOIN job_set_descendants d ON d.job_set_id=j.job_set_id "
        "  WHERE j.state IN ('COMPLETED','SUCCEEDED','SUCCEEDED_WINNER','SUPERSEDED','SUCCEEDED_DUPLICATE','FAILED','CANCELED')), "
        "(SELECT COUNT(1) FROM exec_job j JOIN job_set_descendants d ON d.job_set_id=j.job_set_id WHERE j.state='FAILED') "
        "FROM step_root r "
        "LEFT JOIN exec_job_set root_js ON root_js.job_set_id=r.job_set_id;",
        &st,
        nullptr)) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return ReadStepTerminalSnapshot(st.st);
}

std::vector<WorkflowStepTerminalSnapshot> SqliteWorkflowOrchestrationQueryService::ListTerminalReadyStepSnapshots(
    std::size_t limit) const {
    std::vector<WorkflowStepTerminalSnapshot> rows;
    if (limit == 0) {
        return rows;
    }

    Statement st;
    if (!Prepare(db_,
        "WITH RECURSIVE step_root AS ("
        "  SELECT i.workflow_instance_id, s.workflow_step_id, s.job_set_id, i.workflow_kind, s.step_key, s.step_kind, s.input_ref_kind, s.input_ref_id "
        "  FROM exec_workflow_step s "
        "  JOIN exec_workflow_instance i ON i.workflow_instance_id=s.workflow_instance_id "
        "  WHERE i.state IN ('PENDING','RUNNING') "
        "    AND s.state IN ('MATERIALIZED','RUNNING') "
        "    AND s.job_set_id IS NOT NULL "
        "), "
        "job_set_descendants(workflow_instance_id, workflow_step_id, root_job_set_id, workflow_kind, step_key, step_kind, input_ref_kind, input_ref_id, job_set_id, depth) AS ("
        "  SELECT workflow_instance_id, workflow_step_id, job_set_id, workflow_kind, step_key, step_kind, input_ref_kind, input_ref_id, job_set_id, 0 "
        "  FROM step_root "
        "  UNION ALL "
        "  SELECT d.workflow_instance_id, d.workflow_step_id, d.root_job_set_id, d.workflow_kind, d.step_key, d.step_kind, d.input_ref_kind, d.input_ref_id, child.job_set_id, d.depth + 1 "
        "  FROM exec_job_set child "
        "  JOIN job_set_descendants d ON child.parent_job_set_id=d.job_set_id "
        "  WHERE d.depth < 64"
        "), "
        "expected_summary AS ("
        "  SELECT d.workflow_instance_id, d.workflow_step_id, d.root_job_set_id, d.workflow_kind, d.step_key, d.step_kind, d.input_ref_kind, d.input_ref_id, "
        "    CASE WHEN COALESCE(SUM(CASE WHEN d.depth > 0 THEN 1 ELSE 0 END), 0) > 0 "
        "      THEN COALESCE(SUM(CASE WHEN d.depth > 0 THEN COALESCE(js.expected_total, 0) ELSE 0 END), 0) "
        "      ELSE COALESCE(MAX(CASE WHEN d.depth=0 THEN js.expected_total ELSE NULL END), 0) END AS expected_total "
        "  FROM job_set_descendants d "
        "  LEFT JOIN exec_job_set js ON js.job_set_id=d.job_set_id "
        "  GROUP BY d.workflow_instance_id, d.workflow_step_id, d.root_job_set_id, d.workflow_kind, d.step_key, d.step_kind, d.input_ref_kind, d.input_ref_id "
        "), "
        "job_summary AS ("
        "  SELECT d.workflow_step_id, "
        "    COUNT(j.job_id) AS discovered_total, "
        "    COALESCE(SUM(CASE WHEN j.state IN ('COMPLETED','SUCCEEDED','SUCCEEDED_WINNER','SUPERSEDED','SUCCEEDED_DUPLICATE','FAILED','CANCELED') THEN 1 ELSE 0 END), 0) AS terminal_total, "
        "    COALESCE(SUM(CASE WHEN j.state='FAILED' THEN 1 ELSE 0 END), 0) AS failed_total "
        "  FROM job_set_descendants d "
        "  LEFT JOIN exec_job j ON j.job_set_id=d.job_set_id "
        "  GROUP BY d.workflow_step_id "
        ") "
        "SELECT e.workflow_instance_id, e.workflow_step_id, e.root_job_set_id, e.workflow_kind, e.step_key, e.step_kind, e.input_ref_kind, e.input_ref_id, "
        "e.expected_total, j.discovered_total, j.terminal_total, j.failed_total "
        "FROM expected_summary e "
        "JOIN job_summary j ON j.workflow_step_id=e.workflow_step_id "
        "WHERE e.expected_total > 0 "
        "  AND j.discovered_total > 0 "
        "  AND e.expected_total = j.discovered_total "
        "  AND j.terminal_total >= j.discovered_total "
        "ORDER BY e.workflow_step_id ASC "
        "LIMIT ?1;",
        &st,
        nullptr)) {
        return rows;
    }

    sqlite3_bind_int64(st.st, 1, static_cast<sqlite3_int64>(limit));
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        auto snapshot = ReadStepTerminalSnapshot(st.st);
        if (snapshot.has_value()) {
            rows.push_back(*snapshot);
        }
    }
    return rows;
}

std::optional<WorkflowGraphSnapshot> SqliteWorkflowOrchestrationQueryService::GetWorkflowGraph(std::int64_t workflow_instance_id) const {
    Statement inst;
    if (!Prepare(db_,
        "SELECT workflow_instance_id, workflow_kind, state, root_scope_kind, root_scope_id, input_ref_kind, input_ref_id, workflow_graph_revision_id "
        "FROM exec_workflow_instance WHERE workflow_instance_id=?1;",
        &inst,
        nullptr)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(inst.st, 1, workflow_instance_id);

    if (sqlite3_step(inst.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    WorkflowGraphSnapshot snapshot;
    snapshot.instance.workflow_instance_id = sqlite3_column_int64(inst.st, 0);
    snapshot.instance.workflow_kind = reinterpret_cast<const char*>(sqlite3_column_text(inst.st, 1));
    snapshot.instance.state = ParseInstanceState(sqlite3_column_text(inst.st, 2));
    snapshot.instance.root_scope_kind = reinterpret_cast<const char*>(sqlite3_column_text(inst.st, 3));
    snapshot.instance.root_scope_id = ColumnInt64Optional(inst.st, 4);
    snapshot.instance.input_ref_kind = ColumnTextOptional(inst.st, 5);
    snapshot.instance.input_ref_id = ColumnInt64Optional(inst.st, 6);
    snapshot.instance.workflow_graph_revision_id = ColumnInt64Optional(inst.st, 7);

    Statement step_st;
    if (!Prepare(db_,
        "SELECT workflow_step_id, workflow_instance_id, step_key, step_kind, state, blocked_reason, job_set_id, priority, attempts, max_attempts "
        "FROM exec_workflow_step WHERE workflow_instance_id=?1 ORDER BY workflow_step_id;",
        &step_st,
        nullptr)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(step_st.st, 1, workflow_instance_id);

    while (sqlite3_step(step_st.st) == SQLITE_ROW) {
        WorkflowStepRecord row;
        row.workflow_step_id = sqlite3_column_int64(step_st.st, 0);
        row.workflow_instance_id = sqlite3_column_int64(step_st.st, 1);
        row.step_key = reinterpret_cast<const char*>(sqlite3_column_text(step_st.st, 2));
        row.step_kind = reinterpret_cast<const char*>(sqlite3_column_text(step_st.st, 3));
        row.state = ParseStepState(sqlite3_column_text(step_st.st, 4));
        row.blocked_reason = ColumnTextOptional(step_st.st, 5);
        row.job_set_id = ColumnInt64Optional(step_st.st, 6);
        row.priority = sqlite3_column_int(step_st.st, 7);
        row.attempts = sqlite3_column_int(step_st.st, 8);
        row.max_attempts = sqlite3_column_int(step_st.st, 9);
        snapshot.steps.push_back(std::move(row));
    }

    Statement edge_st;
    if (!Prepare(db_,
        "SELECT workflow_edge_id, workflow_instance_id, from_step_id, to_step_id, condition_kind, condition_value "
        "FROM exec_workflow_edge WHERE workflow_instance_id=?1 ORDER BY workflow_edge_id;",
        &edge_st,
        nullptr)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(edge_st.st, 1, workflow_instance_id);

    while (sqlite3_step(edge_st.st) == SQLITE_ROW) {
        WorkflowEdgeRecord edge;
        edge.workflow_edge_id = sqlite3_column_int64(edge_st.st, 0);
        edge.workflow_instance_id = sqlite3_column_int64(edge_st.st, 1);
        edge.from_step_id = sqlite3_column_int64(edge_st.st, 2);
        edge.to_step_id = sqlite3_column_int64(edge_st.st, 3);
        edge.condition_kind = ColumnTextOptional(edge_st.st, 4);
        edge.condition_value = ColumnTextOptional(edge_st.st, 5);
        snapshot.edges.push_back(std::move(edge));
    }

    Statement binding_st;
    if (!Prepare(db_,
        "SELECT workflow_instance_input_binding_id, workflow_instance_id, workflow_graph_revision_id, node_key, input_key, data_kind, ref_kind, ref_id, source_kind, created_at_utc "
        "FROM exec_workflow_instance_input_binding WHERE workflow_instance_id=?1 ORDER BY workflow_instance_input_binding_id;",
        &binding_st,
        nullptr)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(binding_st.st, 1, workflow_instance_id);

    while (sqlite3_step(binding_st.st) == SQLITE_ROW) {
        WorkflowInstanceInputBindingRecord binding;
        binding.workflow_instance_input_binding_id = sqlite3_column_int64(binding_st.st, 0);
        binding.workflow_instance_id = sqlite3_column_int64(binding_st.st, 1);
        binding.workflow_graph_revision_id = sqlite3_column_int64(binding_st.st, 2);
        binding.node_key = reinterpret_cast<const char*>(sqlite3_column_text(binding_st.st, 3));
        binding.input_key = reinterpret_cast<const char*>(sqlite3_column_text(binding_st.st, 4));
        binding.data_kind = reinterpret_cast<const char*>(sqlite3_column_text(binding_st.st, 5));
        binding.ref_kind = reinterpret_cast<const char*>(sqlite3_column_text(binding_st.st, 6));
        binding.ref_id = sqlite3_column_int64(binding_st.st, 7);
        binding.source_kind = ColumnTextOptional(binding_st.st, 8).value_or("");
        binding.created_at_utc = sqlite3_column_int64(binding_st.st, 9);
        snapshot.input_bindings.push_back(std::move(binding));
    }

    return snapshot;
}

std::vector<WorkflowStepRecord> SqliteWorkflowOrchestrationQueryService::ListBlockedSteps(std::int64_t workflow_instance_id) const {
    std::vector<WorkflowStepRecord> rows;

    Statement st;
    if (!Prepare(db_,
        "SELECT workflow_step_id, workflow_instance_id, step_key, step_kind, state, blocked_reason, job_set_id, priority, attempts, max_attempts "
        "FROM exec_workflow_step WHERE workflow_instance_id=?1 AND blocked_reason IS NOT NULL "
        "ORDER BY priority DESC, workflow_step_id ASC;",
        &st,
        nullptr)) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, workflow_instance_id);

    while (sqlite3_step(st.st) == SQLITE_ROW) {
        WorkflowStepRecord row;
        row.workflow_step_id = sqlite3_column_int64(st.st, 0);
        row.workflow_instance_id = sqlite3_column_int64(st.st, 1);
        row.step_key = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 2));
        row.step_kind = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 3));
        row.state = ParseStepState(sqlite3_column_text(st.st, 4));
        row.blocked_reason = ColumnTextOptional(st.st, 5);
        row.job_set_id = ColumnInt64Optional(st.st, 6);
        row.priority = sqlite3_column_int(st.st, 7);
        row.attempts = sqlite3_column_int(st.st, 8);
        row.max_attempts = sqlite3_column_int(st.st, 9);
        rows.push_back(std::move(row));
    }

    return rows;
}

std::vector<std::pair<std::int64_t, std::int64_t>>
SqliteWorkflowOrchestrationQueryService::GetStepToJobSetMap(std::int64_t workflow_instance_id) const {
    std::vector<std::pair<std::int64_t, std::int64_t>> pairs;

    Statement st;
    if (!Prepare(db_,
        "SELECT workflow_step_id, job_set_id FROM exec_workflow_step "
        "WHERE workflow_instance_id=?1 AND job_set_id IS NOT NULL ORDER BY workflow_step_id;",
        &st,
        nullptr)) {
        return pairs;
    }
    sqlite3_bind_int64(st.st, 1, workflow_instance_id);

    while (sqlite3_step(st.st) == SQLITE_ROW) {
        pairs.emplace_back(sqlite3_column_int64(st.st, 0), sqlite3_column_int64(st.st, 1));
    }

    return pairs;
}

SqliteWorkflowOrchestrationCommandService::SqliteWorkflowOrchestrationCommandService(sqlite3* db)
    : db_(db) {
}

bool SqliteWorkflowOrchestrationCommandService::CreateWorkflowInstance(
    const WorkflowCreateInstanceCommand& command,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (command.workflow_kind.empty()) {
        if (error_out) *error_out = "workflow_kind is required";
        return false;
    }
    if (command.root_scope_kind.empty()) {
        if (error_out) *error_out = "root_scope_kind is required";
        return false;
    }
    if (command.steps.empty()) {
        if (error_out) *error_out = "at least one workflow step is required";
        return false;
    }
    if (command.workflow_graph_revision_id.has_value() && *command.workflow_graph_revision_id <= 0) {
        if (error_out) *error_out = "workflow_graph_revision_id must be > 0";
        return false;
    }
    if (!command.input_bindings.empty() && !command.workflow_graph_revision_id.has_value()) {
        if (error_out) *error_out = "workflow_graph_revision_id is required when input_bindings are provided";
        return false;
    }
    std::unordered_set<std::string> input_binding_keys;
    input_binding_keys.reserve(command.input_bindings.size());
    for (const auto& binding : command.input_bindings) {
        if (binding.node_key.empty() || binding.input_key.empty()) {
            if (error_out) *error_out = "input binding node_key and input_key are required";
            return false;
        }
        if (binding.data_kind.empty() || binding.ref_kind.empty() || binding.ref_id <= 0) {
            if (error_out) *error_out = "input binding data_kind, ref_kind, and ref_id are required";
            return false;
        }
        const auto key = binding.node_key + "\n" + binding.input_key;
        if (!input_binding_keys.emplace(key).second) {
            if (error_out) *error_out = "duplicate input binding for node/input: " + binding.node_key + "/" + binding.input_key;
            return false;
        }
    }

    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }
    const auto rollback = [&]() { (void)Exec(db_, "ROLLBACK;", nullptr); };

    const auto now = command.created_at_utc > 0 ? command.created_at_utc : NowUtc();
    Statement insert_instance;
    if (!Prepare(db_,
        "INSERT INTO exec_workflow_instance(workflow_kind, state, root_scope_kind, root_scope_id, input_ref_kind, input_ref_id, created_by, created_at_utc, started_at_utc, workflow_graph_revision_id) "
        "VALUES(?1, 'RUNNING', ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);",
        &insert_instance,
        error_out)) {
        rollback();
        return false;
    }
    sqlite3_bind_text(insert_instance.st, 1, command.workflow_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_instance.st, 2, command.root_scope_kind.c_str(), -1, SQLITE_TRANSIENT);
    if (command.root_scope_id.has_value()) sqlite3_bind_int64(insert_instance.st, 3, *command.root_scope_id); else sqlite3_bind_null(insert_instance.st, 3);
    if (command.input_ref_kind.has_value()) sqlite3_bind_text(insert_instance.st, 4, command.input_ref_kind->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(insert_instance.st, 4);
    if (command.input_ref_id.has_value()) sqlite3_bind_int64(insert_instance.st, 5, *command.input_ref_id); else sqlite3_bind_null(insert_instance.st, 5);
    sqlite3_bind_text(insert_instance.st, 6, command.created_by.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_instance.st, 7, now);
    sqlite3_bind_int64(insert_instance.st, 8, now);
    if (command.workflow_graph_revision_id.has_value()) sqlite3_bind_int64(insert_instance.st, 9, *command.workflow_graph_revision_id); else sqlite3_bind_null(insert_instance.st, 9);
    if (sqlite3_step(insert_instance.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }

    const auto workflow_instance_id = sqlite3_last_insert_rowid(db_);
    std::unordered_map<std::string, std::int64_t> step_id_by_key;
    step_id_by_key.reserve(command.steps.size());

    for (const auto& step : command.steps) {
        if (step.step_key.empty() || step.step_kind.empty()) {
            if (error_out) *error_out = "step_key and step_kind are required";
            rollback();
            return false;
        }
        if (!step_id_by_key.emplace(step.step_key, 0).second) {
            if (error_out) *error_out = "duplicate step_key: " + step.step_key;
            rollback();
            return false;
        }
    }

    for (const auto& step : command.steps) {
        Statement insert_step;
        if (!Prepare(db_,
            "INSERT INTO exec_workflow_step(workflow_instance_id, step_key, step_kind, state, guard_kind, guard_value, priority, attempts, max_attempts, input_ref_kind, input_ref_id, created_at_utc, ready_at_utc) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, 0, ?8, ?9, ?10, ?11, ?12);",
            &insert_step,
            error_out)) {
            rollback();
            return false;
        }

        const bool is_ready = step.dependencies.empty();
        sqlite3_bind_int64(insert_step.st, 1, workflow_instance_id);
        sqlite3_bind_text(insert_step.st, 2, step.step_key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_step.st, 3, step.step_kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_step.st, 4, is_ready ? "READY" : "WAITING", -1, SQLITE_STATIC);
        if (step.guard_kind.has_value()) sqlite3_bind_text(insert_step.st, 5, step.guard_kind->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(insert_step.st, 5);
        if (step.guard_value.has_value()) sqlite3_bind_text(insert_step.st, 6, step.guard_value->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(insert_step.st, 6);
        sqlite3_bind_int(insert_step.st, 7, step.priority);
        sqlite3_bind_int(insert_step.st, 8, step.max_attempts > 0 ? step.max_attempts : 1);
        if (step.input_ref_kind.has_value()) sqlite3_bind_text(insert_step.st, 9, step.input_ref_kind->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(insert_step.st, 9);
        if (step.input_ref_id.has_value()) sqlite3_bind_int64(insert_step.st, 10, *step.input_ref_id); else sqlite3_bind_null(insert_step.st, 10);
        sqlite3_bind_int64(insert_step.st, 11, now);
        if (is_ready) sqlite3_bind_int64(insert_step.st, 12, now); else sqlite3_bind_null(insert_step.st, 12);

        if (sqlite3_step(insert_step.st) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            rollback();
            return false;
        }
        step_id_by_key[step.step_key] = sqlite3_last_insert_rowid(db_);
    }

    for (const auto& step : command.steps) {
        for (const auto& dependency_key : step.dependencies) {
            const auto from_it = step_id_by_key.find(dependency_key);
            const auto to_it = step_id_by_key.find(step.step_key);
            if (from_it == step_id_by_key.end() || to_it == step_id_by_key.end()) {
                if (error_out) *error_out = "dependency references unknown step key";
                rollback();
                return false;
            }

            Statement insert_edge;
            if (!Prepare(db_,
                "INSERT INTO exec_workflow_edge(workflow_instance_id, from_step_id, to_step_id, condition_kind, condition_value, created_at_utc) "
                "VALUES(?1, ?2, ?3, NULL, NULL, ?4);",
                &insert_edge,
                error_out)) {
                rollback();
                return false;
            }
            sqlite3_bind_int64(insert_edge.st, 1, workflow_instance_id);
            sqlite3_bind_int64(insert_edge.st, 2, from_it->second);
            sqlite3_bind_int64(insert_edge.st, 3, to_it->second);
            sqlite3_bind_int64(insert_edge.st, 4, now);
            if (sqlite3_step(insert_edge.st) != SQLITE_DONE) {
                if (error_out) *error_out = sqlite3_errmsg(db_);
                rollback();
                return false;
            }
        }
    }

    for (const auto& binding : command.input_bindings) {
        Statement insert_binding;
        if (!Prepare(db_,
            "INSERT INTO exec_workflow_instance_input_binding(workflow_instance_id, workflow_graph_revision_id, node_key, input_key, data_kind, ref_kind, ref_id, source_kind, created_at_utc) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);",
            &insert_binding,
            error_out)) {
            rollback();
            return false;
        }
        sqlite3_bind_int64(insert_binding.st, 1, workflow_instance_id);
        sqlite3_bind_int64(insert_binding.st, 2, *command.workflow_graph_revision_id);
        sqlite3_bind_text(insert_binding.st, 3, binding.node_key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_binding.st, 4, binding.input_key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_binding.st, 5, binding.data_kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_binding.st, 6, binding.ref_kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert_binding.st, 7, binding.ref_id);
        if (!binding.source_kind.empty()) sqlite3_bind_text(insert_binding.st, 8, binding.source_kind.c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(insert_binding.st, 8);
        sqlite3_bind_int64(insert_binding.st, 9, now);
        if (sqlite3_step(insert_binding.st) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            rollback();
            return false;
        }
    }

    if (!EmitLifecycleEvent(workflow_instance_id, std::nullopt, "Execution.WorkflowInstanceCreated.v1", "create", error_out)) {
        rollback();
        return false;
    }

    for (const auto& step : command.steps) {
        if (!step.dependencies.empty()) {
            continue;
        }
        const auto it = step_id_by_key.find(step.step_key);
        if (it == step_id_by_key.end()) {
            continue;
        }
        if (!EmitLifecycleEvent(workflow_instance_id, it->second, "Execution.WorkflowStepReady.v1", "create", error_out)) {
            rollback();
            return false;
        }
    }

    if (!Exec(db_, "COMMIT;", error_out)) {
        rollback();
        return false;
    }

    if (workflow_instance_id_out) *workflow_instance_id_out = workflow_instance_id;
    return true;
}

bool SqliteWorkflowOrchestrationCommandService::RetryFailedStep(const WorkflowRetryStepCommand& command, std::string* error_out) {
    if (command.workflow_step_id <= 0) {
        if (error_out) *error_out = "workflow_step_id must be > 0";
        return false;
    }

    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    Statement st;
    if (!Prepare(db_,
        "UPDATE exec_workflow_step SET state='READY', blocked_reason=NULL, failed_at_utc=NULL, "
        "attempts=attempts+1, ready_at_utc=?2 "
        "WHERE workflow_step_id=?1 AND state='FAILED' AND attempts < max_attempts;",
        &st,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, command.workflow_step_id);
    sqlite3_bind_int64(st.st, 2, NowUtc());

    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        if (error_out) *error_out = "retry precondition failed (step must be FAILED and attempts < max_attempts)";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    Statement lookup;
    if (!Prepare(db_, "SELECT workflow_instance_id FROM exec_workflow_step WHERE workflow_step_id=?1;", &lookup, error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(lookup.st, 1, command.workflow_step_id);
    if (sqlite3_step(lookup.st) != SQLITE_ROW) {
        if (error_out) *error_out = "failed to resolve workflow_instance_id";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    const auto workflow_instance_id = sqlite3_column_int64(lookup.st, 0);

    if (!EmitLifecycleEvent(workflow_instance_id, command.workflow_step_id, "Execution.WorkflowStepReady.v1", "retry", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

bool SqliteWorkflowOrchestrationCommandService::SkipStep(const WorkflowSkipStepCommand& command, std::string* error_out) {
    if (command.workflow_step_id <= 0) {
        if (error_out) *error_out = "workflow_step_id must be > 0";
        return false;
    }

    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    Statement st;
    if (!Prepare(db_,
        "UPDATE exec_workflow_step SET state='SKIPPED', blocked_reason=?2, completed_at_utc=?3 "
        "WHERE workflow_step_id=?1 AND state IN ('WAITING','READY','FAILED');",
        &st,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, command.workflow_step_id);
    sqlite3_bind_text(st.st, 2, command.reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 3, NowUtc());

    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        if (error_out) *error_out = "skip precondition failed";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    Statement lookup;
    if (!Prepare(db_, "SELECT workflow_instance_id FROM exec_workflow_step WHERE workflow_step_id=?1;", &lookup, error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(lookup.st, 1, command.workflow_step_id);
    if (sqlite3_step(lookup.st) != SQLITE_ROW) {
        if (error_out) *error_out = "failed to resolve workflow_instance_id";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    const auto workflow_instance_id = sqlite3_column_int64(lookup.st, 0);

    if (!EmitLifecycleEvent(workflow_instance_id, command.workflow_step_id, "Execution.WorkflowStepCompleted.v1", "skip", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

bool SqliteWorkflowOrchestrationCommandService::CancelWorkflowInstance(
    const WorkflowCancelInstanceCommand& command,
    std::string* error_out) {
    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    Statement st;
    if (!Prepare(db_,
        "UPDATE exec_workflow_instance SET state='CANCELED', failure_text=?2, completed_at_utc=?3 "
        "WHERE workflow_instance_id=?1 AND state IN ('PENDING','RUNNING','FAILED');",
        &st,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, command.workflow_instance_id);
    sqlite3_bind_text(st.st, 2, command.reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 3, NowUtc());

    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        if (error_out) *error_out = "cancel precondition failed";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!EmitLifecycleEvent(command.workflow_instance_id, std::nullopt, "Execution.WorkflowInstanceCompleted.v1", "cancel", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

bool SqliteWorkflowOrchestrationCommandService::ResumeWorkflowInstance(
    const WorkflowResumeInstanceCommand& command,
    std::string* error_out) {
    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    Statement st;
    if (!Prepare(db_,
        "UPDATE exec_workflow_instance SET state='RUNNING', failure_text=NULL, started_at_utc=COALESCE(started_at_utc, ?2) "
        "WHERE workflow_instance_id=?1 AND state IN ('FAILED','CANCELED');",
        &st,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, command.workflow_instance_id);
    sqlite3_bind_int64(st.st, 2, NowUtc());

    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        if (error_out) *error_out = "resume precondition failed";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!EmitLifecycleEvent(command.workflow_instance_id, std::nullopt, "Execution.WorkflowInstanceCreated.v1", "resume", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

bool SqliteWorkflowOrchestrationCommandService::CompleteWorkflowInstance(
    const WorkflowCompleteInstanceCommand& command,
    std::string* error_out) {
    if (command.workflow_instance_id <= 0) {
        if (error_out) *error_out = "workflow_instance_id must be > 0";
        return false;
    }

    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    Statement current;
    if (!Prepare(db_,
        "SELECT state FROM exec_workflow_instance WHERE workflow_instance_id=?1;",
        &current,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(current.st, 1, command.workflow_instance_id);
    if (sqlite3_step(current.st) != SQLITE_ROW) {
        if (error_out) *error_out = "workflow instance not found";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    const std::string current_state = reinterpret_cast<const char*>(sqlite3_column_text(current.st, 0));
    if (current_state == "COMPLETED") {
        if (!Exec(db_, "COMMIT;", error_out)) {
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
        return true;
    }
    if (current_state == "FAILED" || current_state == "CANCELED") {
        if (error_out) *error_out = "complete precondition failed (instance already terminal)";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    Statement st;
    if (!Prepare(db_,
        "UPDATE exec_workflow_instance "
        "SET state='COMPLETED', completed_at_utc=?2 "
        "WHERE workflow_instance_id=?1 "
        "  AND state IN ('PENDING','RUNNING') "
        "  AND NOT EXISTS ("
        "    SELECT 1 FROM exec_workflow_step "
        "    WHERE workflow_instance_id=?1 "
        "      AND state NOT IN ('COMPLETED','FAILED','SKIPPED')"
        "  );",
        &st,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, command.workflow_instance_id);
    sqlite3_bind_int64(st.st, 2, NowUtc());

    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        if (error_out) *error_out = "complete precondition failed";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!EmitLifecycleEvent(
            command.workflow_instance_id,
            std::nullopt,
            "Execution.WorkflowInstanceCompleted.v1",
            "completed",
            error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

bool SqliteWorkflowOrchestrationCommandService::PauseWorkflowInstance(
    const WorkflowPauseInstanceCommand& command,
    std::string* error_out) {
    if (command.workflow_instance_id <= 0) {
        if (error_out) *error_out = "workflow_instance_id must be > 0";
        return false;
    }

    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    const std::string failure_code = command.failure_code.empty()
        ? "WORKFLOW_INVARIANT_VIOLATION"
        : command.failure_code;
    const std::string reason = command.reason.empty()
        ? "workflow_paused_for_invariant_violation"
        : command.reason;

    Statement st;
    if (!Prepare(db_,
        "UPDATE exec_workflow_instance "
        "SET state='FAILED', failure_code=?2, failure_text=?3, completed_at_utc=NULL "
        "WHERE workflow_instance_id=?1 AND state='RUNNING';",
        &st,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, command.workflow_instance_id);
    sqlite3_bind_text(st.st, 2, failure_code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 3, reason.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        if (error_out) *error_out = "pause precondition failed (instance must be RUNNING)";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!EmitLifecycleEvent(
            command.workflow_instance_id,
            std::nullopt,
            "Execution.WorkflowInvariantViolation.v1",
            reason.c_str(),
            error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

bool SqliteWorkflowOrchestrationCommandService::TerminalFailWorkflowInstance(
    const WorkflowTerminalFailInstanceCommand& command,
    std::string* error_out) {
    if (command.workflow_instance_id <= 0) {
        if (error_out) *error_out = "workflow_instance_id must be > 0";
        return false;
    }
    if (command.failure_code.empty()) {
        if (error_out) *error_out = "failure_code is required";
        return false;
    }

    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    Statement st;
    if (!Prepare(db_,
        "UPDATE exec_workflow_instance "
        "SET state='FAILED', failure_code=?2, failure_text=?3, completed_at_utc=?4 "
        "WHERE workflow_instance_id=?1 AND state IN ('PENDING','RUNNING','FAILED');",
        &st,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    const auto now = NowUtc();
    sqlite3_bind_int64(st.st, 1, command.workflow_instance_id);
    sqlite3_bind_text(st.st, 2, command.failure_code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 3, command.failure_message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 4, now);

    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        if (error_out) *error_out = "terminal fail precondition failed";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!EmitLifecycleEvent(
            command.workflow_instance_id,
            std::nullopt,
            "Execution.WorkflowRemediationTerminalFailed.v1",
            command.failure_message.c_str(),
            error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!EmitLifecycleEvent(
            command.workflow_instance_id,
            std::nullopt,
            "Execution.WorkflowInstanceCompleted.v1",
            "terminal_failed",
            error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

bool SqliteWorkflowOrchestrationCommandService::MarkStepMaterialized(
    const WorkflowMarkStepMaterializedCommand& command,
    std::string* error_out) {
    if (command.workflow_step_id <= 0) {
        if (error_out) *error_out = "workflow_step_id must be > 0";
        return false;
    }
    if (command.job_set_id <= 0) {
        if (error_out) *error_out = "job_set_id must be > 0";
        return false;
    }

    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    Statement read;
    if (!Prepare(db_,
        "SELECT workflow_instance_id, state, job_set_id "
        "FROM exec_workflow_step WHERE workflow_step_id=?1;",
        &read,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(read.st, 1, command.workflow_step_id);
    if (sqlite3_step(read.st) != SQLITE_ROW) {
        if (error_out) *error_out = "workflow step not found";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    const auto workflow_instance_id = sqlite3_column_int64(read.st, 0);
    const std::string state = reinterpret_cast<const char*>(sqlite3_column_text(read.st, 1));
    const auto existing_job_set_id = ColumnInt64Optional(read.st, 2);

    if (existing_job_set_id.has_value() && *existing_job_set_id != command.job_set_id) {
        if (error_out) *error_out = "materialize precondition failed (step already mapped to a different job_set_id)";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    if (state == "COMPLETED" || state == "FAILED" || state == "SKIPPED") {
        if (error_out) *error_out = "materialize precondition failed (step is terminal)";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    bool should_emit = false;
    if (state != "MATERIALIZED" || !existing_job_set_id.has_value()) {
        Statement update;
        if (!Prepare(db_,
            "UPDATE exec_workflow_step "
            "SET state='MATERIALIZED', job_set_id=?2, started_at_utc=COALESCE(started_at_utc, ?3) "
            "WHERE workflow_step_id=?1 AND state IN ('READY','MATERIALIZED','RUNNING');",
            &update,
            error_out)) {
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
        sqlite3_bind_int64(update.st, 1, command.workflow_step_id);
        sqlite3_bind_int64(update.st, 2, command.job_set_id);
        sqlite3_bind_int64(update.st, 3, NowUtc());
        if (sqlite3_step(update.st) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
        if (sqlite3_changes(db_) == 0) {
            if (error_out) *error_out = "materialize precondition failed";
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
        should_emit = true;
    }

    if (should_emit
        && !EmitLifecycleEvent(workflow_instance_id, command.workflow_step_id, "Execution.WorkflowStepMaterialized.v1", "materialized", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

bool SqliteWorkflowOrchestrationCommandService::MarkStepTerminal(
    const WorkflowMarkStepTerminalCommand& command,
    std::string* error_out) {
    if (command.workflow_step_id <= 0) {
        if (error_out) *error_out = "workflow_step_id must be > 0";
        return false;
    }

    const bool completed = command.terminal_state == "COMPLETED";
    const bool failed = command.terminal_state == "FAILED";
    if (!completed && !failed) {
        if (error_out) *error_out = "terminal_state must be COMPLETED or FAILED";
        return false;
    }

    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    Statement read;
    if (!Prepare(db_,
        "SELECT workflow_instance_id, state "
        "FROM exec_workflow_step WHERE workflow_step_id=?1;",
        &read,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(read.st, 1, command.workflow_step_id);
    if (sqlite3_step(read.st) != SQLITE_ROW) {
        if (error_out) *error_out = "workflow step not found";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    const auto workflow_instance_id = sqlite3_column_int64(read.st, 0);
    const std::string state = reinterpret_cast<const char*>(sqlite3_column_text(read.st, 1));
    const std::string target_state = completed ? "COMPLETED" : "FAILED";

    if (state == target_state) {
        if (!Exec(db_, "COMMIT;", error_out)) {
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
        return true;
    }
    if (state == "COMPLETED" || state == "FAILED" || state == "SKIPPED") {
        if (error_out) *error_out = "terminal precondition failed (step already terminal with different outcome)";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    Statement update;
    if (!Prepare(db_,
        "UPDATE exec_workflow_step "
        "SET state=?2, "
        "completed_at_utc=CASE WHEN ?2='COMPLETED' THEN ?3 ELSE completed_at_utc END, "
        "failed_at_utc=CASE WHEN ?2='FAILED' THEN ?3 ELSE failed_at_utc END "
        "WHERE workflow_step_id=?1 AND state IN ('MATERIALIZED','RUNNING','READY');",
        &update,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(update.st, 1, command.workflow_step_id);
    sqlite3_bind_text(update.st, 2, target_state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update.st, 3, NowUtc());

    if (sqlite3_step(update.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        if (error_out) *error_out = "terminal precondition failed";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    const char* event_kind = completed ? "Execution.WorkflowStepCompleted.v1" : "Execution.WorkflowStepFailed.v1";
    const char* message = completed ? "terminal_completed" : "terminal_failed";
    if (!EmitLifecycleEvent(workflow_instance_id, command.workflow_step_id, event_kind, message, error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

bool SqliteWorkflowOrchestrationCommandService::MarkStepBlocked(
    const WorkflowMarkStepBlockedCommand& command,
    std::string* error_out) {
    if (command.workflow_step_id <= 0) {
        if (error_out) *error_out = "workflow_step_id must be > 0";
        return false;
    }

    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    Statement read;
    if (!Prepare(db_,
        "SELECT workflow_instance_id FROM exec_workflow_step WHERE workflow_step_id=?1;",
        &read,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(read.st, 1, command.workflow_step_id);
    if (sqlite3_step(read.st) != SQLITE_ROW) {
        if (error_out) *error_out = "workflow step not found";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    const auto workflow_instance_id = sqlite3_column_int64(read.st, 0);

    Statement update;
    if (!Prepare(db_,
        "UPDATE exec_workflow_step "
        "SET blocked_reason=?2 "
        "WHERE workflow_step_id=?1;",
        &update,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(update.st, 1, command.workflow_step_id);
    if (command.blocked_reason.has_value()) {
        sqlite3_bind_text(update.st, 2, command.blocked_reason->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(update.st, 2);
    }

    if (sqlite3_step(update.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        if (error_out) *error_out = "blocked_reason update precondition failed";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (command.blocked_reason.has_value()) {
        if (!EmitLifecycleEvent(
            workflow_instance_id,
            command.workflow_step_id,
            "Execution.WorkflowStepBlocked.v1",
            command.blocked_reason->c_str(),
            error_out)) {
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
    }

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

bool SqliteWorkflowOrchestrationCommandService::MarkStepReady(
    const WorkflowMarkStepReadyCommand& command,
    std::string* error_out) {
    if (command.workflow_instance_id <= 0) {
        if (error_out) *error_out = "workflow_instance_id must be > 0";
        return false;
    }
    if (command.step_key.empty()) {
        if (error_out) *error_out = "step_key is required";
        return false;
    }

    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    Statement read;
    if (!Prepare(db_,
        "SELECT workflow_step_id, state, job_set_id "
        "FROM exec_workflow_step WHERE workflow_instance_id=?1 AND step_key=?2;",
        &read,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(read.st, 1, command.workflow_instance_id);
    sqlite3_bind_text(read.st, 2, command.step_key.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(read.st) != SQLITE_ROW) {
        if (error_out) *error_out = "workflow step not found";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    const auto workflow_step_id = sqlite3_column_int64(read.st, 0);
    const std::string state = reinterpret_cast<const char*>(sqlite3_column_text(read.st, 1));
    const auto existing_job_set_id = ColumnInt64Optional(read.st, 2);

    if (state == "READY" && !existing_job_set_id.has_value()) {
        if (!Exec(db_, "COMMIT;", error_out)) {
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
        return true;
    }
    if (state != "WAITING" || existing_job_set_id.has_value()) {
        if (error_out) *error_out = "ready precondition failed";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    Statement update;
    if (!Prepare(db_,
        "UPDATE exec_workflow_step "
        "SET state='READY', ready_at_utc=?3, blocked_reason=NULL "
        "WHERE workflow_instance_id=?1 AND step_key=?2 AND state='WAITING' AND job_set_id IS NULL;",
        &update,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(update.st, 1, command.workflow_instance_id);
    sqlite3_bind_text(update.st, 2, command.step_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update.st, 3, NowUtc());
    if (sqlite3_step(update.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        if (error_out) *error_out = "ready precondition failed";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!EmitLifecycleEvent(
        command.workflow_instance_id,
        workflow_step_id,
        "Execution.WorkflowStepReady.v1",
        "ready",
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

bool SqliteWorkflowOrchestrationCommandService::AppendDynamicSteps(
    const WorkflowAppendDynamicStepsCommand& command,
    std::string* error_out) {
    if (command.workflow_instance_id <= 0) {
        if (error_out) *error_out = "workflow_instance_id must be > 0";
        return false;
    }
    if (command.steps.empty()) {
        return true;
    }
    for (const auto& step : command.steps) {
        if (step.step_key.empty() || step.step_kind.empty()) {
            if (error_out) *error_out = "dynamic step_key and step_kind are required";
            return false;
        }
        if (step.max_attempts <= 0) {
            if (error_out) *error_out = "dynamic max_attempts must be > 0";
            return false;
        }
    }

    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    Statement instance;
    if (!Prepare(db_,
        "SELECT state FROM exec_workflow_instance WHERE workflow_instance_id=?1;",
        &instance,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    sqlite3_bind_int64(instance.st, 1, command.workflow_instance_id);
    if (sqlite3_step(instance.st) != SQLITE_ROW) {
        if (error_out) *error_out = "workflow instance not found";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    const std::string instance_state = reinterpret_cast<const char*>(sqlite3_column_text(instance.st, 0));
    if (instance_state != "RUNNING" && instance_state != "PENDING") {
        if (error_out) *error_out = "append dynamic step precondition failed (workflow instance is terminal)";
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (command.parent_workflow_step_id.has_value()) {
        Statement parent;
        if (!Prepare(db_,
            "SELECT workflow_step_id FROM exec_workflow_step WHERE workflow_instance_id=?1 AND workflow_step_id=?2;",
            &parent,
            error_out)) {
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
        sqlite3_bind_int64(parent.st, 1, command.workflow_instance_id);
        sqlite3_bind_int64(parent.st, 2, *command.parent_workflow_step_id);
        if (sqlite3_step(parent.st) != SQLITE_ROW) {
            if (error_out) *error_out = "parent workflow step not found for instance";
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
    }

    const auto now = NowUtc();
    std::vector<std::int64_t> newly_inserted_step_ids;
    newly_inserted_step_ids.reserve(command.steps.size());
    for (const auto& step : command.steps) {
        std::optional<std::int64_t> workflow_step_id;
        Statement existing;
        if (!Prepare(db_,
            "SELECT workflow_step_id, step_kind, input_ref_kind, input_ref_id "
            "FROM exec_workflow_step WHERE workflow_instance_id=?1 AND step_key=?2;",
            &existing,
            error_out)) {
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
        sqlite3_bind_int64(existing.st, 1, command.workflow_instance_id);
        sqlite3_bind_text(existing.st, 2, step.step_key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(existing.st) == SQLITE_ROW) {
            const std::string existing_kind = reinterpret_cast<const char*>(sqlite3_column_text(existing.st, 1));
            const auto existing_input_kind = ColumnTextOptional(existing.st, 2);
            const auto existing_input_id = ColumnInt64Optional(existing.st, 3);
            if (existing_kind != step.step_kind
                || existing_input_kind != step.input_ref_kind
                || existing_input_id != step.input_ref_id) {
                if (error_out) *error_out = "dynamic step key already exists with incompatible shape";
                Exec(db_, "ROLLBACK;", nullptr);
                return false;
            }
            workflow_step_id = sqlite3_column_int64(existing.st, 0);
        } else {
            Statement insert_step;
            if (!Prepare(db_,
                "INSERT INTO exec_workflow_step("
                "workflow_instance_id, step_key, step_kind, state, guard_kind, guard_value, priority, attempts, max_attempts, input_ref_kind, input_ref_id, created_at_utc, ready_at_utc) "
                "VALUES(?1, ?2, ?3, 'READY', ?4, ?5, ?6, 0, ?7, ?8, ?9, ?10, ?10);",
                &insert_step,
                error_out)) {
                Exec(db_, "ROLLBACK;", nullptr);
                return false;
            }
            sqlite3_bind_int64(insert_step.st, 1, command.workflow_instance_id);
            sqlite3_bind_text(insert_step.st, 2, step.step_key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_step.st, 3, step.step_kind.c_str(), -1, SQLITE_TRANSIENT);
            if (step.guard_kind.has_value()) sqlite3_bind_text(insert_step.st, 4, step.guard_kind->c_str(), -1, SQLITE_TRANSIENT);
            else sqlite3_bind_null(insert_step.st, 4);
            if (step.guard_value.has_value()) sqlite3_bind_text(insert_step.st, 5, step.guard_value->c_str(), -1, SQLITE_TRANSIENT);
            else sqlite3_bind_null(insert_step.st, 5);
            sqlite3_bind_int(insert_step.st, 6, step.priority);
            sqlite3_bind_int(insert_step.st, 7, step.max_attempts);
            if (step.input_ref_kind.has_value()) sqlite3_bind_text(insert_step.st, 8, step.input_ref_kind->c_str(), -1, SQLITE_TRANSIENT);
            else sqlite3_bind_null(insert_step.st, 8);
            if (step.input_ref_id.has_value()) sqlite3_bind_int64(insert_step.st, 9, *step.input_ref_id);
            else sqlite3_bind_null(insert_step.st, 9);
            sqlite3_bind_int64(insert_step.st, 10, now);

            if (sqlite3_step(insert_step.st) != SQLITE_DONE) {
                if (error_out) *error_out = sqlite3_errmsg(db_);
                Exec(db_, "ROLLBACK;", nullptr);
                return false;
            }
            workflow_step_id = sqlite3_last_insert_rowid(db_);
            newly_inserted_step_ids.push_back(*workflow_step_id);
        }

        if (command.parent_workflow_step_id.has_value() && workflow_step_id.has_value()) {
            Statement edge;
            if (!Prepare(db_,
                "INSERT OR IGNORE INTO exec_workflow_edge(workflow_instance_id, from_step_id, to_step_id, condition_kind, condition_value, created_at_utc) "
                "VALUES(?1, ?2, ?3, NULL, NULL, ?4);",
                &edge,
                error_out)) {
                Exec(db_, "ROLLBACK;", nullptr);
                return false;
            }
            sqlite3_bind_int64(edge.st, 1, command.workflow_instance_id);
            sqlite3_bind_int64(edge.st, 2, *command.parent_workflow_step_id);
            sqlite3_bind_int64(edge.st, 3, *workflow_step_id);
            sqlite3_bind_int64(edge.st, 4, now);
            if (sqlite3_step(edge.st) != SQLITE_DONE) {
                if (error_out) *error_out = sqlite3_errmsg(db_);
                Exec(db_, "ROLLBACK;", nullptr);
                return false;
            }
        }
    }

    for (const auto workflow_step_id : newly_inserted_step_ids) {
        if (!EmitLifecycleEvent(
                command.workflow_instance_id,
                workflow_step_id,
                "Execution.WorkflowStepReady.v1",
                "dynamic_spawn",
                error_out)) {
            Exec(db_, "ROLLBACK;", nullptr);
            return false;
        }
    }

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

bool SqliteWorkflowOrchestrationCommandService::AppendStepInputEvent(
    const WorkflowAppendStepInputEventCommand& command,
    std::string* error_out) {
    if (command.workflow_instance_id <= 0 || command.workflow_step_id <= 0) {
        if (error_out) *error_out = "workflow_instance_id and workflow_step_id must be > 0";
        return false;
    }
    if (command.event_kind.empty()) {
        if (error_out) *error_out = "event_kind is required";
        return false;
    }

    bool known = false;
    for (const auto catalog_value : events::kWorkflowInputEventsV1) {
        if (catalog_value == command.event_kind) {
            known = true;
            break;
        }
    }
    if (!known) {
        if (error_out) *error_out = std::string("unknown workflow input event kind: ") + command.event_kind;
        return false;
    }

    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    Statement workflow_input_event;
    if (!Prepare(db_,
        "INSERT INTO exec_workflow_input_event("
        "workflow_instance_id, workflow_step_id, event_kind, source_key, request_id, event_ts_utc, message"
        ") VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7);",
        &workflow_input_event,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    const auto ts = NowUtc();
    sqlite3_bind_int64(workflow_input_event.st, 1, command.workflow_instance_id);
    sqlite3_bind_int64(workflow_input_event.st, 2, command.workflow_step_id);
    sqlite3_bind_text(workflow_input_event.st, 3, command.event_kind.c_str(), -1, SQLITE_TRANSIENT);
    if (command.source_key.has_value()) {
        sqlite3_bind_text(workflow_input_event.st, 4, command.source_key->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(workflow_input_event.st, 4);
    }
    if (command.request_id.has_value()) {
        sqlite3_bind_text(workflow_input_event.st, 5, command.request_id->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(workflow_input_event.st, 5);
    }
    sqlite3_bind_int64(workflow_input_event.st, 6, ts);
    if (command.message.has_value()) {
        sqlite3_bind_text(workflow_input_event.st, 7, command.message->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(workflow_input_event.st, 7);
    }

    if (sqlite3_step(workflow_input_event.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    const auto workflow_input_event_id = sqlite3_last_insert_rowid(db_);
    const auto step_aggregate_id = std::to_string(command.workflow_step_id);
    const auto instance_aggregate_id = std::to_string(command.workflow_instance_id);
    const auto correlation_id = "workflow-instance-" + instance_aggregate_id;
    const auto causation_id = "workflow-step-" + step_aggregate_id;

    Statement outbox;
    if (!Prepare(db_,
        "INSERT INTO exec_outbox_message("
        "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
        "VALUES(?1, ?2, 1, 'Execution', 'workflow_step', ?3, ?4, ?5, ?6, 'workflow_input_event', ?7);",
        &outbox,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    std::ostringstream event_id;
    event_id << "workflow-input-" << command.workflow_step_id << "-" << workflow_input_event_id;
    const auto event_id_value = event_id.str();
    sqlite3_bind_text(outbox.st, 1, event_id_value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outbox.st, 2, command.event_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outbox.st, 3, step_aggregate_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outbox.st, 4, correlation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outbox.st, 5, causation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(outbox.st, 6, ts);
    sqlite3_bind_int64(outbox.st, 7, workflow_input_event_id);
    if (sqlite3_step(outbox.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

bool SqliteWorkflowOrchestrationCommandService::AppendLifecycleEvent(
    const WorkflowAppendLifecycleEventCommand& command,
    std::string* error_out) {
    if (command.workflow_instance_id <= 0) {
        if (error_out) *error_out = "workflow_instance_id must be > 0";
        return false;
    }
    if (command.event_kind.empty()) {
        if (error_out) *error_out = "event_kind is required";
        return false;
    }

    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    const char* message = command.message.has_value() ? command.message->c_str() : "";
    if (!EmitLifecycleEvent(
        command.workflow_instance_id,
        command.workflow_step_id,
        command.event_kind.c_str(),
        message,
        error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }

    if (!Exec(db_, "COMMIT;", error_out)) {
        Exec(db_, "ROLLBACK;", nullptr);
        return false;
    }
    return true;
}

bool SqliteWorkflowOrchestrationCommandService::EmitLifecycleEvent(
    std::int64_t workflow_instance_id,
    std::optional<std::int64_t> workflow_step_id,
    const char* event_kind,
    const char* message,
    std::string* error_out) {
    bool known = false;
    for (const auto catalog_value : events::kWorkflowExecutionEventsV1) {
        if (catalog_value == event_kind) {
            known = true;
            break;
        }
    }
    if (!known) {
        if (error_out) *error_out = std::string("unknown workflow event kind: ") + event_kind;
        return false;
    }

    Statement workflow_event;
    if (!Prepare(db_,
        "INSERT INTO exec_workflow_event(workflow_instance_id, workflow_step_id, event_kind, event_ts_utc, message) "
        "VALUES(?1, ?2, ?3, ?4, ?5);",
        &workflow_event,
        error_out)) {
        return false;
    }

    const auto ts = NowUtc();
    sqlite3_bind_int64(workflow_event.st, 1, workflow_instance_id);
    if (workflow_step_id.has_value()) {
        sqlite3_bind_int64(workflow_event.st, 2, *workflow_step_id);
    } else {
        sqlite3_bind_null(workflow_event.st, 2);
    }
    sqlite3_bind_text(workflow_event.st, 3, event_kind, -1, SQLITE_STATIC);
    sqlite3_bind_int64(workflow_event.st, 4, ts);
    sqlite3_bind_text(workflow_event.st, 5, message, -1, SQLITE_STATIC);

    if (sqlite3_step(workflow_event.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    const auto workflow_event_id = sqlite3_last_insert_rowid(db_);
    std::ostringstream event_id;
    event_id << "workflow-" << workflow_instance_id << "-" << workflow_event_id;
    const auto event_id_value = event_id.str();
    const auto aggregate_id = std::to_string(workflow_instance_id);
    const auto correlation_id = "workflow-instance-" + aggregate_id;
    std::string causation_id = event_id_value;
    if (workflow_step_id.has_value()) {
        causation_id = "workflow-step-" + std::to_string(*workflow_step_id);
    }

    Statement outbox;
    if (!Prepare(db_,
        "INSERT INTO exec_outbox_message("
        "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
        "VALUES(?1, ?2, 1, 'Execution', 'workflow_instance', ?3, ?4, ?5, ?6, 'workflow_event', ?7);",
        &outbox,
        error_out)) {
        return false;
    }

    sqlite3_bind_text(outbox.st, 1, event_id_value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outbox.st, 2, event_kind, -1, SQLITE_STATIC);
    sqlite3_bind_text(outbox.st, 3, aggregate_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outbox.st, 4, correlation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outbox.st, 5, causation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(outbox.st, 6, ts);
    sqlite3_bind_int64(outbox.st, 7, workflow_event_id);

    if (sqlite3_step(outbox.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    return true;
}

} // namespace simcore::db::execution::workflow
