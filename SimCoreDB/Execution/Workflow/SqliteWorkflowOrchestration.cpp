#include "SqliteWorkflowOrchestration.h"

#include <chrono>
#include <sstream>
#include <unordered_map>

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
        "SELECT workflow_instance_id, workflow_kind, state, root_scope_kind, root_scope_id, input_ref_kind, input_ref_id "
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
        "SELECT s.workflow_instance_id, s.workflow_step_id, s.step_key, s.step_kind, s.priority "
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
        rows.push_back(std::move(row));
    }

    return rows;
}

std::optional<WorkflowGraphSnapshot> SqliteWorkflowOrchestrationQueryService::GetWorkflowGraph(std::int64_t workflow_instance_id) const {
    Statement inst;
    if (!Prepare(db_,
        "SELECT workflow_instance_id, workflow_kind, state, root_scope_kind, root_scope_id, input_ref_kind, input_ref_id "
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

    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }
    const auto rollback = [&]() { (void)Exec(db_, "ROLLBACK;", nullptr); };

    const auto now = command.created_at_utc > 0 ? command.created_at_utc : NowUtc();
    Statement insert_instance;
    if (!Prepare(db_,
        "INSERT INTO exec_workflow_instance(workflow_kind, state, root_scope_kind, root_scope_id, input_ref_kind, input_ref_id, created_by, created_at_utc, started_at_utc) "
        "VALUES(?1, 'RUNNING', ?2, ?3, ?4, ?5, ?6, ?7, ?8);",
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
            "INSERT INTO exec_workflow_step(workflow_instance_id, step_key, step_kind, state, guard_kind, guard_value, priority, attempts, max_attempts, input_ref_kind, created_at_utc, ready_at_utc) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, 0, ?8, ?9, ?10, ?11);",
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
        sqlite3_bind_int64(insert_step.st, 10, now);
        if (is_ready) sqlite3_bind_int64(insert_step.st, 11, now); else sqlite3_bind_null(insert_step.st, 11);

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
