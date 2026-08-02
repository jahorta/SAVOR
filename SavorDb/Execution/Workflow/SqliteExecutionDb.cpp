#include "SqliteExecutionDb.h"

#include "../../Common/Events/EventPayloadDispatch.h"
#include "../../Common/Events/EventPayloadValidation.h"
#include "SqliteWorkflowOrchestration.h"
#include "WorkflowRecoveryService.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

namespace savor::db::execution::workflow {

std::optional<std::string> OptionalText(sqlite3_stmt* st, int index);
std::string Text(sqlite3_stmt* st, int index);
void BindOptionalText(
    sqlite3_stmt* st,
    int index,
    const std::optional<std::string>& value);
bool IsSha256(std::string_view value);
bool IsSafeRelativeBlobPath(std::string_view value);
bool InsertAggregateOutboxEvent(
    sqlite3* db,
    const char* event_type,
    const char* aggregate_kind,
    std::int64_t aggregate_id_value,
    const char* payload_ref_kind,
    std::int64_t payload_ref_id,
    std::string_view causation_suffix,
    std::string* error_out);
namespace {

thread_local sqlite3* g_batch_transaction_db = nullptr;
thread_local bool g_batch_refresh_requested = false;

bool RefreshExecutionWorkAvailability(
    sqlite3* db,
    std::string* error_out,
    bool force = false);

struct Statement {
    sqlite3_stmt* st = nullptr;
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }
};


std::int64_t CurrentUtcMs(sqlite3* db) {
    Statement st;
    if (sqlite3_prepare_v2(db,
        "SELECT CAST(unixepoch('now') * 1000 AS INTEGER);",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return 0;
    }
    return sqlite3_column_int64(st.st, 0);
}


std::int64_t NowUtcMillis() {
    return types::UtcNow().time_since_epoch().count();
}

bool ExecuteSql(sqlite3* db, const char* sql, std::string* error_out) {
    if (db == g_batch_transaction_db
        && (std::string_view(sql) == "BEGIN IMMEDIATE;"
            || std::string_view(sql) == "COMMIT;")) {
        return true;
    }
    char* sqlite_error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &sqlite_error);
    if (rc != SQLITE_OK) {
        if (error_out) {
            *error_out = sqlite_error ? sqlite_error : sqlite3_errmsg(db);
        }
        sqlite3_free(sqlite_error);
        return false;
    }
    sqlite3_free(sqlite_error);
    return true;
}

bool Commit(sqlite3* db, std::string* error_out) {
    return ExecuteSql(db, "COMMIT;", error_out);
}

void Rollback(sqlite3* db) {
    if (db == g_batch_transaction_db) {
        return;
    }
    (void)sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
}

class BatchTransactionScope {
public:
    explicit BatchTransactionScope(sqlite3* db) : db_(db) {}

    bool Begin(std::string* error_out) {
        if (db_ == nullptr || g_batch_transaction_db != nullptr) {
            if (error_out != nullptr) {
                *error_out = "invalid nested execution batch transaction";
            }
            return false;
        }
        if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
            return false;
        }
        g_batch_transaction_db = db_;
        g_batch_refresh_requested = false;
        active_ = true;
        return true;
    }

    bool CommitAll(std::string* error_out) {
        if (!active_) {
            if (error_out != nullptr) {
                *error_out = "execution batch transaction cannot commit";
            }
            return false;
        }
        if (g_batch_refresh_requested
            && !RefreshExecutionWorkAvailability(db_, error_out, true)) {
            RollbackAll();
            return false;
        }
        g_batch_refresh_requested = false;
        g_batch_transaction_db = nullptr;
        if (!Commit(db_, error_out)) {
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            active_ = false;
            return false;
        }
        active_ = false;
        return true;
    }

    void RollbackAll() noexcept {
        if (!active_) return;
        g_batch_refresh_requested = false;
        g_batch_transaction_db = nullptr;
        active_ = false;
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }

    ~BatchTransactionScope() { RollbackAll(); }

private:
    sqlite3* db_ = nullptr;
    bool active_ = false;
};

bool InsertJobActionEventAndOutbox(
    sqlite3* db,
    std::int64_t job_id,
    std::int64_t job_set_id,
    const char* event_type,
    const char* message,
    std::string* error_out,
    std::int64_t* job_event_id_out = nullptr) {
    const auto now = CurrentUtcMs(db);
    Statement job_event;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO exec_job_event(job_id,event_kind,event_ts_utc,message,artifact_id) "
            "VALUES(?1,?2,?3,?4,NULL);",
            -1,
            &job_event.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_int64(job_event.st, 1, job_id);
    sqlite3_bind_text(job_event.st, 2, event_type, -1, SQLITE_STATIC);
    sqlite3_bind_int64(job_event.st, 3, now);
    sqlite3_bind_text(job_event.st, 4, message, -1, SQLITE_STATIC);
    if (sqlite3_step(job_event.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    const auto job_event_id = sqlite3_last_insert_rowid(db);
    if (job_event_id_out != nullptr) {
        *job_event_id_out = job_event_id;
    }

    std::ostringstream event_id;
    event_id << "execution-" << event_type << "-" << job_id << "-" << job_event_id;
    const auto event_id_value = event_id.str();
    const auto aggregate_id = std::to_string(job_id);
    const auto correlation_id = "job-set-" + std::to_string(job_set_id);
    const auto causation_id = std::string("job-action-") + message + "-" + std::to_string(job_id);

    Statement outbox;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO exec_outbox_message("
            "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
            "VALUES(?1,?2,1,'Execution','job',?3,?4,?5,?6,'job',?7);",
            -1,
            &outbox.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_text(outbox.st, 1, event_id_value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outbox.st, 2, event_type, -1, SQLITE_STATIC);
    sqlite3_bind_text(outbox.st, 3, aggregate_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outbox.st, 4, correlation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outbox.st, 5, causation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(outbox.st, 6, now);
    sqlite3_bind_int64(outbox.st, 7, job_id);
    if (sqlite3_step(outbox.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

struct JobStateForAction {
    std::int64_t job_set_id = 0;
    std::string state;
    bool workset_backed = false;
};

std::optional<JobStateForAction> GetJobStateForAction(sqlite3* db, std::int64_t job_id, std::string* error_out) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT job_set_id,state,workset_id "
            "FROM exec_job WHERE job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, job_id);
    const auto rc = sqlite3_step(st.st);
    if (rc != SQLITE_ROW) {
        if (error_out) *error_out = rc == SQLITE_DONE ? "job not found" : sqlite3_errmsg(db);
        return std::nullopt;
    }
    JobStateForAction row{};
    row.job_set_id = sqlite3_column_int64(st.st, 0);
    const auto* state = sqlite3_column_text(st.st, 1);
    row.state = state != nullptr ? reinterpret_cast<const char*>(state) : "";
    row.workset_backed =
        sqlite3_column_type(st.st, 2) != SQLITE_NULL;
    return row;
}

constexpr const char* kReadyWorksetExistsSql =
    "SELECT EXISTS("
    "SELECT 1 FROM exec_workset w "
    "WHERE EXISTS(SELECT 1 FROM exec_job j "
    " WHERE j.workset_id=w.workset_id AND j.state='QUEUED' "
    " AND j.attempts<j.max_attempts) "
    "AND NOT EXISTS(SELECT 1 FROM exec_job j "
    " WHERE j.workset_id=w.workset_id AND j.state='QUEUED' "
    " AND j.attempts>=j.max_attempts) "
    "AND NOT EXISTS(SELECT 1 FROM exec_workset_dispatch_attempt d "
    " WHERE d.workset_id=w.workset_id "
    " AND d.state IN ('CLAIMED','ACTIVE','DRAINING')) "
    ");";

std::optional<bool> HasReadyWorksets(
    sqlite3* db,
    std::string* error_out) {
    Statement ready;
    if (sqlite3_prepare_v2(
            db, kReadyWorksetExistsSql, -1, &ready.st, nullptr)
        != SQLITE_OK
        || sqlite3_step(ready.st) != SQLITE_ROW) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db);
        return std::nullopt;
    }
    return sqlite3_column_int(ready.st, 0) != 0;
}

bool RefreshExecutionWorkAvailability(
    sqlite3* db,
    std::string* error_out,
    bool force) {
    if (!force && db == g_batch_transaction_db) {
        g_batch_refresh_requested = true;
        return true;
    }
    const auto ready = HasReadyWorksets(db, error_out);
    if (!ready.has_value()) return false;
    Statement pending;
    if (sqlite3_prepare_v2(
            db,
            "SELECT "
            "EXISTS(SELECT 1 FROM exec_job j "
            " JOIN exec_temp_blob b ON b.temp_blob_id=j.worker_result_blob_id "
            " WHERE j.state='EXECUTION_FINISHED' "
            " AND j.result_processing_state='PENDING' "
            " AND b.cleanup_state='LIVE');",
            -1,
            &pending.st,
            nullptr) != SQLITE_OK
        || sqlite3_step(pending.st) != SQLITE_ROW) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db);
        return false;
    }
    const bool results = sqlite3_column_int(pending.st, 0) != 0;
    Statement update;
    if (sqlite3_prepare_v2(
            db,
            "UPDATE exec_work_availability "
            "SET generation=CASE "
            " WHEN generation=9223372036854775807 THEN 1 "
            " ELSE generation+1 END,"
            "has_ready_worksets=?1,"
            "has_execution_finished_results=?2,"
            "changed_at_utc=CAST(unixepoch('now') * 1000 AS INTEGER) "
            "WHERE singleton_id=1 AND (has_ready_worksets<>?1 "
            "OR has_execution_finished_results<>?2);",
            -1,
            &update.st,
            nullptr) != SQLITE_OK) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_int(update.st, 1, *ready ? 1 : 0);
    sqlite3_bind_int(update.st, 2, results ? 1 : 0);
    if (sqlite3_step(update.st) != SQLITE_DONE) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

std::optional<events::ExecutionWorkflowJobPayloadView> ResolveWorkflowEventPayload(sqlite3* db, std::int64_t payload_ref_id) {
    Statement st;
    if (sqlite3_prepare_v2(db,
        "SELECT e.workflow_instance_id, COALESCE(e.workflow_step_id, 0), "
        "COALESCE(s.job_set_id, 0) "
        "FROM exec_workflow_event e "
        "LEFT JOIN exec_workflow_step s ON s.workflow_step_id=e.workflow_step_id "
        "WHERE e.workflow_event_id=?1;",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::ExecutionWorkflowJobPayloadView view{};
    view.workflow_instance_id = sqlite3_column_int64(st.st, 0);
    view.workflow_step_id = sqlite3_column_int64(st.st, 1);
    view.job_set_id = sqlite3_column_int64(st.st, 2);
    return view;
}

std::optional<events::ExecutionWorkflowJobPayloadView> ResolveJobSetPayload(sqlite3* db, std::int64_t payload_ref_id) {
    Statement st;
    if (sqlite3_prepare_v2(db,
        "SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1;",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::ExecutionWorkflowJobPayloadView view{};
    view.job_set_id = sqlite3_column_int64(st.st, 0);
    return view;
}

std::optional<events::ExecutionWorkflowJobPayloadView>
ResolveWorksetPayload(sqlite3* db, std::int64_t payload_ref_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT workset_id,job_set_id "
            "FROM exec_workset WHERE workset_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::ExecutionWorkflowJobPayloadView view{};
    view.workset_id = sqlite3_column_int64(st.st, 0);
    view.job_set_id = sqlite3_column_int64(st.st, 1);
    return view;
}

std::optional<events::ExecutionWorkflowJobPayloadView> ResolveJobPayload(sqlite3* db, std::int64_t payload_ref_id) {
    Statement st;
    if (sqlite3_prepare_v2(db,
        "SELECT job_id, job_set_id "
        "FROM exec_job "
        "WHERE job_id=?1;",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::ExecutionWorkflowJobPayloadView view{};
    view.job_id = sqlite3_column_int64(st.st, 0);
    view.job_set_id = sqlite3_column_int64(st.st, 1);
    return view;
}

bool ResolveWorkflowStepForJobSetAncestry(
    sqlite3* db,
    std::int64_t job_set_id,
    ClaimedExecutionJob* claimed,
    std::string* error_out) {
    if (claimed == nullptr) {
        if (error_out) *error_out = "claimed job output is required";
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(db,
        "WITH RECURSIVE job_set_ancestry(job_set_id, parent_job_set_id, depth) AS ("
        "  SELECT js.job_set_id, js.parent_job_set_id, 0 "
        "  FROM exec_job_set js "
        "  WHERE js.job_set_id=?1 "
        "  UNION ALL "
        "  SELECT parent.job_set_id, parent.parent_job_set_id, job_set_ancestry.depth + 1 "
        "  FROM exec_job_set parent "
        "  JOIN job_set_ancestry ON parent.job_set_id=job_set_ancestry.parent_job_set_id "
        "  WHERE job_set_ancestry.parent_job_set_id IS NOT NULL "
        "    AND job_set_ancestry.depth < 64"
        ") "
        "SELECT s.workflow_instance_id, s.workflow_step_id, s.step_key, s.step_kind, s.priority "
        "FROM job_set_ancestry a "
        "JOIN exec_workflow_step s ON s.job_set_id=a.job_set_id "
        "ORDER BY a.depth ASC "
        "LIMIT 1;",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }

    sqlite3_bind_int64(st.st, 1, job_set_id);
    const auto rc = sqlite3_step(st.st);
    if (rc == SQLITE_ROW) {
        claimed->workflow_instance_id = sqlite3_column_int64(st.st, 0);
        claimed->workflow_step_id = sqlite3_column_int64(st.st, 1);
        const auto* step_key = sqlite3_column_text(st.st, 2);
        const auto* step_kind = sqlite3_column_text(st.st, 3);
        claimed->workflow_step_key = step_key ? reinterpret_cast<const char*>(step_key) : "";
        claimed->workflow_step_kind = step_kind ? reinterpret_cast<const char*>(step_kind) : "";
        claimed->workflow_step_priority = sqlite3_column_int(st.st, 4);
        return true;
    }
    if (rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }

    if (error_out) {
        *error_out = "workflow step not found for job_set ancestry job_set_id=" + std::to_string(job_set_id);
    }
    return false;
}

struct CancellationInsertResult {
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
    std::int64_t cancellation_request_id = 0;
    std::string state;
};

CommittedJobCancellation MakeCommittedCancellation(
    const ExecutionCancellationRequestSpec& request,
    const CancellationInsertResult& inserted) {
    return {
        .cancellation_request_id = inserted.cancellation_request_id,
        .job_id = request.job_id,
        .request_key = request.request_key,
        .reason_code = request.reason_code,
        .reason_text = request.reason_text,
        .caused_by_job_id = request.caused_by_job_id,
        .state = inserted.state,
        .disposition = inserted.disposition,
    };
}

bool PopulateCommittedCancellationExecutionFacts(
    sqlite3* db,
    CommittedJobCancellation* cancellation,
    std::string* error_out) {
    if (db == nullptr || cancellation == nullptr
        || cancellation->job_id <= 0) {
        if (error_out != nullptr) {
            *error_out = "invalid committed cancellation execution facts";
        }
        return false;
    }
    Statement facts;
    if (sqlite3_prepare_v2(
            db,
            "SELECT j.state,j.workset_id,j.dispatch_attempt_id,d.claim_token "
            "FROM exec_job j "
            "LEFT JOIN exec_workset_dispatch_attempt d "
            " ON d.dispatch_attempt_id=j.dispatch_attempt_id "
            "WHERE j.job_id=?1;",
            -1,
            &facts.st,
            nullptr) != SQLITE_OK) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_int64(facts.st, 1, cancellation->job_id);
    if (sqlite3_step(facts.st) != SQLITE_ROW) {
        if (error_out != nullptr) {
            *error_out = "committed cancellation target job is missing";
        }
        return false;
    }
    cancellation->durable_job_state = Text(facts.st, 0);
    if (sqlite3_column_type(facts.st, 1) != SQLITE_NULL) {
        cancellation->workset_id = sqlite3_column_int64(facts.st, 1);
    }
    if (sqlite3_column_type(facts.st, 2) != SQLITE_NULL) {
        cancellation->dispatch_attempt_id =
            sqlite3_column_int64(facts.st, 2);
    }
    cancellation->claim_token = OptionalText(facts.st, 3);
    return true;
}

bool InsertCancellationRequest(
    sqlite3* db,
    const ExecutionCancellationRequestSpec& request,
    std::int64_t now,
    CancellationInsertResult* result_out,
    std::string* error_out) {
    if (result_out == nullptr) {
        if (error_out != nullptr) {
            *error_out = "cancellation insert result is required";
        }
        return false;
    }
    *result_out = {};

    Statement existing;
    if (sqlite3_prepare_v2(
            db,
            "SELECT cancellation_request_id,reason_code,reason_text,"
            "requested_by,caused_by_job_id,state "
            "FROM exec_job_cancellation_request "
            "WHERE job_id=?1 AND request_key=?2;",
            -1,
            &existing.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_int64(existing.st, 1, request.job_id);
    sqlite3_bind_text(
        existing.st,
        2,
        request.request_key.c_str(),
        -1,
        SQLITE_TRANSIENT);
    const auto existing_rc = sqlite3_step(existing.st);
    if (existing_rc == SQLITE_ROW) {
        const auto existing_cause =
            sqlite3_column_type(existing.st, 4) == SQLITE_NULL
            ? std::optional<std::int64_t>{}
            : std::optional<std::int64_t>{
                sqlite3_column_int64(existing.st, 4)};
        if (Text(existing.st, 1) != request.reason_code
            || OptionalText(existing.st, 2) != request.reason_text
            || Text(existing.st, 3) != request.requested_by
            || existing_cause != request.caused_by_job_id) {
            result_out->disposition =
                ExecutionDbOperationDisposition::Conflict;
            result_out->cancellation_request_id =
                sqlite3_column_int64(existing.st, 0);
            result_out->state = Text(existing.st, 5);
            return true;
        }
        result_out->disposition =
            ExecutionDbOperationDisposition::AlreadyApplied;
        result_out->cancellation_request_id =
            sqlite3_column_int64(existing.st, 0);
        result_out->state = Text(existing.st, 5);
        return true;
    }
    if (existing_rc != SQLITE_DONE) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db);
        return false;
    }

    Statement job;
    if (sqlite3_prepare_v2(
            db,
            "SELECT job_set_id FROM exec_job WHERE job_id=?1;",
            -1,
            &job.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_int64(job.st, 1, request.job_id);
    if (sqlite3_step(job.st) != SQLITE_ROW) {
        result_out->disposition =
            ExecutionDbOperationDisposition::Missing;
        return true;
    }
    const auto job_set_id = sqlite3_column_int64(job.st, 0);

    Statement active;
    if (sqlite3_prepare_v2(
            db,
            "SELECT cancellation_request_id,state "
            "FROM exec_job_cancellation_request "
            "WHERE job_id=?1 "
            "AND state IN ('REQUESTED','DELIVERED') "
            "LIMIT 1;",
            -1,
            &active.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_int64(active.st, 1, request.job_id);
    if (sqlite3_step(active.st) == SQLITE_ROW) {
        result_out->disposition =
            ExecutionDbOperationDisposition::Conflict;
        result_out->cancellation_request_id =
            sqlite3_column_int64(active.st, 0);
        result_out->state = Text(active.st, 1);
        return true;
    }

    Statement insert;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO exec_job_cancellation_request("
            "job_id,request_key,reason_code,reason_text,requested_by,"
            "caused_by_job_id,requested_at_utc,state) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,'REQUESTED');",
            -1,
            &insert.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_int64(insert.st, 1, request.job_id);
    sqlite3_bind_text(
        insert.st, 2, request.request_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(
        insert.st, 3, request.reason_code.c_str(), -1, SQLITE_TRANSIENT);
    BindOptionalText(insert.st, 4, request.reason_text);
    sqlite3_bind_text(
        insert.st, 5, request.requested_by.c_str(), -1, SQLITE_TRANSIENT);
    if (request.caused_by_job_id.has_value()) {
        sqlite3_bind_int64(insert.st, 6, *request.caused_by_job_id);
    } else {
        sqlite3_bind_null(insert.st, 6);
    }
    sqlite3_bind_int64(insert.st, 7, now);
    if (sqlite3_step(insert.st) != SQLITE_DONE) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db);
        return false;
    }
    result_out->cancellation_request_id = sqlite3_last_insert_rowid(db);

    Statement summary;
    if (sqlite3_prepare_v2(
            db,
            "UPDATE exec_job SET "
            "cancellation_state='REQUESTED',"
            "cancellation_request_key=?1,cancellation_reason_code=?2,"
            "cancellation_reason_text=?3,cancellation_requested_by=?4,"
            "cancellation_caused_by_job_id=?5,"
            "cancellation_requested_at_utc=?6,"
            "cancellation_delivery_attempts=0,"
            "cancellation_last_delivery_error_code=NULL,"
            "cancellation_last_delivery_error_text=NULL,"
            "cancellation_last_delivery_failed_at_utc=NULL,"
            "cancellation_delivered_at_utc=NULL,"
            "cancellation_resolved_at_utc=NULL,"
            "cancellation_resolution_code=NULL "
            "WHERE job_id=?7;",
            -1,
            &summary.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_text(
        summary.st, 1, request.request_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(
        summary.st, 2, request.reason_code.c_str(), -1, SQLITE_TRANSIENT);
    BindOptionalText(summary.st, 3, request.reason_text);
    sqlite3_bind_text(
        summary.st, 4, request.requested_by.c_str(), -1, SQLITE_TRANSIENT);
    if (request.caused_by_job_id.has_value()) {
        sqlite3_bind_int64(summary.st, 5, *request.caused_by_job_id);
    } else {
        sqlite3_bind_null(summary.st, 5);
    }
    sqlite3_bind_int64(summary.st, 6, now);
    sqlite3_bind_int64(summary.st, 7, request.job_id);
    if (sqlite3_step(summary.st) != SQLITE_DONE
        || sqlite3_changes(db) != 1
        || !InsertJobActionEventAndOutbox(
            db,
            request.job_id,
            job_set_id,
            "Execution.JobCancellationRequested.v1",
            "durable-cancellation-requested",
            error_out)) {
        if (error_out != nullptr && error_out->empty()) {
            *error_out = "failed updating cancellation summary";
        }
        return false;
    }
    result_out->disposition =
        ExecutionDbOperationDisposition::Applied;
    result_out->state = "REQUESTED";
    return true;
}

} // namespace

SqliteExecutionDb::SqliteExecutionDb(sqlite3* db)
    : db_(db)
    , query_service_(std::make_unique<SqliteWorkflowOrchestrationQueryService>(db_))
    , command_service_(std::make_unique<SqliteWorkflowOrchestrationCommandService>(db_))
    , job_command_service_(std::make_unique<jobs::SqliteJobEventCommandService>(db_)) {
}


bool SqliteExecutionDb::ValidationExecuteSql(std::string_view sql, std::string* error_out) const {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    char* sqlite_err = nullptr;
    const int rc = sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &sqlite_err);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite_err ? sqlite_err : sqlite3_errmsg(db_);
        sqlite3_free(sqlite_err);
        return false;
    }
    sqlite3_free(sqlite_err);
    return true;
}

bool SqliteExecutionDb::ValidationQueryInt(std::string_view sql, std::int64_t* value_out, std::string* error_out) const {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (value_out == nullptr) {
        if (error_out) *error_out = "value_out is required";
        return false;
    }
    Statement st;
    if (sqlite3_prepare_v2(db_, std::string(sql).c_str(), -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        if (error_out) *error_out = "query returned no row";
        return false;
    }
    *value_out = sqlite3_column_int64(st.st, 0);
    return true;
}

bool SqliteExecutionDb::ValidationQueryText(std::string_view sql, std::string* value_out, std::string* error_out) const {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (value_out == nullptr) {
        if (error_out) *error_out = "value_out is required";
        return false;
    }
    Statement st;
    if (sqlite3_prepare_v2(db_, std::string(sql).c_str(), -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        if (error_out) *error_out = "query returned no row";
        return false;
    }
    const auto* text = sqlite3_column_text(st.st, 0);
    *value_out = text ? reinterpret_cast<const char*>(text) : "";
    return true;
}

bool SqliteExecutionDb::ValidationExecuteInvariantRemediation(
    const WorkflowInvariantRemediationCommand& command,
    bool* reopened_out,
    std::string* error_out) {
    WorkflowRecoveryService recovery(db_);
    return recovery.ExecuteInvariantRemediation(command, command_service_.get(), reopened_out, error_out);
}

IWorkflowOrchestrationQueryService* SqliteExecutionDb::WorkflowQueryService() {
    return query_service_.get();
}

IWorkflowOrchestrationCommandService* SqliteExecutionDb::WorkflowCommandService() {
    return command_service_.get();
}

jobs::IJobEventCommandService* SqliteExecutionDb::JobCommandService() {
    return job_command_service_.get();
}

bool SqliteExecutionDb::CreateWorkflowInstance(
    const WorkflowCreateInstanceCommand& command,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (command_service_ == nullptr) {
        if (error_out) *error_out = "workflow command service unavailable";
        return false;
    }
    return command_service_->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
}

std::optional<ExecutionJobRecord> SqliteExecutionDb::GetJob(std::int64_t job_id) const {
    if (db_ == nullptr || job_id <= 0) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(db_,
        "SELECT job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, "
        "savestate_id, fingerprint, state, priority, attempts, max_attempts, queued_at_utc, input_ini, "
        "claimed_by_token, lease_expires_at_utc,workset_id,workset_item_ordinal,"
        "dispatch_attempt_id,dispatch_item_ordinal,reserved_attempt_id,"
        "execution_finished_at_utc,"
        "worker_terminal_status,worker_terminal_fingerprint,worker_result_blob_id,"
        "result_processing_state,result_processing_attempts,result_processing_failures,"
        "cancellation_state,cancellation_group_key "
        "FROM exec_job WHERE job_id=?1;",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    ExecutionJobRecord row{};
    row.job_id = sqlite3_column_int64(st.st, 0);
    row.job_set_id = sqlite3_column_int64(st.st, 1);
    row.program_kind = sqlite3_column_int(st.st, 2);
    row.program_version = sqlite3_column_int(st.st, 3);
    row.program_ref_kind = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 4));
    row.program_ref_id = sqlite3_column_int64(st.st, 5);
    if (sqlite3_column_type(st.st, 6) != SQLITE_NULL) {
        row.savestate_id = sqlite3_column_int64(st.st, 6);
    }
    row.fingerprint = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 7));
    row.state = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 8));
    row.priority = sqlite3_column_int(st.st, 9);
    row.attempts = sqlite3_column_int(st.st, 10);
    row.max_attempts = sqlite3_column_int(st.st, 11);
    row.queued_at_utc = sqlite3_column_int64(st.st, 12);
    const auto* input_ini = sqlite3_column_text(st.st, 13);
    row.input_ini = input_ini == nullptr ? "" : reinterpret_cast<const char*>(input_ini);
    if (sqlite3_column_type(st.st, 14) != SQLITE_NULL) {
        const auto* token = sqlite3_column_text(st.st, 14);
        if (token != nullptr && token[0] != '\0') {
            row.claimed_by_token = reinterpret_cast<const char*>(token);
        }
    }
    if (sqlite3_column_type(st.st, 15) != SQLITE_NULL) {
        row.lease_expires_at_utc = sqlite3_column_int64(st.st, 15);
    }
    if (sqlite3_column_type(st.st, 16) != SQLITE_NULL) {
        row.workset_id = sqlite3_column_int64(st.st, 16);
    }
    if (sqlite3_column_type(st.st, 17) != SQLITE_NULL) {
        row.workset_item_ordinal = sqlite3_column_int(st.st, 17);
    }
    if (sqlite3_column_type(st.st, 18) != SQLITE_NULL) {
        row.dispatch_attempt_id = sqlite3_column_int64(st.st, 18);
    }
    if (sqlite3_column_type(st.st, 19) != SQLITE_NULL) {
        row.dispatch_item_ordinal = static_cast<std::uint32_t>(
            sqlite3_column_int(st.st, 19));
    }
    if (sqlite3_column_type(st.st, 20) != SQLITE_NULL) {
        row.reserved_attempt_id = static_cast<std::uint64_t>(
            sqlite3_column_int64(st.st, 20));
    }
    if (sqlite3_column_type(st.st, 21) != SQLITE_NULL) {
        row.execution_finished_at_utc = sqlite3_column_int64(st.st, 21);
    }
    row.worker_terminal_status = OptionalText(st.st, 22);
    row.worker_terminal_fingerprint = OptionalText(st.st, 23);
    if (sqlite3_column_type(st.st, 24) != SQLITE_NULL) {
        row.worker_result_blob_id = sqlite3_column_int64(st.st, 24);
    }
    row.result_processing_state = OptionalText(st.st, 25);
    row.result_processing_attempts = sqlite3_column_int(st.st, 26);
    row.result_processing_failures = sqlite3_column_int(st.st, 27);
    row.cancellation_state = OptionalText(st.st, 28);
    row.cancellation_group_key = OptionalText(st.st, 29);
    return row;
}

std::vector<ExecutionJobEventRecord> SqliteExecutionDb::ListJobEvents(std::int64_t job_id, int limit) const {
    std::vector<ExecutionJobEventRecord> rows;
    if (db_ == nullptr || job_id <= 0) {
        return rows;
    }
    if (limit <= 0) {
        limit = 128;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT job_event_id,job_id,event_kind,event_ts_utc,COALESCE(message,''),artifact_id "
            "FROM exec_job_event "
            "WHERE job_id=?1 "
            "ORDER BY job_event_id DESC "
            "LIMIT ?2;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, job_id);
    sqlite3_bind_int(st.st, 2, limit);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        ExecutionJobEventRecord row{};
        row.job_event_id = sqlite3_column_int64(st.st, 0);
        row.job_id = sqlite3_column_int64(st.st, 1);
        const auto* kind = sqlite3_column_text(st.st, 2);
        const auto* message = sqlite3_column_text(st.st, 4);
        row.event_kind = kind != nullptr ? reinterpret_cast<const char*>(kind) : "";
        row.event_ts_utc = sqlite3_column_int64(st.st, 3);
        row.message = message != nullptr ? reinterpret_cast<const char*>(message) : "";
        if (sqlite3_column_type(st.st, 5) != SQLITE_NULL) {
            row.artifact_id = sqlite3_column_int64(st.st, 5);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

bool SqliteExecutionDb::RecordJobOutput(
    const RecordExecutionJobOutputCommand& command,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.job_id <= 0) {
        if (error_out) *error_out = "job_id must be > 0";
        return false;
    }
    if (command.output_key.empty() || command.data_kind.empty() || command.ref_kind.empty() || command.ref_id <= 0) {
        if (error_out) *error_out = "output_key, data_kind, ref_kind, and ref_id are required";
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    Statement authority;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT workset_id FROM exec_job WHERE job_id=?1;",
            -1,
            &authority.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Rollback(db_);
        return false;
    }
    sqlite3_bind_int64(authority.st, 1, command.job_id);
    const auto authority_rc = sqlite3_step(authority.st);
    if (authority_rc != SQLITE_ROW) {
        if (error_out) {
            *error_out = authority_rc == SQLITE_DONE
                ? "job not found"
                : sqlite3_errmsg(db_);
        }
        Rollback(db_);
        return false;
    }
    if (sqlite3_column_type(authority.st, 0) != SQLITE_NULL) {
        if (error_out) {
            *error_out =
                "legacy output API cannot mutate a workset-backed job";
        }
        Rollback(db_);
        return false;
    }

    Statement insert;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO exec_job_output(job_id, output_key, data_kind, ref_kind, ref_id, created_at_utc) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6) "
            "ON CONFLICT(job_id, output_key) DO UPDATE SET "
            "created_at_utc=CASE WHEN data_kind=excluded.data_kind AND ref_kind=excluded.ref_kind AND ref_id=excluded.ref_id THEN created_at_utc ELSE created_at_utc END;",
            -1,
            &insert.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Rollback(db_);
        return false;
    }
    sqlite3_bind_int64(insert.st, 1, command.job_id);
    sqlite3_bind_text(insert.st, 2, command.output_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.st, 3, command.data_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.st, 4, command.ref_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.st, 5, command.ref_id);
    sqlite3_bind_int64(insert.st, 6, NowUtcMillis());
    if (sqlite3_step(insert.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Rollback(db_);
        return false;
    }

    Statement verify;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT data_kind, ref_kind, ref_id FROM exec_job_output WHERE job_id=?1 AND output_key=?2;",
            -1,
            &verify.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        Rollback(db_);
        return false;
    }
    sqlite3_bind_int64(verify.st, 1, command.job_id);
    sqlite3_bind_text(verify.st, 2, command.output_key.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(verify.st) != SQLITE_ROW
        || command.data_kind != reinterpret_cast<const char*>(sqlite3_column_text(verify.st, 0))
        || command.ref_kind != reinterpret_cast<const char*>(sqlite3_column_text(verify.st, 1))
        || command.ref_id != sqlite3_column_int64(verify.st, 2)) {
        if (error_out) *error_out = "job output already exists with incompatible shape";
        Rollback(db_);
        return false;
    }

    return Commit(db_, error_out);
}

std::vector<ExecutionJobOutputRecord> SqliteExecutionDb::ListJobOutputsForWorkflowStep(
    std::int64_t workflow_step_id) const {
    std::vector<ExecutionJobOutputRecord> rows;
    if (db_ == nullptr || workflow_step_id <= 0) {
        return rows;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "WITH RECURSIVE step_root(job_set_id) AS ("
            "  SELECT job_set_id FROM exec_workflow_step WHERE workflow_step_id=?1 AND job_set_id IS NOT NULL"
            "), "
            "job_set_descendants(job_set_id, depth) AS ("
            "  SELECT job_set_id, 0 FROM step_root "
            "  UNION ALL "
            "  SELECT child.job_set_id, job_set_descendants.depth + 1 "
            "  FROM exec_job_set child "
            "  JOIN job_set_descendants ON child.parent_job_set_id=job_set_descendants.job_set_id "
            "  WHERE job_set_descendants.depth < 64"
            ") "
            "SELECT o.job_output_id, o.job_id, o.output_key, o.data_kind, o.ref_kind, o.ref_id, o.created_at_utc "
            "FROM exec_job_output o "
            "JOIN exec_job j ON j.job_id=o.job_id "
            "JOIN job_set_descendants d ON d.job_set_id=j.job_set_id "
            "ORDER BY o.job_output_id;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, workflow_step_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        ExecutionJobOutputRecord row{};
        row.job_output_id = sqlite3_column_int64(st.st, 0);
        row.job_id = sqlite3_column_int64(st.st, 1);
        row.output_key = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 2));
        row.data_kind = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 3));
        row.ref_kind = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 4));
        row.ref_id = sqlite3_column_int64(st.st, 5);
        row.created_at_utc = sqlite3_column_int64(st.st, 6);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::optional<std::string> SqliteExecutionDb::GetJobInputIni(std::int64_t job_id, std::string* error_out) const {
    const auto job = GetJob(job_id);
    if (!job.has_value()) {
        if (error_out) *error_out = "job not found";
        return std::nullopt;
    }
    if (error_out) error_out->clear();
    return job->input_ini;
}

bool SqliteExecutionDb::RequeueJob(std::int64_t job_id, std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (job_id <= 0) {
        if (error_out) *error_out = "job_id must be > 0";
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    const auto job = GetJobStateForAction(db_, job_id, error_out);
    if (!job.has_value()) {
        Rollback(db_);
        return false;
    }
    if (job->workset_backed) {
        Rollback(db_);
        if (error_out) {
            *error_out =
                "legacy requeue API cannot mutate a workset-backed job";
        }
        return false;
    }
    if (job->state == "QUEUED" || job->state == "CLAIMED" || job->state == "RUNNING" || job->state == "FAILED") {
        Rollback(db_);
        if (error_out) *error_out = "job cannot be requeued from state " + job->state;
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job "
            "SET state='QUEUED', claimed_by_token=NULL, lease_expires_at_utc=NULL, started_at_utc=NULL, ended_at_utc=NULL, error_code=NULL, error_text=NULL "
            "WHERE job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, job_id);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (!InsertJobActionEventAndOutbox(db_, job_id, job->job_set_id, "Execution.JobQueued.v1", "REQUEUE", error_out)) {
        Rollback(db_);
        return false;
    }
    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    return true;
}

bool SqliteExecutionDb::RestartFailedJob(std::int64_t job_id, std::optional<std::string> input_ini_override, std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (job_id <= 0) {
        if (error_out) *error_out = "job_id must be > 0";
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    const auto job = GetJobStateForAction(db_, job_id, error_out);
    if (!job.has_value()) {
        Rollback(db_);
        return false;
    }
    if (job->workset_backed) {
        Rollback(db_);
        if (error_out) {
            *error_out =
                "legacy restart API cannot mutate a workset-backed job";
        }
        return false;
    }
    if (job->state != "FAILED") {
        Rollback(db_);
        if (error_out) *error_out = "restart requires FAILED job";
        return false;
    }

    const char* sql = input_ini_override.has_value()
        ? "UPDATE exec_job "
          "SET state='QUEUED', attempts=0, claimed_by_token=NULL, lease_expires_at_utc=NULL, started_at_utc=NULL, ended_at_utc=NULL, error_code=NULL, error_text=NULL, input_ini=?2 "
          "WHERE job_id=?1;"
        : "UPDATE exec_job "
          "SET state='QUEUED', attempts=0, claimed_by_token=NULL, lease_expires_at_utc=NULL, started_at_utc=NULL, ended_at_utc=NULL, error_code=NULL, error_text=NULL "
          "WHERE job_id=?1;";
    Statement st;
    if (sqlite3_prepare_v2(db_, sql, -1, &st.st, nullptr) != SQLITE_OK) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, job_id);
    if (input_ini_override.has_value()) {
        sqlite3_bind_text(st.st, 2, input_ini_override->c_str(), -1, SQLITE_TRANSIENT);
    }
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (!InsertJobActionEventAndOutbox(db_, job_id, job->job_set_id, "Execution.JobQueued.v1", "RESTART", error_out)) {
        Rollback(db_);
        return false;
    }
    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    return true;
}

bool SqliteExecutionDb::CancelQueuedOrClaimedJob(std::int64_t job_id, std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (job_id <= 0) {
        if (error_out) *error_out = "job_id must be > 0";
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    const auto job = GetJobStateForAction(db_, job_id, error_out);
    if (!job.has_value()) {
        Rollback(db_);
        return false;
    }
    if (job->workset_backed) {
        Rollback(db_);
        if (error_out) {
            *error_out =
                "legacy cancel API cannot mutate a workset-backed job";
        }
        return false;
    }
    if (job->state != "QUEUED" && job->state != "INTERRUPTED" && job->state != "CLAIMED") {
        Rollback(db_);
        if (error_out) *error_out = "job cannot be canceled from state " + job->state;
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job "
            "SET state='CANCELED', claimed_by_token=NULL, lease_expires_at_utc=NULL, ended_at_utc=?2 "
            "WHERE job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, job_id);
    sqlite3_bind_int64(st.st, 2, CurrentUtcMs(db_));
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (!InsertJobActionEventAndOutbox(db_, job_id, job->job_set_id, "Execution.JobCompleted.v1", "CANCEL", error_out)) {
        Rollback(db_);
        return false;
    }
    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    return true;
}

std::optional<ExecutionJobSetProgressDetails> SqliteExecutionDb::GetJobSetProgress(std::int64_t job_set_id) const {
    if (db_ == nullptr || job_set_id <= 0) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "WITH RECURSIVE job_set_descendants(job_set_id, depth) AS ("
            "  SELECT js.job_set_id, 0 FROM exec_job_set js WHERE js.job_set_id=?1 "
            "  UNION ALL "
            "  SELECT child.job_set_id, job_set_descendants.depth + 1 "
            "  FROM exec_job_set child "
            "  JOIN job_set_descendants ON child.parent_job_set_id=job_set_descendants.job_set_id "
            "  WHERE job_set_descendants.depth < 64"
            "), "
            "summary AS ("
            "  SELECT "
            "    COUNT(j.job_id) AS total_jobs, "
            "    COALESCE(SUM(CASE WHEN j.state IN ('COMPLETED','SUCCEEDED','SUCCEEDED_WINNER','SUPERSEDED','SUCCEEDED_DUPLICATE','FAILED','CANCELED') THEN 1 ELSE 0 END), 0) AS completed_jobs, "
            "    COALESCE(SUM(CASE WHEN j.state IN ('COMPLETED','SUCCEEDED','SUCCEEDED_WINNER','SUPERSEDED','SUCCEEDED_DUPLICATE') THEN 1 ELSE 0 END), 0) AS succeeded_jobs, "
            "    COALESCE(SUM(CASE WHEN j.state='FAILED' THEN 1 ELSE 0 END), 0) AS failed_jobs, "
            "    COALESCE(SUM(CASE WHEN j.state='CANCELED' THEN 1 ELSE 0 END), 0) AS canceled_jobs "
            "  FROM job_set_descendants d "
            "  LEFT JOIN exec_job j ON j.job_set_id=d.job_set_id"
            "), "
            "expected AS ("
            "  SELECT COALESCE(SUM(COALESCE(js.expected_total, 0)), 0) "
            "    AS expected_total "
            "  FROM exec_job_set js "
            "  JOIN job_set_descendants d ON d.job_set_id=js.job_set_id"
            ") "
            "SELECT ?1, summary.total_jobs, summary.completed_jobs, summary.succeeded_jobs, "
            "summary.failed_jobs, summary.canceled_jobs, expected.expected_total "
            "FROM summary, expected;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, job_set_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    ExecutionJobSetProgressDetails details{};
    details.job_set_id = sqlite3_column_int64(st.st, 0);
    details.total_jobs = sqlite3_column_int64(st.st, 1);
    details.completed_jobs = sqlite3_column_int64(st.st, 2);
    details.succeeded_jobs = sqlite3_column_int64(st.st, 3);
    details.failed_jobs = sqlite3_column_int64(st.st, 4);
    details.canceled_jobs = sqlite3_column_int64(st.st, 5);
    if (sqlite3_column_type(st.st, 6) != SQLITE_NULL) {
        details.expected_total = sqlite3_column_int64(st.st, 6);
    }
    return details;
}

std::vector<ExecutionJobSetJobRecord>
SqliteExecutionDb::ListJobsInJobSet(std::int64_t job_set_id) const {
    std::vector<ExecutionJobSetJobRecord> rows;
    if (db_ == nullptr || job_set_id <= 0) {
        return rows;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT job_id,state,COALESCE(input_ini,''),"
            "cancellation_group_key "
            "FROM exec_job "
            "WHERE job_set_id=?1 "
            "ORDER BY job_id ASC;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, job_set_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        rows.push_back(
            ExecutionJobSetJobRecord{
                .job_id = sqlite3_column_int64(st.st, 0),
                .state = Text(st.st, 1),
                .input_ini = Text(st.st, 2),
                .cancellation_group_key =
                    OptionalText(st.st, 3),
            });
    }
    return rows;
}

std::vector<ExecutionJobSetJobRecord>
SqliteExecutionDb::ListJobsByProgramReference(
    std::int32_t program_kind,
    std::string_view program_ref_kind,
    std::int64_t program_ref_id) const {
    std::vector<ExecutionJobSetJobRecord> rows;
    if (db_ == nullptr || program_kind <= 0
        || program_ref_kind.empty() || program_ref_id <= 0) {
        return rows;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT job_id,state,COALESCE(input_ini,''),"
            "cancellation_group_key FROM exec_job "
            "WHERE program_kind=?1 AND program_ref_kind=?2 "
            "AND program_ref_id=?3 ORDER BY job_id ASC;",
            -1,
            &st.st,
            nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int(st.st, 1, program_kind);
    sqlite3_bind_text(st.st, 2, program_ref_kind.data(),
        static_cast<int>(program_ref_kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 3, program_ref_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        rows.push_back({
            .job_id = sqlite3_column_int64(st.st, 0),
            .state = Text(st.st, 1),
            .input_ini = Text(st.st, 2),
            .cancellation_group_key = OptionalText(st.st, 3),
        });
    }
    return rows;
}

std::optional<ExecutionJobSetMaterializationRecord>
SqliteExecutionDb::GetJobSetByMaterializationKey(
    std::string_view materialization_key) const {
    if (db_ == nullptr || materialization_key.empty()) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT job_set_id,materialization_key,"
            "COALESCE(materialization_state,''),parent_job_set_id,"
            "domain_ref_kind,domain_ref_id,purpose,expected_total "
            "FROM exec_job_set WHERE materialization_key=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(
        st.st,
        1,
        materialization_key.data(),
        static_cast<int>(materialization_key.size()),
        SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    ExecutionJobSetMaterializationRecord row{};
    row.job_set_id = sqlite3_column_int64(st.st, 0);
    row.materialization_key = Text(st.st, 1);
    row.materialization_state = Text(st.st, 2);
    if (sqlite3_column_type(st.st, 3) != SQLITE_NULL) {
        row.parent_job_set_id = sqlite3_column_int64(st.st, 3);
    }
    row.domain_ref_kind = OptionalText(st.st, 4);
    if (sqlite3_column_type(st.st, 5) != SQLITE_NULL) {
        row.domain_ref_id = sqlite3_column_int64(st.st, 5);
    }
    row.purpose = Text(st.st, 6);
    if (sqlite3_column_type(st.st, 7) != SQLITE_NULL) {
        row.expected_total = sqlite3_column_int(st.st, 7);
    }
    return row;
}

std::vector<ExecutionChildJobSetProgressDetails> SqliteExecutionDb::GetChildJobSetProgress(std::int64_t parent_job_set_id) const {
    std::vector<ExecutionChildJobSetProgressDetails> rows;
    if (db_ == nullptr || parent_job_set_id <= 0) {
        return rows;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT js.job_set_id, js.purpose, COALESCE(js.meta_note, ''), "
            "COALESCE((SELECT COUNT(1) FROM exec_job j WHERE j.job_set_id=js.job_set_id), 0) AS total_jobs, "
            "COALESCE((SELECT COUNT(1) FROM exec_job j "
            "         WHERE j.job_set_id=js.job_set_id "
            "           AND j.state IN ('COMPLETED','SUCCEEDED','SUCCEEDED_WINNER','SUPERSEDED','SUCCEEDED_DUPLICATE','FAILED','CANCELED')), 0) AS completed_jobs, "
            "COALESCE((SELECT COUNT(1) FROM exec_job j "
            "         WHERE j.job_set_id=js.job_set_id "
            "           AND j.state IN ('COMPLETED','SUCCEEDED','SUCCEEDED_WINNER','SUPERSEDED','SUCCEEDED_DUPLICATE')), 0) AS succeeded_jobs, "
            "COALESCE((SELECT COUNT(1) FROM exec_job j WHERE j.job_set_id=js.job_set_id AND j.state='FAILED'), 0) AS failed_jobs, "
            "COALESCE((SELECT COUNT(1) FROM exec_job j WHERE j.job_set_id=js.job_set_id AND j.state='CANCELED'), 0) AS canceled_jobs, "
            "js.expected_total "
            "FROM exec_job_set js "
            "WHERE js.parent_job_set_id=?1 "
            "ORDER BY js.job_set_id ASC;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return rows;
    }

    sqlite3_bind_int64(st.st, 1, parent_job_set_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        ExecutionChildJobSetProgressDetails row{};
        row.job_set_id = sqlite3_column_int64(st.st, 0);
        const auto* purpose = sqlite3_column_text(st.st, 1);
        const auto* meta_note = sqlite3_column_text(st.st, 2);
        row.purpose = purpose ? reinterpret_cast<const char*>(purpose) : "";
        row.meta_note = meta_note ? reinterpret_cast<const char*>(meta_note) : "";
        row.total_jobs = sqlite3_column_int64(st.st, 3);
        row.completed_jobs = sqlite3_column_int64(st.st, 4);
        row.succeeded_jobs = sqlite3_column_int64(st.st, 5);
        row.failed_jobs = sqlite3_column_int64(st.st, 6);
        row.canceled_jobs = sqlite3_column_int64(st.st, 7);
        if (sqlite3_column_type(st.st, 8) != SQLITE_NULL) {
            row.expected_total = sqlite3_column_int64(st.st, 8);
        }
        constexpr std::string_view token = "expected_delta=";
        const auto pos = row.meta_note.find(token);
        if (pos != std::string::npos) {
            const auto start = pos + token.size();
            try {
                row.expected_delta = std::stoll(row.meta_note.substr(start));
            } catch (...) {
                row.expected_delta = std::nullopt;
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

bool SqliteExecutionDb::CreateJobSet(
    const CreateJobSetCommand& command,
    std::int64_t* job_set_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.program_kind <= 0) {
        if (error_out) *error_out = "program_kind must be > 0";
        return false;
    }
    if (command.purpose.empty()) {
        if (error_out) *error_out = "purpose is required";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto rollback = [&]() { (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr); };

    const auto now = command.created_at_utc > 0 ? command.created_at_utc : NowUtcMillis();
    Statement insert_set;
    if (sqlite3_prepare_v2(db_,
        "INSERT INTO exec_job_set(parent_job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);",
        -1,
        &insert_set.st,
        nullptr)
        != SQLITE_OK) {
        rollback();
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (command.parent_job_set_id.has_value()) sqlite3_bind_int64(insert_set.st, 1, *command.parent_job_set_id); else sqlite3_bind_null(insert_set.st, 1);
    sqlite3_bind_int(insert_set.st, 2, command.program_kind);
    sqlite3_bind_text(insert_set.st, 3, command.purpose.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_set.st, 4, command.created_by.value().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_set.st, 5, now);
    sqlite3_bind_int(insert_set.st, 6, command.priority_boost);
    if (command.expected_total.has_value()) sqlite3_bind_int(insert_set.st, 7, *command.expected_total); else sqlite3_bind_null(insert_set.st, 7);
    if (command.domain_ref_kind.has_value()) sqlite3_bind_text(insert_set.st, 8, command.domain_ref_kind->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(insert_set.st, 8);
    if (command.domain_ref_id.has_value()) sqlite3_bind_int64(insert_set.st, 9, *command.domain_ref_id); else sqlite3_bind_null(insert_set.st, 9);
    if (command.meta_note.has_value()) sqlite3_bind_text(insert_set.st, 10, command.meta_note->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(insert_set.st, 10);
    if (sqlite3_step(insert_set.st) != SQLITE_DONE) {
        rollback();
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto job_set_id = sqlite3_last_insert_rowid(db_);

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        rollback();
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    if (job_set_id_out) *job_set_id_out = job_set_id;

    if (job_command_service_) {
        std::string ignored;
        (void)job_command_service_->AppendLifecycleEvent(
            {
                .kind = jobs::JobLifecycleEventKind::JobSetCreated,
                .job_set_id = job_set_id,
                .message = command.meta_note,
                .requested_by = command.created_by,
            },
            &ignored);
    }

    return true;
}

bool SqliteExecutionDb::EnqueueJob(
    const EnqueueJobCommand& command,
    std::int64_t* job_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.job_set_id <= 0 || command.program_kind <= 0 || command.program_ref_kind.empty() || command.program_ref_id <= 0 || command.fingerprint.empty()) {
        if (error_out) *error_out = "invalid enqueue command";
        return false;
    }

    const auto queued_at_utc = CurrentUtcMs(db_);
    Statement insert_job;
    if (sqlite3_prepare_v2(db_,
        "INSERT INTO exec_job(job_set_id,parent_job_id,program_kind,program_version,program_ref_kind,program_ref_id,savestate_id,fingerprint,priority,state,attempts,max_attempts,claimed_by_token,lease_expires_at_utc,queued_at_utc,started_at_utc,ended_at_utc,error_code,error_text,input_ini) "
        "SELECT ?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,"
        "NULL,NULL,?13,NULL,NULL,NULL,NULL,?14 "
        "FROM exec_job_set js "
        "WHERE js.job_set_id=?1 AND js.materialization_state IS NULL;",
        -1,
        &insert_job.st,
        nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(insert_job.st, 1, command.job_set_id);
    if (command.parent_job_id.has_value()) sqlite3_bind_int64(insert_job.st, 2, *command.parent_job_id); else sqlite3_bind_null(insert_job.st, 2);
    sqlite3_bind_int(insert_job.st, 3, command.program_kind);
    sqlite3_bind_int(insert_job.st, 4, command.program_version);
    sqlite3_bind_text(insert_job.st, 5, command.program_ref_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_job.st, 6, command.program_ref_id);
    if (command.savestate_id.has_value()) sqlite3_bind_int64(insert_job.st, 7, *command.savestate_id); else sqlite3_bind_null(insert_job.st, 7);
    sqlite3_bind_text(insert_job.st, 8, command.fingerprint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insert_job.st, 9, command.priority);
    const char* initial_state = command.pending_until_workflow_materialized ? "PENDING_MATERIALIZATION" : "QUEUED";
    sqlite3_bind_text(insert_job.st, 10, initial_state, -1, SQLITE_STATIC);
    sqlite3_bind_int(insert_job.st, 11, 0);
    sqlite3_bind_int(insert_job.st, 12, command.max_attempts);
    sqlite3_bind_int64(insert_job.st, 13, queued_at_utc);
    sqlite3_bind_text(insert_job.st, 14, command.input_ini.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(insert_job.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_changes(db_) != 1) {
        if (error_out) {
            *error_out =
                "legacy enqueue cannot target a materializing job set";
        }
        return false;
    }

    if (job_id_out) *job_id_out = sqlite3_last_insert_rowid(db_);
    return true;
}

bool SqliteExecutionDb::EnsureMaterializingJobSet(
    const EnsureMaterializingJobSetCommand& command,
    EnsureMaterializingJobSetReceipt* receipt_out,
    std::string* error_out) {
    EnsureMaterializingJobSetReceipt receipt{};
    if (receipt_out != nullptr) {
        *receipt_out = receipt;
    }
    if (db_ == nullptr
        || command.materialization_key.empty()
        || command.program_kind <= 0
        || command.purpose.empty()
        || command.created_at_utc < 0
        || (command.parent_job_set_id.has_value()
            && *command.parent_job_set_id <= 0)
        || (command.expected_total.has_value()
            && *command.expected_total < 0)
        || command.domain_ref_kind.has_value()
            != command.domain_ref_id.has_value()
        || (command.domain_ref_id.has_value()
            && *command.domain_ref_id <= 0)) {
        receipt.disposition =
            ExecutionDbOperationDisposition::InvalidRequest;
        if (receipt_out != nullptr) *receipt_out = receipt;
        if (error_out != nullptr) {
            *error_out = "invalid materializing job-set command";
        }
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }
    const auto fail = [&](std::string message) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = std::move(message);
    };

    Statement existing;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT job_set_id,parent_job_set_id,program_kind,purpose,"
            "created_by,priority_boost,expected_total,domain_ref_kind,"
            "domain_ref_id,meta_note,COALESCE(materialization_state,'') "
            "FROM exec_job_set WHERE materialization_key=?1;",
            -1,
            &existing.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_text(
        existing.st,
        1,
        command.materialization_key.c_str(),
        -1,
        SQLITE_TRANSIENT);
    const auto existing_rc = sqlite3_step(existing.st);
    if (existing_rc == SQLITE_ROW) {
        receipt.job_set_id = sqlite3_column_int64(existing.st, 0);
        receipt.materialization_state = Text(existing.st, 10);
        const auto parent_job_set_id =
            sqlite3_column_type(existing.st, 1) == SQLITE_NULL
            ? std::optional<std::int64_t>{}
            : std::optional<std::int64_t>{
                sqlite3_column_int64(existing.st, 1)};
        const auto expected_total =
            sqlite3_column_type(existing.st, 6) == SQLITE_NULL
            ? std::optional<int>{}
            : std::optional<int>{
                sqlite3_column_int(existing.st, 6)};
        const auto domain_ref_id =
            sqlite3_column_type(existing.st, 8) == SQLITE_NULL
            ? std::optional<std::int64_t>{}
            : std::optional<std::int64_t>{
                sqlite3_column_int64(existing.st, 8)};
        if (parent_job_set_id != command.parent_job_set_id
            || sqlite3_column_int(existing.st, 2)
                != command.program_kind
            || Text(existing.st, 3) != command.purpose
            || OptionalText(existing.st, 4) != command.created_by
            || sqlite3_column_int(existing.st, 5)
                != command.priority_boost
            || expected_total != command.expected_total
            || OptionalText(existing.st, 7)
                != command.domain_ref_kind
            || domain_ref_id != command.domain_ref_id
            || OptionalText(existing.st, 9) != command.meta_note) {
            Rollback(db_);
            receipt.disposition =
                ExecutionDbOperationDisposition::Conflict;
            if (receipt_out != nullptr) *receipt_out = receipt;
            if (error_out != nullptr) {
                *error_out =
                    "materialization_key identifies a different job set";
            }
            return true;
        }
        if (!Commit(db_, error_out)) {
            Rollback(db_);
            return false;
        }
        receipt.disposition =
            ExecutionDbOperationDisposition::AlreadyApplied;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    if (existing_rc != SQLITE_DONE) {
        fail(sqlite3_errmsg(db_));
        return false;
    }

    const auto now = command.created_at_utc > 0
        ? command.created_at_utc
        : CurrentUtcMs(db_);
    Statement insert;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO exec_job_set("
            "parent_job_set_id,program_kind,purpose,created_by,"
            "created_at_utc,priority_boost,expected_total,"
            "domain_ref_kind,domain_ref_id,meta_note,"
            "materialization_key,materialization_state) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,"
            "'MATERIALIZING');",
            -1,
            &insert.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    if (command.parent_job_set_id.has_value()) {
        sqlite3_bind_int64(
            insert.st,
            1,
            *command.parent_job_set_id);
    } else {
        sqlite3_bind_null(insert.st, 1);
    }
    sqlite3_bind_int(insert.st, 2, command.program_kind);
    sqlite3_bind_text(
        insert.st,
        3,
        command.purpose.c_str(),
        -1,
        SQLITE_TRANSIENT);
    BindOptionalText(insert.st, 4, command.created_by);
    sqlite3_bind_int64(insert.st, 5, now);
    sqlite3_bind_int(insert.st, 6, command.priority_boost);
    if (command.expected_total.has_value()) {
        sqlite3_bind_int(insert.st, 7, *command.expected_total);
    } else {
        sqlite3_bind_null(insert.st, 7);
    }
    BindOptionalText(insert.st, 8, command.domain_ref_kind);
    if (command.domain_ref_id.has_value()) {
        sqlite3_bind_int64(insert.st, 9, *command.domain_ref_id);
    } else {
        sqlite3_bind_null(insert.st, 9);
    }
    BindOptionalText(insert.st, 10, command.meta_note);
    sqlite3_bind_text(
        insert.st,
        11,
        command.materialization_key.c_str(),
        -1,
        SQLITE_TRANSIENT);
    if (sqlite3_step(insert.st) != SQLITE_DONE) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    receipt.job_set_id = sqlite3_last_insert_rowid(db_);
    receipt.materialization_state = "MATERIALIZING";
    if (!InsertAggregateOutboxEvent(
            db_,
            "Execution.JobSetMaterializing.v1",
            "job_set",
            receipt.job_set_id,
            "job_set",
            receipt.job_set_id,
            "materializing-" + std::to_string(receipt.job_set_id),
            error_out)
        || !Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    receipt.disposition = ExecutionDbOperationDisposition::Applied;
    if (receipt_out != nullptr) *receipt_out = receipt;
    return true;
}

bool SqliteExecutionDb::CreatePendingJob(
    const CreatePendingJobCommand& command,
    CreatePendingJobReceipt* receipt_out,
    std::string* error_out) {
    CreatePendingJobReceipt receipt{};
    if (receipt_out != nullptr) *receipt_out = receipt;
    if (db_ == nullptr
        || command.job_set_id <= 0
        || command.program_kind <= 0
        || command.program_version <= 0
        || command.program_ref_kind.empty()
        || command.program_ref_id <= 0
        || command.fingerprint.empty()
        || command.max_attempts <= 0
        || (command.cancellation_group_key.has_value()
            && command.cancellation_group_key->empty())
        || (command.parent_job_id.has_value()
            && *command.parent_job_id <= 0)
        || (command.savestate_id.has_value()
            && *command.savestate_id <= 0)) {
        receipt.disposition =
            ExecutionDbOperationDisposition::InvalidRequest;
        if (receipt_out != nullptr) *receipt_out = receipt;
        if (error_out != nullptr) {
            *error_out = "invalid pending-job command";
        }
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }
    const auto fail = [&](std::string message) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = std::move(message);
    };

    Statement job_set;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT program_kind,COALESCE(materialization_state,'') "
            "FROM exec_job_set WHERE job_set_id=?1;",
            -1,
            &job_set.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(job_set.st, 1, command.job_set_id);
    if (sqlite3_step(job_set.st) != SQLITE_ROW) {
        Rollback(db_);
        receipt.disposition =
            ExecutionDbOperationDisposition::Missing;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    if (sqlite3_column_int(job_set.st, 0) != command.program_kind
        || Text(job_set.st, 1) != "MATERIALIZING") {
        Rollback(db_);
        receipt.disposition =
            ExecutionDbOperationDisposition::WrongState;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }

    Statement existing;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT job_id,job_set_id,state,parent_job_id,program_kind,"
            "program_version,program_ref_kind,program_ref_id,savestate_id,"
            "priority,max_attempts,input_ini,cancellation_group_key "
            "FROM exec_job "
            "WHERE fingerprint=?1;",
            -1,
            &existing.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_text(
        existing.st,
        1,
        command.fingerprint.c_str(),
        -1,
        SQLITE_TRANSIENT);
    const auto existing_rc = sqlite3_step(existing.st);
    if (existing_rc == SQLITE_ROW) {
        receipt.job_id = sqlite3_column_int64(existing.st, 0);
        const auto parent_job_id =
            sqlite3_column_type(existing.st, 3) == SQLITE_NULL
            ? std::optional<std::int64_t>{}
            : std::optional<std::int64_t>{
                sqlite3_column_int64(existing.st, 3)};
        const auto savestate_id =
            sqlite3_column_type(existing.st, 8) == SQLITE_NULL
            ? std::optional<std::int64_t>{}
            : std::optional<std::int64_t>{
                sqlite3_column_int64(existing.st, 8)};
        receipt.disposition =
            sqlite3_column_int64(existing.st, 1) == command.job_set_id
                && Text(existing.st, 2) == "PENDING_WORKSET"
                && parent_job_id == command.parent_job_id
                && sqlite3_column_int(existing.st, 4)
                    == command.program_kind
                && sqlite3_column_int(existing.st, 5)
                    == command.program_version
                && Text(existing.st, 6)
                    == command.program_ref_kind
                && sqlite3_column_int64(existing.st, 7)
                    == command.program_ref_id
                && savestate_id == command.savestate_id
                && sqlite3_column_int(existing.st, 9)
                    == command.priority
                && sqlite3_column_int(existing.st, 10)
                    == command.max_attempts
                && Text(existing.st, 11) == command.input_ini
                && OptionalText(existing.st, 12)
                    == command.cancellation_group_key
            ? ExecutionDbOperationDisposition::AlreadyApplied
            : ExecutionDbOperationDisposition::Conflict;
        if (!Commit(db_, error_out)) {
            Rollback(db_);
            return false;
        }
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    if (existing_rc != SQLITE_DONE) {
        fail(sqlite3_errmsg(db_));
        return false;
    }

    const auto now = CurrentUtcMs(db_);
    Statement insert;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO exec_job("
            "job_set_id,parent_job_id,program_kind,program_version,"
            "program_ref_kind,program_ref_id,savestate_id,fingerprint,"
            "priority,state,attempts,max_attempts,claimed_by_token,"
            "lease_expires_at_utc,queued_at_utc,started_at_utc,"
            "ended_at_utc,error_code,error_text,input_ini,"
            "cancellation_group_key) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,"
            "'PENDING_WORKSET',0,?10,NULL,NULL,?11,NULL,NULL,NULL,NULL,?12,?13);",
            -1,
            &insert.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(insert.st, 1, command.job_set_id);
    if (command.parent_job_id.has_value()) {
        sqlite3_bind_int64(insert.st, 2, *command.parent_job_id);
    } else {
        sqlite3_bind_null(insert.st, 2);
    }
    sqlite3_bind_int(insert.st, 3, command.program_kind);
    sqlite3_bind_int(insert.st, 4, command.program_version);
    sqlite3_bind_text(
        insert.st,
        5,
        command.program_ref_kind.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.st, 6, command.program_ref_id);
    if (command.savestate_id.has_value()) {
        sqlite3_bind_int64(insert.st, 7, *command.savestate_id);
    } else {
        sqlite3_bind_null(insert.st, 7);
    }
    sqlite3_bind_text(
        insert.st,
        8,
        command.fingerprint.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_int(insert.st, 9, command.priority);
    sqlite3_bind_int(insert.st, 10, command.max_attempts);
    sqlite3_bind_int64(insert.st, 11, now);
    sqlite3_bind_text(
        insert.st,
        12,
        command.input_ini.c_str(),
        -1,
        SQLITE_TRANSIENT);
    BindOptionalText(
        insert.st,
        13,
        command.cancellation_group_key);
    if (sqlite3_step(insert.st) != SQLITE_DONE) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    receipt.job_id = sqlite3_last_insert_rowid(db_);
    if (!InsertJobActionEventAndOutbox(
            db_,
            receipt.job_id,
            command.job_set_id,
            "Execution.JobPendingWorkset.v1",
            "pending-workset",
            error_out)
        || !Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    receipt.disposition = ExecutionDbOperationDisposition::Applied;
    if (receipt_out != nullptr) *receipt_out = receipt;
    return true;
}

bool SqliteExecutionDb::SealJobPopulation(
    const SealJobPopulationCommand& command,
    SealJobPopulationReceipt* receipt_out,
    std::string* error_out) {
    SealJobPopulationReceipt receipt{};
    if (receipt_out != nullptr) *receipt_out = receipt;
    if (db_ == nullptr
        || command.job_set_id <= 0
        || command.expected_job_count < 0
        || command.requested_by.empty()) {
        receipt.disposition =
            ExecutionDbOperationDisposition::InvalidRequest;
        if (receipt_out != nullptr) *receipt_out = receipt;
        if (error_out != nullptr) {
            *error_out = "invalid seal-job-population command";
        }
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }
    const auto fail = [&](std::string message) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = std::move(message);
    };
    Statement state;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT COALESCE(materialization_state,''),"
            "(SELECT COUNT(1) FROM exec_job WHERE job_set_id=?1) "
            "FROM exec_job_set WHERE job_set_id=?1;",
            -1,
            &state.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(state.st, 1, command.job_set_id);
    if (sqlite3_step(state.st) != SQLITE_ROW) {
        Rollback(db_);
        receipt.disposition =
            ExecutionDbOperationDisposition::Missing;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    receipt.materialization_state = Text(state.st, 0);
    receipt.durable_job_count = sqlite3_column_int(state.st, 1);
    if (receipt.durable_job_count != command.expected_job_count) {
        Rollback(db_);
        receipt.disposition =
            ExecutionDbOperationDisposition::Conflict;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    if (receipt.materialization_state != "MATERIALIZING") {
        receipt.disposition =
            receipt.materialization_state == "POPULATION_SEALED"
                || receipt.materialization_state == "PUBLISHING_WORKSETS"
                || receipt.materialization_state
                    == "WORKSET_PUBLICATION_COMPLETE"
            ? ExecutionDbOperationDisposition::AlreadyApplied
            : ExecutionDbOperationDisposition::WrongState;
        if (!Commit(db_, error_out)) {
            Rollback(db_);
            return false;
        }
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    const auto now = CurrentUtcMs(db_);
    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job_set "
            "SET materialization_state='POPULATION_SEALED',"
            "population_sealed_at_utc=?1 "
            "WHERE job_set_id=?2 AND materialization_state='MATERIALIZING';",
            -1,
            &update.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(update.st, 1, now);
    sqlite3_bind_int64(update.st, 2, command.job_set_id);
    if (sqlite3_step(update.st) != SQLITE_DONE
        || sqlite3_changes(db_) != 1) {
        fail("job population seal CAS failed");
        return false;
    }
    if (!InsertAggregateOutboxEvent(
            db_,
            "Execution.JobPopulationSealed.v1",
            "job_set",
            command.job_set_id,
            "job_set",
            command.job_set_id,
            "population-sealed-" + std::to_string(command.job_set_id),
            error_out)
        || !Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    receipt.disposition = ExecutionDbOperationDisposition::Applied;
    receipt.materialization_state = "POPULATION_SEALED";
    if (receipt_out != nullptr) *receipt_out = receipt;
    return true;
}

bool SqliteExecutionDb::PublishWorksetWave(
    const PublishWorksetWaveCommand& command,
    PublishWorksetWaveReceipt* receipt_out,
    std::string* error_out) {
    PublishWorksetWaveReceipt receipt{};
    if (receipt_out != nullptr) *receipt_out = receipt;
    if (db_ == nullptr || command.job_set_id <= 0
        || command.expected_job_count < 0
        || command.worksets.empty() || command.requested_by.empty()) {
        receipt.disposition = ExecutionDbOperationDisposition::InvalidRequest;
        if (receipt_out != nullptr) *receipt_out = receipt;
        if (error_out != nullptr) *error_out = "invalid publish-workset-wave command";
        return false;
    }

    std::unordered_set<std::string> workset_keys;
    std::unordered_set<std::int64_t> job_ids;
    std::size_t job_count = 0;
    for (const auto& workset : command.worksets) {
        if (workset.job_set_id != command.job_set_id
            || workset.requested_by != command.requested_by
            || !workset_keys.insert(workset.workset_key).second) {
            receipt.disposition = ExecutionDbOperationDisposition::InvalidRequest;
            if (receipt_out != nullptr) *receipt_out = receipt;
            if (error_out != nullptr) {
                *error_out = "workset wave contains inconsistent or duplicate worksets";
            }
            return false;
        }
        job_count += workset.ordered_job_ids.size();
        for (const auto job_id : workset.ordered_job_ids) {
            if (!job_ids.insert(job_id).second) {
                receipt.disposition = ExecutionDbOperationDisposition::InvalidRequest;
                if (receipt_out != nullptr) *receipt_out = receipt;
                if (error_out != nullptr) {
                    *error_out = "workset wave contains duplicate job membership";
                }
                return false;
            }
        }
    }
    if (job_count != static_cast<std::size_t>(command.expected_job_count)) {
        receipt.disposition = ExecutionDbOperationDisposition::InvalidRequest;
        if (receipt_out != nullptr) *receipt_out = receipt;
        if (error_out != nullptr) *error_out = "workset wave job count mismatch";
        return false;
    }

    std::string availability_error;
    const auto availability_before =
        GetExecutionWorkAvailability(&availability_error);
    if (!availability_before.has_value()) {
        if (error_out != nullptr) *error_out = std::move(availability_error);
        return false;
    }

    BatchTransactionScope batch(db_);
    if (!batch.Begin(error_out)) return false;
    receipt.worksets.reserve(command.worksets.size());
    for (const auto& workset : command.worksets) {
        PublishWorksetReceipt item_receipt{};
        if (!PublishWorkset(workset, &item_receipt, error_out)) {
            return false;
        }
        receipt.worksets.push_back(item_receipt);
        if (item_receipt.disposition != ExecutionDbOperationDisposition::Applied
            && item_receipt.disposition
                != ExecutionDbOperationDisposition::AlreadyApplied) {
            receipt.disposition = item_receipt.disposition;
            batch.RollbackAll();
            if (receipt_out != nullptr) *receipt_out = receipt;
            return true;
        }
    }

    CompleteWorksetPublicationReceipt completion{};
    if (!CompleteWorksetPublication(
            CompleteWorksetPublicationCommand{
                .job_set_id = command.job_set_id,
                .expected_workset_count =
                    static_cast<int>(command.worksets.size()),
                .expected_job_count = command.expected_job_count,
                .requested_by = command.requested_by,
            },
            &completion,
            error_out)) {
        return false;
    }
    receipt.durable_workset_count = completion.durable_workset_count;
    receipt.durable_job_count = completion.durable_job_count;
    receipt.materialization_state = completion.materialization_state;
    if (completion.disposition != ExecutionDbOperationDisposition::Applied
        && completion.disposition
            != ExecutionDbOperationDisposition::AlreadyApplied) {
        receipt.disposition = completion.disposition;
        batch.RollbackAll();
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    if (!batch.CommitAll(error_out)) return false;

    const auto availability_after = GetExecutionWorkAvailability(error_out);
    if (!availability_after.has_value()) return false;
    receipt.ready_workset_availability_changed =
        availability_before->generation != availability_after->generation;
    receipt.disposition = completion.disposition;
    if (receipt_out != nullptr) *receipt_out = receipt;
    return true;
}

bool SqliteExecutionDb::PublishWorkset(
    const PublishWorksetCommand& command,
    PublishWorksetReceipt* receipt_out,
    std::string* error_out) {
    PublishWorksetReceipt receipt{};
    if (receipt_out != nullptr) *receipt_out = receipt;
    const auto& compatibility = command.compatibility;
    if (db_ == nullptr
        || command.job_set_id <= 0
        || command.workset_key.empty()
        || command.program_kind <= 0
        || command.program_version <= 0
        || ((command.workflow_step_id > 0)
            != (command.root_job_set_id > 0))
        || command.ordered_job_ids.empty()
        || command.ordered_job_ids.size()
            > static_cast<std::size_t>(
                std::numeric_limits<int>::max())
        || command.requested_by.empty()
        || compatibility.compatibility_key.empty()
        || compatibility.module_canonical_id.empty()
        || compatibility.module_version <= 0
        || !IsSha256(compatibility.module_sha256)
        || compatibility.entrypoint.empty()
        || !IsSha256(compatibility.verified_dependency_sha256)
        || !IsSha256(compatibility.runtime_profile_sha256)
        || compatibility.required_capability_mask
            > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())
        || compatibility.estimated_payload_bytes
            > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
        receipt.disposition =
            ExecutionDbOperationDisposition::InvalidRequest;
        if (receipt_out != nullptr) *receipt_out = receipt;
        if (error_out != nullptr) {
            *error_out = "invalid publish-workset command";
        }
        return false;
    }
    std::unordered_set<std::int64_t> unique_jobs;
    for (const auto job_id : command.ordered_job_ids) {
        if (job_id <= 0 || !unique_jobs.insert(job_id).second) {
            receipt.disposition =
                ExecutionDbOperationDisposition::InvalidRequest;
            if (receipt_out != nullptr) *receipt_out = receipt;
            if (error_out != nullptr) {
                *error_out = "workset job ids must be positive and unique";
            }
            return false;
        }
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }
    const auto fail = [&](std::string message) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = std::move(message);
    };
    Statement job_set;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT program_kind,COALESCE(materialization_state,'') "
            "FROM exec_job_set WHERE job_set_id=?1;",
            -1,
            &job_set.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(job_set.st, 1, command.job_set_id);
    if (sqlite3_step(job_set.st) != SQLITE_ROW) {
        Rollback(db_);
        receipt.disposition =
            ExecutionDbOperationDisposition::Missing;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    const auto publication_state = Text(job_set.st, 1);
    if (sqlite3_column_int(job_set.st, 0) != command.program_kind) {
        Rollback(db_);
        receipt.disposition =
            ExecutionDbOperationDisposition::Conflict;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }

    std::int64_t root_job_set_id = command.root_job_set_id;
    std::int64_t workflow_step_id = command.workflow_step_id;
    if (workflow_step_id > 0) {
        Statement root;
        if (sqlite3_prepare_v2(
                db_,
                "WITH RECURSIVE ancestors(job_set_id,parent_job_set_id) AS ("
                "  SELECT job_set_id,parent_job_set_id FROM exec_job_set "
                "  WHERE job_set_id=?1 "
                "  UNION ALL "
                "  SELECT parent.job_set_id,parent.parent_job_set_id "
                "  FROM exec_job_set parent "
                "  JOIN ancestors child "
                "    ON child.parent_job_set_id=parent.job_set_id"
                ") "
                "SELECT job_set_id FROM ancestors "
                "WHERE parent_job_set_id IS NULL;",
                -1,
                &root.st,
                nullptr)
            != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int64(root.st, 1, command.job_set_id);
        if (sqlite3_step(root.st) != SQLITE_ROW ||
            sqlite3_column_int64(root.st, 0) != root_job_set_id ||
            sqlite3_step(root.st) != SQLITE_DONE) {
            fail("published workset root invocation anchor disagrees with job-set ancestry");
            return false;
        }

        Statement step;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT job_set_id FROM exec_workflow_step "
                "WHERE workflow_step_id=?1;",
                -1,
                &step.st,
                nullptr)
            != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int64(step.st, 1, workflow_step_id);
        if (sqlite3_step(step.st) != SQLITE_ROW) {
            fail("published workset workflow-step invocation anchor does not exist");
            return false;
        }
        const auto existing_root =
            sqlite3_column_type(step.st, 0) == SQLITE_NULL
            ? std::optional<std::int64_t>{}
            : std::optional<std::int64_t>{
                  sqlite3_column_int64(step.st, 0)};
        if ((existing_root.has_value() &&
             *existing_root != root_job_set_id) ||
            sqlite3_step(step.st) != SQLITE_DONE) {
            fail("published workset workflow-step invocation anchor conflicts with its durable root");
            return false;
        }
    } else {
        Statement invocation_anchor;
        if (sqlite3_prepare_v2(
                db_,
                "WITH RECURSIVE ancestors(job_set_id,parent_job_set_id) AS ("
                "  SELECT job_set_id,parent_job_set_id FROM exec_job_set "
                "  WHERE job_set_id=?1 "
                "  UNION ALL "
                "  SELECT parent.job_set_id,parent.parent_job_set_id "
                "  FROM exec_job_set parent "
                "  JOIN ancestors child "
                "    ON child.parent_job_set_id=parent.job_set_id"
                ") "
                "SELECT a.job_set_id,s.workflow_step_id "
                "FROM ancestors a "
                "JOIN exec_workflow_step s ON s.job_set_id=a.job_set_id "
                "WHERE a.parent_job_set_id IS NULL;",
                -1,
                &invocation_anchor.st,
                nullptr)
            != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int64(
            invocation_anchor.st, 1, command.job_set_id);
        if (sqlite3_step(invocation_anchor.st) != SQLITE_ROW) {
            fail("workset job set has no workflow-step root invocation anchor");
            return false;
        }
        root_job_set_id =
            sqlite3_column_int64(invocation_anchor.st, 0);
        workflow_step_id =
            sqlite3_column_int64(invocation_anchor.st, 1);
        if (root_job_set_id <= 0 || workflow_step_id <= 0 ||
            sqlite3_step(invocation_anchor.st) != SQLITE_DONE) {
            fail("workset job set has an ambiguous workflow-step root invocation anchor");
            return false;
        }
    }

    Statement existing;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT workset_id,item_count,program_kind,program_version,"
            "compatibility_key,module_canonical_id,module_version,"
            "module_sha256,entrypoint,verified_dependency_sha256,"
            "runtime_profile_sha256,required_capability_mask,"
            "execution_affinity_key,baseline_affinity_key,"
            "estimated_payload_bytes,priority,workflow_step_id,"
            "root_job_set_id "
            "FROM exec_workset "
            "WHERE job_set_id=?1 AND workset_key=?2;",
            -1,
            &existing.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(existing.st, 1, command.job_set_id);
    sqlite3_bind_text(
        existing.st,
        2,
        command.workset_key.c_str(),
        -1,
        SQLITE_TRANSIENT);
    const auto existing_rc = sqlite3_step(existing.st);
    if (existing_rc == SQLITE_ROW) {
        receipt.workset_id = sqlite3_column_int64(existing.st, 0);
        receipt.item_count = sqlite3_column_int(existing.st, 1);
        const bool metadata_matches =
            receipt.item_count
                == static_cast<int>(command.ordered_job_ids.size())
            && sqlite3_column_int(existing.st, 2)
                == command.program_kind
            && sqlite3_column_int(existing.st, 3)
                == command.program_version
            && Text(existing.st, 4)
                == compatibility.compatibility_key
            && Text(existing.st, 5)
                == compatibility.module_canonical_id
            && sqlite3_column_int(existing.st, 6)
                == compatibility.module_version
            && Text(existing.st, 7) == compatibility.module_sha256
            && Text(existing.st, 8) == compatibility.entrypoint
            && Text(existing.st, 9)
                == compatibility.verified_dependency_sha256
            && Text(existing.st, 10)
                == compatibility.runtime_profile_sha256
            && static_cast<std::uint64_t>(
                sqlite3_column_int64(existing.st, 11))
                == compatibility.required_capability_mask
            && OptionalText(existing.st, 12)
                == compatibility.execution_affinity_key
            && OptionalText(existing.st, 13)
                == compatibility.baseline_affinity_key
            && static_cast<std::uint64_t>(
                sqlite3_column_int64(existing.st, 14))
                == compatibility.estimated_payload_bytes
            && sqlite3_column_int(existing.st, 15)
                == command.priority
            && sqlite3_column_int64(existing.st, 16)
                == workflow_step_id
            && sqlite3_column_int64(existing.st, 17)
                == root_job_set_id;
        bool membership_matches = metadata_matches;
        if (membership_matches) {
            Statement members;
            if (sqlite3_prepare_v2(
                    db_,
                    "SELECT job_id FROM exec_job "
                    "WHERE workset_id=?1 "
                    "ORDER BY workset_item_ordinal ASC;",
                    -1,
                    &members.st,
                    nullptr)
                != SQLITE_OK) {
                fail(sqlite3_errmsg(db_));
                return false;
            }
            sqlite3_bind_int64(
                members.st,
                1,
                receipt.workset_id);
            for (const auto expected_job_id :
                 command.ordered_job_ids) {
                if (sqlite3_step(members.st) != SQLITE_ROW
                    || sqlite3_column_int64(members.st, 0)
                        != expected_job_id) {
                    membership_matches = false;
                    break;
                }
            }
            if (membership_matches
                && sqlite3_step(members.st) != SQLITE_DONE) {
                membership_matches = false;
            }
        }
        receipt.disposition = membership_matches
            ? ExecutionDbOperationDisposition::AlreadyApplied
            : ExecutionDbOperationDisposition::Conflict;
        if (!Commit(db_, error_out)) {
            Rollback(db_);
            return false;
        }
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    if (existing_rc != SQLITE_DONE) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    if (publication_state != "POPULATION_SEALED"
        && publication_state != "PUBLISHING_WORKSETS") {
        Rollback(db_);
        receipt.disposition =
            ExecutionDbOperationDisposition::WrongState;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }

    Statement validate_job;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT job_set_id,program_kind,program_version,state,"
            "workset_id,workset_item_ordinal "
            "FROM exec_job WHERE job_id=?1;",
            -1,
            &validate_job.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    for (const auto job_id : command.ordered_job_ids) {
        sqlite3_reset(validate_job.st);
        sqlite3_clear_bindings(validate_job.st);
        sqlite3_bind_int64(validate_job.st, 1, job_id);
        if (sqlite3_step(validate_job.st) != SQLITE_ROW
            || sqlite3_column_int64(validate_job.st, 0)
                != command.job_set_id
            || sqlite3_column_int(validate_job.st, 1)
                != command.program_kind
            || sqlite3_column_int(validate_job.st, 2)
                != command.program_version
            || Text(validate_job.st, 3) != "PENDING_WORKSET"
            || sqlite3_column_type(validate_job.st, 4) != SQLITE_NULL
            || sqlite3_column_type(validate_job.st, 5) != SQLITE_NULL) {
            Rollback(db_);
            receipt.disposition =
                ExecutionDbOperationDisposition::Conflict;
            if (receipt_out != nullptr) *receipt_out = receipt;
            return true;
        }
    }

    const auto now = CurrentUtcMs(db_);
    Statement insert_workset;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO exec_workset("
            "job_set_id,workflow_step_id,root_job_set_id,workset_key,"
            "program_kind,program_version,"
            "compatibility_key,module_canonical_id,module_version,"
            "module_sha256,entrypoint,verified_dependency_sha256,"
            "runtime_profile_sha256,required_capability_mask,"
            "execution_affinity_key,baseline_affinity_key,"
            "estimated_payload_bytes,priority,item_count,published_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,"
            "?13,?14,?15,?16,?17,?18,?19,?20);",
            -1,
            &insert_workset.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(insert_workset.st, 1, command.job_set_id);
    sqlite3_bind_int64(insert_workset.st, 2, workflow_step_id);
    sqlite3_bind_int64(insert_workset.st, 3, root_job_set_id);
    sqlite3_bind_text(
        insert_workset.st,
        4,
        command.workset_key.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_int(insert_workset.st, 5, command.program_kind);
    sqlite3_bind_int(insert_workset.st, 6, command.program_version);
    sqlite3_bind_text(
        insert_workset.st,
        7,
        compatibility.compatibility_key.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_text(
        insert_workset.st,
        8,
        compatibility.module_canonical_id.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_int(
        insert_workset.st,
        9,
        compatibility.module_version);
    sqlite3_bind_text(
        insert_workset.st,
        10,
        compatibility.module_sha256.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_text(
        insert_workset.st,
        11,
        compatibility.entrypoint.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_text(
        insert_workset.st,
        12,
        compatibility.verified_dependency_sha256.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_text(
        insert_workset.st,
        13,
        compatibility.runtime_profile_sha256.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_int64(
        insert_workset.st,
        14,
        static_cast<std::int64_t>(
            compatibility.required_capability_mask));
    BindOptionalText(
        insert_workset.st,
        15,
        compatibility.execution_affinity_key);
    BindOptionalText(
        insert_workset.st,
        16,
        compatibility.baseline_affinity_key);
    sqlite3_bind_int64(
        insert_workset.st,
        17,
        static_cast<std::int64_t>(
            compatibility.estimated_payload_bytes));
    sqlite3_bind_int(insert_workset.st, 18, command.priority);
    sqlite3_bind_int(
        insert_workset.st,
        19,
        static_cast<int>(command.ordered_job_ids.size()));
    sqlite3_bind_int64(insert_workset.st, 20, now);
    if (sqlite3_step(insert_workset.st) != SQLITE_DONE) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    receipt.workset_id = sqlite3_last_insert_rowid(db_);
    receipt.item_count =
        static_cast<int>(command.ordered_job_ids.size());

    Statement publish_job;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job "
            "SET workset_id=?1,workset_item_ordinal=?2,state='QUEUED',"
            "queued_at_utc=?3 "
            "WHERE job_id=?4 AND state='PENDING_WORKSET' "
            "AND workset_id IS NULL AND workset_item_ordinal IS NULL;",
            -1,
            &publish_job.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    for (std::size_t ordinal = 0;
         ordinal < command.ordered_job_ids.size();
         ++ordinal) {
        sqlite3_reset(publish_job.st);
        sqlite3_clear_bindings(publish_job.st);
        sqlite3_bind_int64(
            publish_job.st,
            1,
            receipt.workset_id);
        sqlite3_bind_int(
            publish_job.st,
            2,
            static_cast<int>(ordinal));
        sqlite3_bind_int64(publish_job.st, 3, now);
        sqlite3_bind_int64(
            publish_job.st,
            4,
            command.ordered_job_ids[ordinal]);
        if (sqlite3_step(publish_job.st) != SQLITE_DONE
            || sqlite3_changes(db_) != 1
            || !InsertJobActionEventAndOutbox(
                db_,
                command.ordered_job_ids[ordinal],
                command.job_set_id,
                "Execution.JobQueued.v1",
                "workset-published",
                error_out)) {
            fail(
                error_out != nullptr && !error_out->empty()
                ? *error_out
                : "workset membership publication CAS failed");
            return false;
        }
    }

    Statement update_job_set;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job_set "
            "SET materialization_state='PUBLISHING_WORKSETS' "
            "WHERE job_set_id=?1 AND materialization_state IN "
            "('POPULATION_SEALED','PUBLISHING_WORKSETS');",
            -1,
            &update_job_set.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(
        update_job_set.st,
        1,
        command.job_set_id);
    if (sqlite3_step(update_job_set.st) != SQLITE_DONE
        || sqlite3_changes(db_) != 1
        || !InsertAggregateOutboxEvent(
            db_,
            "Execution.WorksetPublished.v1",
            "workset",
            receipt.workset_id,
            "workset",
            receipt.workset_id,
            "workset-published-"
                + std::to_string(receipt.workset_id),
            error_out)
        || !RefreshExecutionWorkAvailability(db_, error_out)
        || !Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    receipt.disposition = ExecutionDbOperationDisposition::Applied;
    if (receipt_out != nullptr) *receipt_out = receipt;
    return true;
}

bool SqliteExecutionDb::CompleteWorksetPublication(
    const CompleteWorksetPublicationCommand& command,
    CompleteWorksetPublicationReceipt* receipt_out,
    std::string* error_out) {
    CompleteWorksetPublicationReceipt receipt{};
    if (receipt_out != nullptr) *receipt_out = receipt;
    if (db_ == nullptr
        || command.job_set_id <= 0
        || command.expected_workset_count < 0
        || command.expected_job_count < 0
        || command.requested_by.empty()) {
        receipt.disposition =
            ExecutionDbOperationDisposition::InvalidRequest;
        if (receipt_out != nullptr) *receipt_out = receipt;
        if (error_out != nullptr) {
            *error_out = "invalid complete-workset-publication command";
        }
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }
    const auto fail = [&](std::string message) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = std::move(message);
    };
    Statement counts;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT COALESCE(js.materialization_state,''),"
            "(SELECT COUNT(1) FROM exec_workset w "
            " WHERE w.job_set_id=js.job_set_id),"
            "(SELECT COUNT(1) FROM exec_job j "
            " WHERE j.job_set_id=js.job_set_id),"
            "(SELECT COUNT(1) FROM exec_job j "
            " WHERE j.job_set_id=js.job_set_id "
            "   AND (j.workset_id IS NULL "
            "        OR j.workset_item_ordinal IS NULL)) "
            "FROM exec_job_set js WHERE js.job_set_id=?1;",
            -1,
            &counts.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(counts.st, 1, command.job_set_id);
    if (sqlite3_step(counts.st) != SQLITE_ROW) {
        Rollback(db_);
        receipt.disposition =
            ExecutionDbOperationDisposition::Missing;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    receipt.materialization_state = Text(counts.st, 0);
    receipt.durable_workset_count = sqlite3_column_int(counts.st, 1);
    receipt.durable_job_count = sqlite3_column_int(counts.st, 2);
    const auto unassigned_jobs = sqlite3_column_int(counts.st, 3);
    if (receipt.materialization_state
            == "WORKSET_PUBLICATION_COMPLETE"
        && receipt.durable_workset_count
            == command.expected_workset_count
        && receipt.durable_job_count
            == command.expected_job_count
        && unassigned_jobs == 0) {
        if (!Commit(db_, error_out)) {
            Rollback(db_);
            return false;
        }
        receipt.disposition =
            ExecutionDbOperationDisposition::AlreadyApplied;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    if ((receipt.materialization_state != "POPULATION_SEALED"
            && receipt.materialization_state
                != "PUBLISHING_WORKSETS")
        || receipt.durable_workset_count
            != command.expected_workset_count
        || receipt.durable_job_count != command.expected_job_count
        || unassigned_jobs != 0) {
        Rollback(db_);
        receipt.disposition =
            ExecutionDbOperationDisposition::Conflict;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    const auto now = CurrentUtcMs(db_);
    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job_set "
            "SET materialization_state='WORKSET_PUBLICATION_COMPLETE',"
            "workset_publication_completed_at_utc=?1 "
            "WHERE job_set_id=?2 AND materialization_state IN "
            "('POPULATION_SEALED','PUBLISHING_WORKSETS');",
            -1,
            &update.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(update.st, 1, now);
    sqlite3_bind_int64(update.st, 2, command.job_set_id);
    if (sqlite3_step(update.st) != SQLITE_DONE
        || sqlite3_changes(db_) != 1
        || !InsertAggregateOutboxEvent(
            db_,
            "Execution.WorksetPublicationCompleted.v1",
            "job_set",
            command.job_set_id,
            "job_set",
            command.job_set_id,
            "workset-publication-complete-"
                + std::to_string(command.job_set_id),
            error_out)
        || !Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    receipt.disposition = ExecutionDbOperationDisposition::Applied;
    receipt.materialization_state =
        "WORKSET_PUBLICATION_COMPLETE";
    if (receipt_out != nullptr) *receipt_out = receipt;
    return true;
}

std::vector<ClaimedPublishedWorkset>
SqliteExecutionDb::ClaimPublishedWorksetBatch(
    const ClaimPublishedWorksetBatchCommand& command,
    std::string* error_out) {
    std::vector<ClaimedPublishedWorkset> claimed_worksets;
    if (error_out != nullptr) error_out->clear();
    if (db_ == nullptr
        || command.batch_nonce.empty()
        || command.requested_workset_count == 0) {
        if (error_out != nullptr) {
            *error_out = "invalid published-workset batch-claim command";
        }
        return claimed_worksets;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return claimed_worksets;
    }
    const auto fail = [&](std::string message) {
        Rollback(db_);
        claimed_worksets.clear();
        if (error_out != nullptr) *error_out = std::move(message);
    };

    struct Candidate {
        ClaimedPublishedWorkset workset;
        std::int64_t published_at_utc = 0;
        int runnable_count = 0;
    };
    std::vector<Candidate> selected;
    std::optional<int> absolute_priority;
    Statement candidates;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT "
            "w.workset_id,w.job_set_id,w.workflow_step_id,"
            "w.root_job_set_id,w.workset_key,w.program_kind,"
            "w.program_version,w.compatibility_key,"
            "w.module_canonical_id,w.module_version,w.module_sha256,"
            "w.entrypoint,w.verified_dependency_sha256,"
            "w.runtime_profile_sha256,w.required_capability_mask,"
            "w.execution_affinity_key,w.baseline_affinity_key,"
            "w.estimated_payload_bytes,w.priority,w.published_at_utc,"
            "(SELECT COUNT(1) FROM exec_job j "
            " WHERE j.workset_id=w.workset_id AND j.state='QUEUED' "
            "   AND j.attempts<j.max_attempts) "
            "FROM exec_workset w "
            "WHERE EXISTS("
            "  SELECT 1 FROM exec_job j "
            "  WHERE j.workset_id=w.workset_id AND j.state='QUEUED' "
            "    AND j.attempts<j.max_attempts"
            ") "
            "AND NOT EXISTS("
            "  SELECT 1 FROM exec_job j "
            "  WHERE j.workset_id=w.workset_id AND j.state='QUEUED' "
            "    AND j.attempts>=j.max_attempts"
            ") "
            "AND NOT EXISTS("
            "  SELECT 1 FROM exec_workset_dispatch_attempt d "
            "  WHERE d.workset_id=w.workset_id "
            "    AND d.state IN ('CLAIMED','ACTIVE','DRAINING')"
            ") "
            "ORDER BY w.priority DESC,w.published_at_utc ASC,"
            "w.workset_id ASC;",
            -1,
            &candidates.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return claimed_worksets;
    }
    for (;;) {
        const auto rc = sqlite3_step(candidates.st);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            fail(sqlite3_errmsg(db_));
            return claimed_worksets;
        }
        const auto priority = sqlite3_column_int(candidates.st, 18);
        if (!absolute_priority.has_value()) {
            absolute_priority = priority;
        } else if (priority != *absolute_priority) {
            break;
        }

        Candidate candidate{};
        auto& row = candidate.workset;
        row.workset_id = sqlite3_column_int64(candidates.st, 0);
        row.job_set_id = sqlite3_column_int64(candidates.st, 1);
        row.workflow_step_id = sqlite3_column_int64(candidates.st, 2);
        row.root_job_set_id = sqlite3_column_int64(candidates.st, 3);
        row.workset_key = Text(candidates.st, 4);
        row.program_kind = sqlite3_column_int(candidates.st, 5);
        row.program_version = sqlite3_column_int(candidates.st, 6);
        row.compatibility.compatibility_key = Text(candidates.st, 7);
        row.compatibility.module_canonical_id = Text(candidates.st, 8);
        row.compatibility.module_version =
            sqlite3_column_int(candidates.st, 9);
        row.compatibility.module_sha256 = Text(candidates.st, 10);
        row.compatibility.entrypoint = Text(candidates.st, 11);
        row.compatibility.verified_dependency_sha256 =
            Text(candidates.st, 12);
        row.compatibility.runtime_profile_sha256 =
            Text(candidates.st, 13);
        row.compatibility.required_capability_mask =
            static_cast<std::uint64_t>(
                sqlite3_column_int64(candidates.st, 14));
        row.compatibility.execution_affinity_key =
            OptionalText(candidates.st, 15);
        row.compatibility.baseline_affinity_key =
            OptionalText(candidates.st, 16);
        row.compatibility.estimated_payload_bytes =
            static_cast<std::uint64_t>(
                sqlite3_column_int64(candidates.st, 17));
        row.priority = priority;
        candidate.published_at_utc =
            sqlite3_column_int64(candidates.st, 19);
        candidate.runnable_count =
            sqlite3_column_int(candidates.st, 20);
        selected.push_back(std::move(candidate));
    }
    std::stable_sort(
        selected.begin(),
        selected.end(),
        [](const Candidate& lhs, const Candidate& rhs) {
            if (lhs.published_at_utc != rhs.published_at_utc) {
                return lhs.published_at_utc < rhs.published_at_utc;
            }
            return lhs.workset.workset_id < rhs.workset.workset_id;
        });
    if (selected.size() > command.requested_workset_count) {
        selected.resize(command.requested_workset_count);
    }
    if (selected.empty()) {
        if (!RefreshExecutionWorkAvailability(db_, error_out)
            || !Commit(db_, error_out)) {
            Rollback(db_);
        }
        return claimed_worksets;
    }

    const auto now = CurrentUtcMs(db_);
    if (now <= 0) {
        fail("failed reading workset claim time");
        return claimed_worksets;
    }
    claimed_worksets.reserve(selected.size());
    for (std::size_t selected_index = 0;
         selected_index < selected.size();
         ++selected_index) {
        auto claimed = std::move(selected[selected_index].workset);
        claimed.claim_token =
            command.batch_nonce + "-" + std::to_string(selected_index + 1);

        Statement sequence;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT COALESCE(MAX(dispatch_sequence),0)+1 "
                "FROM exec_workset_dispatch_attempt "
                "WHERE workset_id=?1;",
                -1,
                &sequence.st,
                nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return claimed_worksets;
        }
        sqlite3_bind_int64(sequence.st, 1, claimed.workset_id);
        if (sqlite3_step(sequence.st) != SQLITE_ROW) {
            fail(sqlite3_errmsg(db_));
            return claimed_worksets;
        }
        const auto dispatch_sequence =
            sqlite3_column_int64(sequence.st, 0);

        Statement insert_dispatch;
        if (sqlite3_prepare_v2(
                db_,
                "INSERT INTO exec_workset_dispatch_attempt("
                "workset_id,dispatch_sequence,state,claim_token,"
                "claimed_at_utc) "
                "VALUES(?1,?2,'CLAIMED',?3,?4);",
                -1,
                &insert_dispatch.st,
                nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return claimed_worksets;
        }
        sqlite3_bind_int64(insert_dispatch.st, 1, claimed.workset_id);
        sqlite3_bind_int64(insert_dispatch.st, 2, dispatch_sequence);
        sqlite3_bind_text(
            insert_dispatch.st,
            3,
            claimed.claim_token.c_str(),
            -1,
            SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert_dispatch.st, 4, now);
        if (sqlite3_step(insert_dispatch.st) != SQLITE_DONE) {
            fail(sqlite3_errmsg(db_));
            return claimed_worksets;
        }
        claimed.dispatch_attempt_id = sqlite3_last_insert_rowid(db_);

        Statement claim_jobs;
        if (sqlite3_prepare_v2(
                db_,
                "WITH ordered(job_id,runtime_ordinal) AS MATERIALIZED ("
                "  SELECT job_id,"
                "         ROW_NUMBER() OVER (ORDER BY workset_item_ordinal)-1 "
                "  FROM exec_job "
                "  WHERE workset_id=?3 AND state='QUEUED' "
                "    AND attempts<max_attempts"
                ") "
                "UPDATE exec_job "
                "SET state='CLAIMED',claimed_by_token=?1,"
                "lease_expires_at_utc=NULL,dispatch_attempt_id=?2,"
                "dispatch_item_ordinal=("
                "  SELECT runtime_ordinal FROM ordered "
                "  WHERE ordered.job_id=exec_job.job_id"
                "),reserved_attempt_id=attempts+1 "
                "WHERE job_id IN (SELECT job_id FROM ordered) "
                "AND workset_id=?3 AND state='QUEUED' "
                "AND attempts<max_attempts;",
                -1,
                &claim_jobs.st,
                nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return claimed_worksets;
        }
        sqlite3_bind_text(
            claim_jobs.st,
            1,
            claimed.claim_token.c_str(),
            -1,
            SQLITE_TRANSIENT);
        sqlite3_bind_int64(
            claim_jobs.st,
            2,
            claimed.dispatch_attempt_id);
        sqlite3_bind_int64(claim_jobs.st, 3, claimed.workset_id);
        const auto claim_rc = sqlite3_step(claim_jobs.st);
        const auto changed_jobs = sqlite3_changes(db_);
        if (claim_rc != SQLITE_DONE
            || changed_jobs != selected[selected_index].runnable_count) {
            fail(
                "whole-workset batch claim CAS failed for workset "
                + std::to_string(claimed.workset_id)
                + ": sqlite rc=" + std::to_string(claim_rc)
                + ", changed=" + std::to_string(changed_jobs)
                + ", expected="
                + std::to_string(
                    selected[selected_index].runnable_count)
                + ", diagnostic=" + sqlite3_errmsg(db_));
            return claimed_worksets;
        }

        Statement items;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT job_id,job_set_id,program_kind,program_version,"
                "program_ref_kind,program_ref_id,savestate_id,fingerprint,"
                "priority,attempts,max_attempts,queued_at_utc,input_ini,"
                "workset_item_ordinal,dispatch_item_ordinal,"
                "reserved_attempt_id "
                "FROM exec_job "
                "WHERE dispatch_attempt_id=?1 AND state='CLAIMED' "
                "ORDER BY workset_item_ordinal ASC;",
                -1,
                &items.st,
                nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return claimed_worksets;
        }
        sqlite3_bind_int64(items.st, 1, claimed.dispatch_attempt_id);
        for (;;) {
            const auto rc = sqlite3_step(items.st);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) {
                fail(sqlite3_errmsg(db_));
                return claimed_worksets;
            }
            ClaimedPublishedWorksetItem item{};
            item.job_id = sqlite3_column_int64(items.st, 0);
            item.job_set_id = sqlite3_column_int64(items.st, 1);
            item.program_kind = sqlite3_column_int(items.st, 2);
            item.program_version = sqlite3_column_int(items.st, 3);
            item.program_ref_kind = Text(items.st, 4);
            item.program_ref_id = sqlite3_column_int64(items.st, 5);
            if (sqlite3_column_type(items.st, 6) != SQLITE_NULL) {
                item.savestate_id = sqlite3_column_int64(items.st, 6);
            }
            item.fingerprint = Text(items.st, 7);
            item.priority = sqlite3_column_int(items.st, 8);
            item.attempts = sqlite3_column_int(items.st, 9);
            item.max_attempts = sqlite3_column_int(items.st, 10);
            item.queued_at_utc = sqlite3_column_int64(items.st, 11);
            item.input_ini = Text(items.st, 12);
            item.item_ordinal = sqlite3_column_int(items.st, 13);
            if (sqlite3_column_type(items.st, 14) == SQLITE_NULL
                || sqlite3_column_int64(items.st, 14) < 0
                || sqlite3_column_int64(items.st, 14)
                    > static_cast<sqlite3_int64>(
                        (std::numeric_limits<std::uint32_t>::max)())) {
                fail("claimed workset item has invalid runtime ordinal");
                return claimed_worksets;
            }
            item.dispatch_item_ordinal = static_cast<std::uint32_t>(
                sqlite3_column_int64(items.st, 14));
            item.reserved_attempt_id = static_cast<std::uint64_t>(
                sqlite3_column_int64(items.st, 15));
            if (!InsertJobActionEventAndOutbox(
                    db_,
                    item.job_id,
                    item.job_set_id,
                    "Execution.JobClaimed.v1",
                    "published-workset-batch-claimed",
                    error_out)) {
                fail(
                    error_out != nullptr
                        ? *error_out
                        : "job batch-claim event failed");
                return claimed_worksets;
            }
            claimed.items.push_back(std::move(item));
        }
        if (claimed.items.size()
                != static_cast<std::size_t>(
                    selected[selected_index].runnable_count)
            || !InsertAggregateOutboxEvent(
                db_,
                "Execution.WorksetClaimed.v1",
                "workset",
                claimed.workset_id,
                "workset",
                claimed.workset_id,
                "dispatch-" + std::to_string(claimed.dispatch_attempt_id),
                error_out)) {
            fail(
                error_out != nullptr
                    ? *error_out
                    : "workset batch-claim event failed");
            return claimed_worksets;
        }
        claimed_worksets.push_back(std::move(claimed));
    }
    if (!RefreshExecutionWorkAvailability(db_, error_out)
        || !Commit(db_, error_out)) {
        Rollback(db_);
        claimed_worksets.clear();
    }
    return claimed_worksets;
}

std::vector<WorksetDispatchLeaseReceipt>
SqliteExecutionDb::RenewActiveWorksetLeases(
    const RenewActiveWorksetLeasesCommand& command,
    std::string* error_out) {
    std::vector<WorksetDispatchLeaseReceipt> receipts;
    receipts.reserve(command.requests.size());
    if (error_out != nullptr) error_out->clear();
    if (db_ == nullptr || command.requests.empty()
        || command.lease_duration_ms <= 0) {
        if (error_out != nullptr) {
            *error_out = "invalid active workset lease renewal batch";
        }
        return receipts;
    }
    for (const auto& request : command.requests) {
        receipts.push_back({
            .disposition = ExecutionDbOperationDisposition::BackendError,
            .dispatch_attempt_id = request.dispatch_attempt_id,
        });
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return receipts;
    }
    const auto fail = [&](std::string message) {
        Rollback(db_);
        for (auto& receipt : receipts) {
            receipt.disposition =
                ExecutionDbOperationDisposition::BackendError;
            receipt.lease_expires_at_utc.reset();
        }
        if (error_out != nullptr) *error_out = std::move(message);
    };
    const auto now = CurrentUtcMs(db_);
    if (now <= 0
        || command.lease_duration_ms
            > std::numeric_limits<std::int64_t>::max() - now) {
        fail("workset lease expiration overflow");
        return receipts;
    }
    const auto next = now + command.lease_duration_ms;
    for (std::size_t index = 0; index < command.requests.size(); ++index) {
        const auto& request = command.requests[index];
        auto& receipt = receipts[index];
        if (request.dispatch_attempt_id <= 0
            || request.claim_token.empty()) {
            receipt.disposition =
                ExecutionDbOperationDisposition::InvalidRequest;
            continue;
        }
        Statement update_dispatch;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE exec_workset_dispatch_attempt "
                "SET lease_expires_at_utc=?1 "
                "WHERE dispatch_attempt_id=?2 AND claim_token=?3 "
                "AND state='ACTIVE';",
                -1,
                &update_dispatch.st,
                nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return receipts;
        }
        sqlite3_bind_int64(update_dispatch.st, 1, next);
        sqlite3_bind_int64(
            update_dispatch.st, 2, request.dispatch_attempt_id);
        sqlite3_bind_text(
            update_dispatch.st, 3, request.claim_token.c_str(), -1,
            SQLITE_TRANSIENT);
        if (sqlite3_step(update_dispatch.st) != SQLITE_DONE) {
            fail(sqlite3_errmsg(db_));
            return receipts;
        }
        if (sqlite3_changes(db_) != 1) {
            Statement current;
            if (sqlite3_prepare_v2(
                    db_,
                    "SELECT state,claim_token,lease_expires_at_utc "
                    "FROM exec_workset_dispatch_attempt "
                    "WHERE dispatch_attempt_id=?1;",
                    -1,
                    &current.st,
                    nullptr) != SQLITE_OK) {
                fail(sqlite3_errmsg(db_));
                return receipts;
            }
            sqlite3_bind_int64(
                current.st, 1, request.dispatch_attempt_id);
            if (sqlite3_step(current.st) != SQLITE_ROW) {
                receipt.disposition =
                    ExecutionDbOperationDisposition::Missing;
            } else {
                const auto state = Text(current.st, 0);
                const auto token = Text(current.st, 1);
                receipt.disposition = token != request.claim_token
                    ? ExecutionDbOperationDisposition::TokenMismatch
                    : state == "CLOSED"
                        ? ExecutionDbOperationDisposition::AlreadyApplied
                        : ExecutionDbOperationDisposition::WrongState;
            }
            continue;
        }
        receipt.disposition = ExecutionDbOperationDisposition::Applied;
        receipt.lease_expires_at_utc = next;
    }
    if (!Commit(db_, error_out)) {
        Rollback(db_);
        for (auto& receipt : receipts) {
            receipt.disposition = ExecutionDbOperationDisposition::BackendError;
            receipt.lease_expires_at_utc.reset();
        }
    }
    return receipts;
}

std::optional<ExecutionWorkAvailabilitySnapshot>
SqliteExecutionDb::GetExecutionWorkAvailability(
    std::string* error_out) const {
    if (error_out != nullptr) error_out->clear();
    if (db_ == nullptr) {
        if (error_out != nullptr) *error_out = "database handle is null";
        return std::nullopt;
    }
    Statement snapshot;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT generation,has_ready_worksets,"
            "has_execution_finished_results,"
            "changed_at_utc FROM exec_work_availability WHERE singleton_id=1;",
            -1,
            &snapshot.st,
            nullptr) != SQLITE_OK
        || sqlite3_step(snapshot.st) != SQLITE_ROW) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return std::nullopt;
    }
    const auto generation = sqlite3_column_int64(snapshot.st, 0);
    if (generation <= 0) {
        if (error_out != nullptr) {
            *error_out = "execution-work generation is invalid";
        }
        return std::nullopt;
    }
    return ExecutionWorkAvailabilitySnapshot{
        .generation = static_cast<std::uint64_t>(generation),
        .has_ready_worksets = sqlite3_column_int(snapshot.st, 1) != 0,
        .has_execution_finished_results =
            sqlite3_column_int(snapshot.st, 2) != 0,
        .changed_at_utc = sqlite3_column_int64(snapshot.st, 3),
    };
}

bool SqliteExecutionDb::MarkWorksetActive(
    const MarkWorksetActiveCommand& command,
    WorksetDispatchMutationReceipt* receipt_out,
    std::string* error_out) {
    WorksetDispatchMutationReceipt receipt{};
    receipt.dispatch_attempt_id = command.dispatch_attempt_id;
    if (receipt_out != nullptr) *receipt_out = receipt;
    if (db_ == nullptr
        || command.dispatch_attempt_id <= 0
        || command.claim_token.empty()
        || command.lease_duration_ms <= 0
        || command.requested_by.empty()) {
        receipt.disposition =
            ExecutionDbOperationDisposition::InvalidRequest;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }
    const auto now = CurrentUtcMs(db_);
    if (now <= 0
        || command.lease_duration_ms
            > std::numeric_limits<std::int64_t>::max() - now) {
        Rollback(db_);
        if (error_out != nullptr) {
            *error_out = "active workset lease duration overflow";
        }
        return false;
    }
    const auto lease_expires = now + command.lease_duration_ms;
    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_workset_dispatch_attempt "
            "SET state='ACTIVE',dispatched_at_utc=?1,"
            "lease_expires_at_utc=?2 "
            "WHERE dispatch_attempt_id=?3 AND claim_token=?4 "
            "AND state='CLAIMED';",
            -1,
            &update.st,
            nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(update.st, 1, now);
    sqlite3_bind_int64(update.st, 2, lease_expires);
    sqlite3_bind_int64(update.st, 3, command.dispatch_attempt_id);
    sqlite3_bind_text(
        update.st,
        4,
        command.claim_token.c_str(),
        -1,
        SQLITE_TRANSIENT);
    if (sqlite3_step(update.st) != SQLITE_DONE) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_changes(db_) != 1) {
        Statement current;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT state,claim_token,lease_expires_at_utc "
                "FROM exec_workset_dispatch_attempt "
                "WHERE dispatch_attempt_id=?1;",
                -1,
                &current.st,
                nullptr)
            != SQLITE_OK) {
            Rollback(db_);
            if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(
            current.st,
            1,
            command.dispatch_attempt_id);
        if (sqlite3_step(current.st) != SQLITE_ROW) {
            receipt.disposition =
                ExecutionDbOperationDisposition::Missing;
        } else {
            const auto current_state = Text(current.st, 0);
            const auto current_token = Text(current.st, 1);
            if (sqlite3_column_type(current.st, 2) != SQLITE_NULL) {
                receipt.lease_expires_at_utc =
                    sqlite3_column_int64(current.st, 2);
            }
            receipt.disposition =
                current_token != command.claim_token
                ? ExecutionDbOperationDisposition::TokenMismatch
                : current_state == "ACTIVE"
                    || current_state == "DRAINING"
                    || current_state == "CLOSED"
                    ? ExecutionDbOperationDisposition::AlreadyApplied
                    : ExecutionDbOperationDisposition::WrongState;
        }
        if (!Commit(db_, error_out)) {
            Rollback(db_);
            return false;
        }
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    Statement identity;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT d.workset_id,w.job_set_id "
            "FROM exec_workset_dispatch_attempt d "
            "JOIN exec_workset w ON w.workset_id=d.workset_id "
            "WHERE d.dispatch_attempt_id=?1;",
            -1,
            &identity.st,
            nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(identity.st, 1, command.dispatch_attempt_id);
    if (sqlite3_step(identity.st) != SQLITE_ROW
        || !InsertAggregateOutboxEvent(
            db_,
            "Execution.WorksetDispatched.v1",
            "workset",
            sqlite3_column_int64(identity.st, 0),
            "workset",
            sqlite3_column_int64(identity.st, 0),
            "dispatch-"
                + std::to_string(command.dispatch_attempt_id),
            error_out)
        || !Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    receipt.disposition = ExecutionDbOperationDisposition::Applied;
    receipt.lease_expires_at_utc = lease_expires;
    if (receipt_out != nullptr) *receipt_out = receipt;
    return true;
}

bool SqliteExecutionDb::MarkWorksetDraining(
    const MarkWorksetDrainingCommand& command,
    WorksetDispatchMutationReceipt* receipt_out,
    std::string* error_out) {
    WorksetDispatchMutationReceipt receipt{};
    receipt.dispatch_attempt_id = command.dispatch_attempt_id;
    if (receipt_out != nullptr) *receipt_out = receipt;
    if (db_ == nullptr
        || command.dispatch_attempt_id <= 0
        || command.claim_token.empty()
        || command.requested_by.empty()) {
        receipt.disposition =
            ExecutionDbOperationDisposition::InvalidRequest;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }
    const auto now = CurrentUtcMs(db_);
    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_workset_dispatch_attempt "
            "SET state='DRAINING',draining_at_utc=?1,"
            "lease_expires_at_utc=NULL "
            "WHERE dispatch_attempt_id=?2 AND claim_token=?3 "
            "AND state='ACTIVE';",
            -1,
            &update.st,
            nullptr) != SQLITE_OK) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(update.st, 1, now);
    sqlite3_bind_int64(update.st, 2, command.dispatch_attempt_id);
    sqlite3_bind_text(
        update.st, 3, command.claim_token.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(update.st) != SQLITE_DONE) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_changes(db_) != 1) {
        Statement current;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT state,claim_token "
                "FROM exec_workset_dispatch_attempt "
                "WHERE dispatch_attempt_id=?1;",
                -1,
                &current.st,
                nullptr) != SQLITE_OK) {
            Rollback(db_);
            if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(current.st, 1, command.dispatch_attempt_id);
        if (sqlite3_step(current.st) != SQLITE_ROW) {
            receipt.disposition = ExecutionDbOperationDisposition::Missing;
        } else if (Text(current.st, 1) != command.claim_token) {
            receipt.disposition =
                ExecutionDbOperationDisposition::TokenMismatch;
        } else {
            const auto state = Text(current.st, 0);
            receipt.disposition = state == "DRAINING" || state == "CLOSED"
                ? ExecutionDbOperationDisposition::AlreadyApplied
                : ExecutionDbOperationDisposition::WrongState;
        }
        if (!Commit(db_, error_out)) {
            Rollback(db_);
            return false;
        }
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    Statement remaining;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT COUNT(1) FROM exec_job "
            "WHERE dispatch_attempt_id=?1 "
            "AND state IN ('CLAIMED','RUNNING');",
            -1,
            &remaining.st,
            nullptr) != SQLITE_OK) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(remaining.st, 1, command.dispatch_attempt_id);
    if (sqlite3_step(remaining.st) != SQLITE_ROW) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_column_int(remaining.st, 0) == 0) {
        Statement close;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE exec_workset_dispatch_attempt "
                "SET state='CLOSED',closed_at_utc=?1,"
                "close_reason_code='WORKER_TERMINALS_STAGED' "
                "WHERE dispatch_attempt_id=?2 AND state='DRAINING';",
                -1,
                &close.st,
                nullptr) != SQLITE_OK) {
            Rollback(db_);
            if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(close.st, 1, now);
        sqlite3_bind_int64(close.st, 2, command.dispatch_attempt_id);
        if (sqlite3_step(close.st) != SQLITE_DONE) {
            Rollback(db_);
            if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        receipt.dispatch_closed = sqlite3_changes(db_) == 1;
    }
    receipt.disposition = ExecutionDbOperationDisposition::Applied;
    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    if (receipt_out != nullptr) *receipt_out = receipt;
    return true;
}

bool SqliteExecutionDb::ReleaseWorksetDispatch(
    const ReleaseWorksetDispatchCommand& command,
    WorksetDispatchMutationReceipt* receipt_out,
    std::string* error_out) {
    WorksetDispatchMutationReceipt receipt{};
    receipt.dispatch_attempt_id = command.dispatch_attempt_id;
    if (receipt_out != nullptr) *receipt_out = receipt;
    if (db_ == nullptr
        || command.dispatch_attempt_id <= 0
        || command.claim_token.empty()
        || command.reason_code.empty()
        || command.requested_by.empty()) {
        receipt.disposition =
            ExecutionDbOperationDisposition::InvalidRequest;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }
    const auto fail = [&](std::string message) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = std::move(message);
    };
    Statement dispatch;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT d.state,d.claim_token,d.workset_id,w.job_set_id "
            "FROM exec_workset_dispatch_attempt d "
            "JOIN exec_workset w ON w.workset_id=d.workset_id "
            "WHERE d.dispatch_attempt_id=?1;",
            -1,
            &dispatch.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(
        dispatch.st,
        1,
        command.dispatch_attempt_id);
    if (sqlite3_step(dispatch.st) != SQLITE_ROW) {
        Rollback(db_);
        receipt.disposition =
            ExecutionDbOperationDisposition::Missing;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    const auto dispatch_state = Text(dispatch.st, 0);
    const auto durable_token = Text(dispatch.st, 1);
    const auto workset_id = sqlite3_column_int64(dispatch.st, 2);
    const auto job_set_id = sqlite3_column_int64(dispatch.st, 3);
    if (durable_token != command.claim_token) {
        Rollback(db_);
        receipt.disposition =
            ExecutionDbOperationDisposition::TokenMismatch;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    if (dispatch_state == "CLOSED") {
        if (!Commit(db_, error_out)) {
            Rollback(db_);
            return false;
        }
        receipt.disposition =
            ExecutionDbOperationDisposition::AlreadyApplied;
        receipt.dispatch_closed = true;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }

    struct ReleasableJob {
        std::int64_t job_id = 0;
        std::int64_t job_set_id = 0;
        std::string state;
        int attempts = 0;
        int max_attempts = 0;
    };
    std::vector<ReleasableJob> jobs;
    Statement list_jobs;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT job_id,job_set_id,state,attempts,max_attempts "
            "FROM exec_job WHERE dispatch_attempt_id=?1 "
            "AND state IN ('CLAIMED','RUNNING') "
            "ORDER BY workset_item_ordinal ASC;",
            -1,
            &list_jobs.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(
        list_jobs.st,
        1,
        command.dispatch_attempt_id);
    for (;;) {
        const auto rc = sqlite3_step(list_jobs.st);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        jobs.push_back({
            .job_id = sqlite3_column_int64(list_jobs.st, 0),
            .job_set_id = sqlite3_column_int64(list_jobs.st, 1),
            .state = Text(list_jobs.st, 2),
            .attempts = sqlite3_column_int(list_jobs.st, 3),
            .max_attempts = sqlite3_column_int(list_jobs.st, 4),
        });
    }

    const auto now = CurrentUtcMs(db_);
    Statement requeue;
    Statement fail_job;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job SET state='QUEUED',queued_at_utc=?1,"
            "claimed_by_token=NULL,lease_expires_at_utc=NULL,"
            "dispatch_attempt_id=NULL,dispatch_item_ordinal=NULL,"
            "reserved_attempt_id=NULL "
            "WHERE job_id=?2 AND dispatch_attempt_id=?3 "
            "AND state IN ('CLAIMED','RUNNING');",
            -1,
            &requeue.st,
            nullptr)
            != SQLITE_OK
        || sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job SET state='FAILED',ended_at_utc=?1,"
            "error_code=?2,error_text=?3,claimed_by_token=NULL,"
            "lease_expires_at_utc=NULL,dispatch_attempt_id=NULL,"
            "dispatch_item_ordinal=NULL,reserved_attempt_id=NULL "
            "WHERE job_id=?4 AND dispatch_attempt_id=?5 "
            "AND state='RUNNING';",
            -1,
            &fail_job.st,
            nullptr)
            != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    for (const auto& job : jobs) {
        const bool exhausted = job.state == "RUNNING"
            && job.attempts >= job.max_attempts;
        auto* statement = exhausted ? fail_job.st : requeue.st;
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
        sqlite3_bind_int64(statement, 1, now);
        if (exhausted) {
            sqlite3_bind_text(
                statement,
                2,
                command.reason_code.c_str(),
                -1,
                SQLITE_TRANSIENT);
            BindOptionalText(statement, 3, command.reason_text);
            sqlite3_bind_int64(statement, 4, job.job_id);
            sqlite3_bind_int64(
                statement,
                5,
                command.dispatch_attempt_id);
        } else {
            sqlite3_bind_int64(statement, 2, job.job_id);
            sqlite3_bind_int64(
                statement,
                3,
                command.dispatch_attempt_id);
        }
        if (sqlite3_step(statement) != SQLITE_DONE
            || sqlite3_changes(db_) != 1) {
            fail("workset job release CAS failed");
            return false;
        }
        if (exhausted) {
            ++receipt.jobs_failed;
            if (!InsertJobActionEventAndOutbox(
                    db_,
                    job.job_id,
                    job.job_set_id,
                    "Execution.JobCompleted.v1",
                    "workset-release-attempts-exhausted",
                    error_out)) {
                fail(error_out != nullptr
                    ? *error_out
                    : "job completion event failed");
                return false;
            }
        } else {
            ++receipt.jobs_requeued;
            if (!InsertJobActionEventAndOutbox(
                    db_,
                    job.job_id,
                    job.job_set_id,
                    "Execution.JobClaimRequeued.v1",
                    "workset-released",
                    error_out)) {
                fail(error_out != nullptr
                    ? *error_out
                    : "job requeue event failed");
                return false;
            }
        }
    }

    Statement close;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_workset_dispatch_attempt "
            "SET state='CLOSED',closed_at_utc=?1,"
            "close_reason_code=?2,close_reason_text=?3,"
            "lease_expires_at_utc=NULL "
            "WHERE dispatch_attempt_id=?4 AND claim_token=?5 "
            "AND state IN ('CLAIMED','ACTIVE','DRAINING');",
            -1,
            &close.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(close.st, 1, now);
    sqlite3_bind_text(
        close.st,
        2,
        command.reason_code.c_str(),
        -1,
        SQLITE_TRANSIENT);
    BindOptionalText(close.st, 3, command.reason_text);
    sqlite3_bind_int64(
        close.st,
        4,
        command.dispatch_attempt_id);
    sqlite3_bind_text(
        close.st,
        5,
        command.claim_token.c_str(),
        -1,
        SQLITE_TRANSIENT);
    if (sqlite3_step(close.st) != SQLITE_DONE
        || sqlite3_changes(db_) != 1
        || !InsertAggregateOutboxEvent(
            db_,
            "Execution.WorksetReleased.v1",
            "workset",
            workset_id,
            "workset",
            workset_id,
            "dispatch-release-"
                + std::to_string(command.dispatch_attempt_id),
            error_out)
        || !RefreshExecutionWorkAvailability(db_, error_out)
        || !Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    receipt.disposition = ExecutionDbOperationDisposition::Applied;
    receipt.dispatch_closed = true;
    if (receipt_out != nullptr) *receipt_out = receipt;
    return true;
}

bool SqliteExecutionDb::PersistWorkerExecutionEventsBatch(
    const PersistWorkerExecutionEventsBatchCommand& command,
    PersistWorkerExecutionEventsBatchReceipt* receipt_out,
    std::string* error_out) {
    PersistWorkerExecutionEventsBatchReceipt receipt{};
    if (receipt_out != nullptr) *receipt_out = receipt;
    if (db_ == nullptr || command.events.empty()) {
        if (error_out != nullptr) *error_out = "invalid worker execution-event batch";
        return false;
    }
    BatchTransactionScope batch(db_);
    if (!batch.Begin(error_out)) return false;
    receipt.events.reserve(command.events.size());
    for (const auto& event : command.events) {
        bool ok = false;
        if (const auto* started =
                std::get_if<MarkWorksetJobStartedCommand>(&event)) {
            WorksetJobStartReceipt item_receipt{};
            ok = MarkWorksetJobStarted(started[0], &item_receipt, error_out);
            receipt.events.emplace_back(std::move(item_receipt));
        } else {
            StageWorkerTerminalReceipt item_receipt{};
            ok = StageWorkerTerminal(
                std::get<StageWorkerTerminalCommand>(event),
                &item_receipt,
                error_out);
            receipt.events.emplace_back(std::move(item_receipt));
        }
        if (!ok) return false;
    }
    if (!batch.CommitAll(error_out)) return false;
    if (receipt_out != nullptr) *receipt_out = std::move(receipt);
    return true;
}

bool SqliteExecutionDb::MarkWorksetJobStarted(
    const MarkWorksetJobStartedCommand& command,
    WorksetJobStartReceipt* receipt_out,
    std::string* error_out) {
    WorksetJobStartReceipt receipt{};
    receipt.job_id = command.job_id;
    if (receipt_out != nullptr) *receipt_out = receipt;
    if (db_ == nullptr
        || command.dispatch_attempt_id <= 0
        || command.claim_token.empty()
        || command.job_id <= 0
        || command.reserved_attempt_id == 0
        || command.reserved_attempt_id
            > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())
        || command.worker_invocation_id.empty()
        || command.requested_by.empty()) {
        receipt.disposition =
            ExecutionDbOperationDisposition::InvalidRequest;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }
    const auto now = CurrentUtcMs(db_);
    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job "
            "SET state='RUNNING',attempts=?1,"
            "started_at_utc=COALESCE(started_at_utc,?2) "
            "WHERE job_id=?3 AND state='CLAIMED' "
            "AND dispatch_attempt_id=?4 "
            "AND claimed_by_token=?5 "
            "AND dispatch_item_ordinal=?6 "
            "AND reserved_attempt_id=?1 "
            "AND attempts=?1-1 "
            "AND EXISTS("
            "  SELECT 1 FROM exec_workset_dispatch_attempt d "
            "  WHERE d.dispatch_attempt_id=?4 "
            "    AND d.claim_token=?5 "
            "    AND d.state IN ('ACTIVE','DRAINING')"
            ");",
            -1,
            &update.st,
            nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(
        update.st,
        1,
        static_cast<std::int64_t>(
            command.reserved_attempt_id));
    sqlite3_bind_int64(update.st, 2, now);
    sqlite3_bind_int64(update.st, 3, command.job_id);
    sqlite3_bind_int64(
        update.st,
        4,
        command.dispatch_attempt_id);
    sqlite3_bind_text(
        update.st,
        5,
        command.claim_token.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_int64(
        update.st,
        6,
        static_cast<sqlite3_int64>(
            command.dispatch_item_ordinal));
    if (sqlite3_step(update.st) != SQLITE_DONE) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_changes(db_) != 1) {
        Statement existing;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT j.state,j.attempts,j.dispatch_attempt_id,"
                "j.claimed_by_token,j.dispatch_item_ordinal,"
                "j.reserved_attempt_id,d.dispatch_attempt_id,"
                "d.state,d.claim_token,d.lease_expires_at_utc "
                "FROM exec_job j "
                "LEFT JOIN exec_workset_dispatch_attempt d "
                "  ON d.dispatch_attempt_id=j.dispatch_attempt_id "
                "WHERE j.job_id=?1;",
                -1,
                &existing.st,
                nullptr)
            != SQLITE_OK) {
            Rollback(db_);
            if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(existing.st, 1, command.job_id);
        const auto existing_rc = sqlite3_step(existing.st);
        if (existing_rc == SQLITE_DONE) {
            Rollback(db_);
            receipt.disposition =
                ExecutionDbOperationDisposition::Missing;
            if (receipt_out != nullptr) *receipt_out = receipt;
            return true;
        }
        if (existing_rc != SQLITE_ROW) {
            Rollback(db_);
            if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
            return false;
        }

        const auto job_state = Text(existing.st, 0);
        const auto durable_attempts =
            sqlite3_column_int64(existing.st, 1);
        const bool has_durable_dispatch =
            sqlite3_column_type(existing.st, 2) != SQLITE_NULL;
        const auto durable_dispatch =
            sqlite3_column_int64(existing.st, 2);
        const auto durable_token = Text(existing.st, 3);
        const bool has_durable_ordinal =
            sqlite3_column_type(existing.st, 4) != SQLITE_NULL;
        const auto durable_ordinal =
            sqlite3_column_int64(existing.st, 4);
        const bool has_reserved_attempt =
            sqlite3_column_type(existing.st, 5) != SQLITE_NULL;
        const auto durable_reserved_attempt =
            static_cast<std::uint64_t>(
                sqlite3_column_int64(existing.st, 5));
        const bool has_dispatch_authority =
            sqlite3_column_type(existing.st, 6) != SQLITE_NULL;
        const auto dispatch_state = Text(existing.st, 7);
        const auto dispatch_token = Text(existing.st, 8);
        if (job_state == "RUNNING"
            && static_cast<std::uint64_t>(
                durable_attempts)
                == command.reserved_attempt_id
            && has_durable_dispatch
            && durable_dispatch == command.dispatch_attempt_id
            && durable_token == command.claim_token
            && has_durable_ordinal
            && static_cast<std::uint64_t>(
                durable_ordinal)
                == command.dispatch_item_ordinal
            && has_reserved_attempt
            && durable_reserved_attempt
                == command.reserved_attempt_id) {
            if (!Commit(db_, error_out)) {
                Rollback(db_);
                return false;
            }
            receipt.disposition =
                ExecutionDbOperationDisposition::AlreadyApplied;
            receipt.durable_attempt_id =
                command.reserved_attempt_id;
            if (receipt_out != nullptr) *receipt_out = receipt;
            return true;
        }

        const bool token_mismatch =
            durable_token != command.claim_token
            || (has_dispatch_authority
                && dispatch_token != command.claim_token);
        const bool attempt_mismatch =
            !has_reserved_attempt
            || durable_reserved_attempt
                != command.reserved_attempt_id
            || (job_state == "CLAIMED"
                && durable_attempts
                    != static_cast<std::int64_t>(
                        command.reserved_attempt_id)
                        - 1)
            || (job_state == "RUNNING"
                && durable_attempts
                    != static_cast<std::int64_t>(
                        command.reserved_attempt_id));
        const bool identity_or_state_mismatch =
            job_state != "CLAIMED"
            || !has_durable_dispatch
            || durable_dispatch != command.dispatch_attempt_id
            || !has_durable_ordinal
            || static_cast<std::uint64_t>(durable_ordinal)
                != command.dispatch_item_ordinal
            || !has_dispatch_authority
            || (dispatch_state != "ACTIVE"
                && dispatch_state != "DRAINING");

        Rollback(db_);
        receipt.disposition =
            token_mismatch
            ? ExecutionDbOperationDisposition::TokenMismatch
            : attempt_mismatch
                ? ExecutionDbOperationDisposition::AttemptMismatch
                : identity_or_state_mismatch
                    ? ExecutionDbOperationDisposition::WrongState
                    : ExecutionDbOperationDisposition::WrongState;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }

    Statement job_set;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT job_set_id FROM exec_job WHERE job_id=?1;",
            -1,
            &job_set.st,
            nullptr)
            != SQLITE_OK) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(job_set.st, 1, command.job_id);
    if (sqlite3_step(job_set.st) != SQLITE_ROW
        || !InsertJobActionEventAndOutbox(
            db_,
            command.job_id,
            sqlite3_column_int64(job_set.st, 0),
            "Execution.JobStarted.v1",
            "workset-item-started",
            error_out)
        || !Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    receipt.disposition = ExecutionDbOperationDisposition::Applied;
    receipt.durable_attempt_id = command.reserved_attempt_id;
    if (receipt_out != nullptr) *receipt_out = receipt;
    return true;
}

bool SqliteExecutionDb::StageWorkerTerminal(
    const StageWorkerTerminalCommand& command,
    StageWorkerTerminalReceipt* receipt_out,
    std::string* error_out) {
    StageWorkerTerminalReceipt receipt{};
    receipt.job_id = command.job_id;
    if (receipt_out != nullptr) *receipt_out = receipt;
    const bool valid_terminal_status =
        command.terminal_status == "SUCCEEDED"
        || command.terminal_status == "FAILED"
        || command.terminal_status == "CANCELLED"
        || command.terminal_status == "INFRASTRUCTURE_FAILURE"
        || command.terminal_status == "CLEANUP_FAILURE"
        || command.terminal_status == "TIMED_OUT";
    if (db_ == nullptr
        || command.dispatch_attempt_id <= 0
        || command.claim_token.empty()
        || command.job_id <= 0
        || command.reserved_attempt_id == 0
        || command.reserved_attempt_id
            > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())
        || !valid_terminal_status
        || command.terminal_fingerprint.empty()
        || command.terminal_id.empty()
        || command.requested_by.empty()
        || !IsSafeRelativeBlobPath(
            command.result_blob.relative_path)
        || !IsSha256(command.result_blob.sha256)
        || command.terminal_fingerprint
            != command.result_blob.sha256
        || command.result_blob.format.empty()
        || command.result_blob.size_bytes == 0
        || command.result_blob.size_bytes
            > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
        receipt.disposition =
            ExecutionDbOperationDisposition::InvalidRequest;
        if (receipt_out != nullptr) *receipt_out = receipt;
        if (error_out != nullptr) {
            *error_out = "invalid worker-terminal staging command";
        }
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }
    const auto fail = [&](std::string message) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = std::move(message);
    };
    Statement job;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT j.job_set_id,j.state,j.attempts,j.max_attempts,"
            "j.dispatch_attempt_id,j.reserved_attempt_id,"
            "j.claimed_by_token,j.worker_terminal_fingerprint,"
            "j.worker_result_blob_id,d.state,d.lease_expires_at_utc,"
            "j.workset_id,b.sha256,b.relative_path,b.size_bytes,b.format,"
            "j.worker_terminal_status,j.worker_terminal_id,"
            "j.worker_terminal_error_code,j.worker_terminal_error_text,"
            "j.worker_terminal_unstarted,d.claim_token,"
            "j.dispatch_item_ordinal "
            "FROM exec_job j "
            "LEFT JOIN exec_workset_dispatch_attempt d "
            "  ON d.dispatch_attempt_id=j.dispatch_attempt_id "
            "LEFT JOIN exec_temp_blob b "
            "  ON b.temp_blob_id=j.worker_result_blob_id "
            "WHERE j.job_id=?1;",
            -1,
            &job.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(job.st, 1, command.job_id);
    if (sqlite3_step(job.st) != SQLITE_ROW) {
        Rollback(db_);
        receipt.disposition =
            ExecutionDbOperationDisposition::Missing;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    const auto job_set_id = sqlite3_column_int64(job.st, 0);
    const auto job_state = Text(job.st, 1);
    const auto attempts = sqlite3_column_int(job.st, 2);
    const auto durable_dispatch = sqlite3_column_int64(job.st, 4);
    const auto reserved_attempt =
        static_cast<std::uint64_t>(
            sqlite3_column_int64(job.st, 5));
    const auto durable_token = Text(job.st, 6);
    const auto existing_fingerprint = OptionalText(job.st, 7);
    const auto existing_blob_id =
        sqlite3_column_type(job.st, 8) != SQLITE_NULL
        ? std::optional<std::int64_t>(
            sqlite3_column_int64(job.st, 8))
        : std::nullopt;
    const auto dispatch_state = Text(job.st, 9);
    const auto workset_id = sqlite3_column_int64(job.st, 11);
    receipt.durable_job_state = job_state;

    if ((job_state == "EXECUTION_FINISHED"
            || job_state == "SUCCEEDED"
            || job_state == "FAILED"
            || job_state == "CANCELED"
            || job_state == "SUPERSEDED")
        && existing_fingerprint
            == std::optional<std::string>(
                command.terminal_fingerprint)
        && existing_blob_id.has_value()
        && Text(job.st, 12) == command.result_blob.sha256
        && Text(job.st, 13) == command.result_blob.relative_path
        && static_cast<std::uint64_t>(
            sqlite3_column_int64(job.st, 14))
            == command.result_blob.size_bytes
        && Text(job.st, 15) == command.result_blob.format
        && Text(job.st, 16) == command.terminal_status
        && Text(job.st, 17) == command.terminal_id
        && OptionalText(job.st, 18) == command.error_code
        && OptionalText(job.st, 19) == command.error_text
        && (sqlite3_column_int(job.st, 20) != 0)
            == command.unstarted
        && Text(job.st, 21) == command.claim_token
        && sqlite3_column_type(job.st, 22) != SQLITE_NULL
        && static_cast<std::uint64_t>(
            sqlite3_column_int64(job.st, 22))
            == command.dispatch_item_ordinal
        && durable_dispatch == command.dispatch_attempt_id
        && reserved_attempt == command.reserved_attempt_id) {
        if (!Commit(db_, error_out)) {
            Rollback(db_);
            return false;
        }
        receipt.disposition =
            ExecutionDbOperationDisposition::AlreadyApplied;
        receipt.temp_blob_id = *existing_blob_id;
        receipt.durable_job_state = job_state;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    const auto now = CurrentUtcMs(db_);
    const bool valid_started = !command.unstarted
        && job_state == "RUNNING"
        && static_cast<std::uint64_t>(attempts)
            == command.reserved_attempt_id;
    const bool valid_unstarted = command.unstarted
        && job_state == "CLAIMED"
        && static_cast<std::uint64_t>(attempts + 1)
            == command.reserved_attempt_id;
    if ((!valid_started && !valid_unstarted)
        || durable_dispatch != command.dispatch_attempt_id
        || reserved_attempt != command.reserved_attempt_id
        || sqlite3_column_type(job.st, 22) == SQLITE_NULL
        || static_cast<std::uint64_t>(
            sqlite3_column_int64(job.st, 22))
            != command.dispatch_item_ordinal
        || durable_token != command.claim_token
        || (dispatch_state != "ACTIVE"
            && dispatch_state != "DRAINING")) {
        Rollback(db_);
        receipt.disposition =
            durable_token != command.claim_token
            ? ExecutionDbOperationDisposition::TokenMismatch
            : reserved_attempt != command.reserved_attempt_id
                ? ExecutionDbOperationDisposition::AttemptMismatch
                : ExecutionDbOperationDisposition::WrongState;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }

    Statement insert_blob;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO exec_temp_blob("
            "relative_path,sha256,size_bytes,format,cleanup_state,"
            "created_at_utc) "
            "VALUES(?1,?2,?3,?4,'LIVE',?5);",
            -1,
            &insert_blob.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_text(
        insert_blob.st,
        1,
        command.result_blob.relative_path.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_text(
        insert_blob.st,
        2,
        command.result_blob.sha256.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_int64(
        insert_blob.st,
        3,
        static_cast<std::int64_t>(
            command.result_blob.size_bytes));
    sqlite3_bind_text(
        insert_blob.st,
        4,
        command.result_blob.format.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_blob.st, 5, now);
    if (sqlite3_step(insert_blob.st) != SQLITE_DONE) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    receipt.temp_blob_id = sqlite3_last_insert_rowid(db_);

    Statement stage;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job "
            "SET state='EXECUTION_FINISHED',"
            "execution_finished_at_utc=?1,"
            "worker_terminal_status=?2,"
            "worker_terminal_fingerprint=?3,"
            "worker_terminal_id=?4,"
            "worker_terminal_error_code=?5,"
            "worker_terminal_error_text=?6,"
            "worker_terminal_unstarted=?7,"
            "worker_result_blob_id=?8,"
            "result_processing_state='PENDING',"
            "result_processing_attempts=0,"
            "result_processing_failures=0,"
            "result_processing_error_code=NULL,"
            "result_processing_error_text=NULL,"
            "result_processing_failed_at_utc=NULL,"
            "result_processed_at_utc=NULL,"
            "claimed_by_token=NULL,lease_expires_at_utc=NULL "
            "WHERE job_id=?9 AND dispatch_attempt_id=?10 "
            "AND reserved_attempt_id=?11 "
            "AND state=?12;",
            -1,
            &stage.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(stage.st, 1, now);
    sqlite3_bind_text(
        stage.st,
        2,
        command.terminal_status.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_text(
        stage.st,
        3,
        command.terminal_fingerprint.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_text(
        stage.st,
        4,
        command.terminal_id.c_str(),
        -1,
        SQLITE_TRANSIENT);
    BindOptionalText(stage.st, 5, command.error_code);
    BindOptionalText(stage.st, 6, command.error_text);
    sqlite3_bind_int(stage.st, 7, command.unstarted ? 1 : 0);
    sqlite3_bind_int64(stage.st, 8, receipt.temp_blob_id);
    sqlite3_bind_int64(stage.st, 9, command.job_id);
    sqlite3_bind_int64(
        stage.st,
        10,
        command.dispatch_attempt_id);
    sqlite3_bind_int64(
        stage.st,
        11,
        static_cast<std::int64_t>(
            command.reserved_attempt_id));
    sqlite3_bind_text(
        stage.st,
        12,
        valid_started ? "RUNNING" : "CLAIMED",
        -1,
        SQLITE_STATIC);
    if (sqlite3_step(stage.st) != SQLITE_DONE
        || sqlite3_changes(db_) != 1) {
        fail("worker-terminal staging CAS failed");
        return false;
    }

    Statement resolve_cancellation;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job_cancellation_request "
            "SET state='RESOLVED',resolved_at_utc=?1,"
            "resolution_code='WORKER_TERMINAL' "
            "WHERE job_id=?2 "
            "AND state IN "
            "('REQUESTED','DELIVERED');",
            -1,
            &resolve_cancellation.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(resolve_cancellation.st, 1, now);
    sqlite3_bind_int64(resolve_cancellation.st, 2, command.job_id);
    if (sqlite3_step(resolve_cancellation.st) != SQLITE_DONE) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    const bool cancellation_resolved = sqlite3_changes(db_) > 0;
    if (cancellation_resolved) {
        Statement summary;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE exec_job SET cancellation_state='RESOLVED',"
                "cancellation_resolved_at_utc=?1,"
                "cancellation_resolution_code='WORKER_TERMINAL' "
                "WHERE job_id=?2;",
                -1,
                &summary.st,
                nullptr)
            != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int64(summary.st, 1, now);
        sqlite3_bind_int64(summary.st, 2, command.job_id);
        if (sqlite3_step(summary.st) != SQLITE_DONE) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
    }

    Statement remaining;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT COUNT(1) FROM exec_job "
            "WHERE dispatch_attempt_id=?1 "
            "AND state IN ('CLAIMED','RUNNING');",
            -1,
            &remaining.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(
        remaining.st,
        1,
        command.dispatch_attempt_id);
    if (sqlite3_step(remaining.st) != SQLITE_ROW) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    if (sqlite3_column_int(remaining.st, 0) == 0) {
        Statement close;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE exec_workset_dispatch_attempt "
                "SET state='CLOSED',closed_at_utc=?1,"
                "close_reason_code='WORKER_TERMINALS_STAGED',"
                "lease_expires_at_utc=NULL "
                "WHERE dispatch_attempt_id=?2 "
                "AND state='DRAINING';",
                -1,
                &close.st,
                nullptr)
            != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int64(close.st, 1, now);
        sqlite3_bind_int64(
            close.st,
            2,
            command.dispatch_attempt_id);
        if (sqlite3_step(close.st) != SQLITE_DONE) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        receipt.dispatch_closed = sqlite3_changes(db_) == 1;
    }
    if (!InsertJobActionEventAndOutbox(
            db_,
            command.job_id,
            job_set_id,
            "Execution.JobExecutionFinished.v1",
            "worker-terminal-staged",
            error_out)) {
        fail(error_out != nullptr
            ? *error_out
            : "execution-finished event failed");
        return false;
    }
    if (cancellation_resolved
        && !InsertJobActionEventAndOutbox(
            db_,
            command.job_id,
            job_set_id,
            "Execution.JobCancellationResolved.v1",
            "worker-terminal",
            error_out)) {
        fail(error_out != nullptr
            ? *error_out
            : "cancellation resolution event failed");
        return false;
    }
    if (receipt.dispatch_closed
        && !InsertAggregateOutboxEvent(
            db_,
            "Execution.WorksetReleased.v1",
            "workset",
            workset_id,
            "workset",
            workset_id,
            "dispatch-complete-"
                + std::to_string(command.dispatch_attempt_id),
            error_out)) {
        fail(error_out != nullptr
            ? *error_out
            : "workset-close event failed");
        return false;
    }
    if (!RefreshExecutionWorkAvailability(db_, error_out)
        || !Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    receipt.disposition = ExecutionDbOperationDisposition::Applied;
    receipt.durable_job_state = "EXECUTION_FINISHED";
    if (receipt_out != nullptr) *receipt_out = receipt;
    return true;
}

std::vector<ClaimedExecutionFinishedJob>
SqliteExecutionDb::ClaimExecutionFinishedJobsBatch(
    const ClaimExecutionFinishedJobsBatchCommand& command,
    std::string* error_out) {
    std::vector<ClaimedExecutionFinishedJob> claimed;
    if (db_ == nullptr || command.requested_job_count == 0
        || command.requested_job_count > 1024) {
        if (error_out != nullptr) *error_out = "invalid execution-finished batch claim";
        return claimed;
    }
    BatchTransactionScope batch(db_);
    if (!batch.Begin(error_out)) return claimed;
    claimed.reserve(command.requested_job_count);
    for (std::size_t index = 0; index < command.requested_job_count; ++index) {
        std::string item_error;
        auto item = ClaimNextExecutionFinishedJob(&item_error);
        if (!item.has_value()) {
            if (!item_error.empty()) {
                if (error_out != nullptr) *error_out = std::move(item_error);
                return {};
            }
            break;
        }
        claimed.push_back(std::move(*item));
    }
    if (!RefreshExecutionWorkAvailability(db_, error_out)) return {};
    if (!batch.CommitAll(error_out)) return {};
    return claimed;
}


std::optional<ClaimedExecutionFinishedJob>
SqliteExecutionDb::ClaimNextExecutionFinishedJob(
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out != nullptr) {
            *error_out = "invalid execution-finished claim command";
        }
        return std::nullopt;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return std::nullopt;
    }
    const auto fail = [&](std::string message) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = std::move(message);
    };
    Statement candidate;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT j.job_id "
            "FROM exec_job j "
            "JOIN exec_temp_blob b "
            "  ON b.temp_blob_id=j.worker_result_blob_id "
            "WHERE j.state='EXECUTION_FINISHED' "
            "AND j.workset_id IS NOT NULL "
            "AND j.dispatch_attempt_id IS NOT NULL "
            "AND j.reserved_attempt_id IS NOT NULL "
            "AND b.cleanup_state='LIVE' "
            "AND j.result_processing_state='PENDING' "
            "ORDER BY j.execution_finished_at_utc ASC,j.job_id ASC "
            "LIMIT 1;",
            -1,
            &candidate.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return std::nullopt;
    }
    const auto candidate_rc = sqlite3_step(candidate.st);
    if (candidate_rc == SQLITE_DONE) {
        if (!Commit(db_, error_out)) {
            Rollback(db_);
        }
        return std::nullopt;
    }
    if (candidate_rc != SQLITE_ROW) {
        fail(sqlite3_errmsg(db_));
        return std::nullopt;
    }
    const auto job_id = sqlite3_column_int64(candidate.st, 0);

    Statement claim;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job SET "
            "result_processing_state='PROCESSING',"
            "result_processing_attempts=result_processing_attempts+1 "
            "WHERE job_id=?1 AND state='EXECUTION_FINISHED' "
            "AND result_processing_state='PENDING';",
            -1,
            &claim.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return std::nullopt;
    }
    sqlite3_bind_int64(claim.st, 1, job_id);
    if (sqlite3_step(claim.st) != SQLITE_DONE
        || sqlite3_changes(db_) != 1) {
        fail("execution-finished result claim CAS failed");
        return std::nullopt;
    }

    Statement details;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT w.workset_id,j.dispatch_attempt_id,"
            "j.reserved_attempt_id,w.workset_key,"
            "w.compatibility_key,w.module_canonical_id,"
            "w.module_version,w.module_sha256,w.entrypoint,"
            "w.verified_dependency_sha256,w.runtime_profile_sha256,"
            "w.required_capability_mask,w.execution_affinity_key,"
            "w.baseline_affinity_key,w.estimated_payload_bytes,"
            "j.worker_terminal_status,j.worker_terminal_fingerprint,"
            "j.worker_terminal_id,j.worker_terminal_error_code,"
            "j.worker_terminal_error_text,j.worker_terminal_unstarted,"
            "b.temp_blob_id,b.relative_path,b.sha256,b.size_bytes,"
            "b.format,b.cleanup_state,b.created_at_utc,"
            "j.result_processing_attempts,j.result_processing_failures,"
            "j.dispatch_item_ordinal "
            "FROM exec_job j "
            "JOIN exec_workset w ON w.workset_id=j.workset_id "
            "JOIN exec_temp_blob b "
            "  ON b.temp_blob_id=j.worker_result_blob_id "
            "WHERE j.job_id=?1;",
            -1,
            &details.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return std::nullopt;
    }
    sqlite3_bind_int64(details.st, 1, job_id);
    if (sqlite3_step(details.st) != SQLITE_ROW) {
        fail("claimed execution-finished details are missing");
        return std::nullopt;
    }

    ClaimedExecutionFinishedJob claimed{};
    const auto job = GetJob(job_id);
    if (!job.has_value()) {
        fail("claimed execution-finished job is missing");
        return std::nullopt;
    }
    claimed.job = *job;
    claimed.workset_id = sqlite3_column_int64(details.st, 0);
    claimed.dispatch_attempt_id = sqlite3_column_int64(details.st, 1);
    claimed.reserved_attempt_id = static_cast<std::uint64_t>(
        sqlite3_column_int64(details.st, 2));
    claimed.workset_key = Text(details.st, 3);
    claimed.compatibility.compatibility_key = Text(details.st, 4);
    claimed.compatibility.module_canonical_id = Text(details.st, 5);
    claimed.compatibility.module_version =
        sqlite3_column_int(details.st, 6);
    claimed.compatibility.module_sha256 = Text(details.st, 7);
    claimed.compatibility.entrypoint = Text(details.st, 8);
    claimed.compatibility.verified_dependency_sha256 =
        Text(details.st, 9);
    claimed.compatibility.runtime_profile_sha256 = Text(details.st, 10);
    claimed.compatibility.required_capability_mask =
        static_cast<std::uint64_t>(
            sqlite3_column_int64(details.st, 11));
    claimed.compatibility.execution_affinity_key =
        OptionalText(details.st, 12);
    claimed.compatibility.baseline_affinity_key =
        OptionalText(details.st, 13);
    claimed.compatibility.estimated_payload_bytes =
        static_cast<std::uint64_t>(
            sqlite3_column_int64(details.st, 14));
    claimed.worker_terminal_status = Text(details.st, 15);
    claimed.worker_terminal_fingerprint = Text(details.st, 16);
    claimed.worker_terminal_id = Text(details.st, 17);
    claimed.worker_terminal_error_code = OptionalText(details.st, 18);
    claimed.worker_terminal_error_text = OptionalText(details.st, 19);
    claimed.worker_terminal_unstarted =
        sqlite3_column_int(details.st, 20) != 0;
    claimed.result_blob.temp_blob_id =
        sqlite3_column_int64(details.st, 21);
    claimed.result_blob.relative_path = Text(details.st, 22);
    claimed.result_blob.sha256 = Text(details.st, 23);
    claimed.result_blob.size_bytes = static_cast<std::uint64_t>(
        sqlite3_column_int64(details.st, 24));
    claimed.result_blob.format = Text(details.st, 25);
    claimed.result_blob.cleanup_state = Text(details.st, 26);
    claimed.result_blob.created_at_utc =
        sqlite3_column_int64(details.st, 27);
    claimed.processing_attempts = sqlite3_column_int(details.st, 28);
    claimed.processing_failures = sqlite3_column_int(details.st, 29);
    if (sqlite3_column_type(details.st, 30) == SQLITE_NULL
        || sqlite3_column_int64(details.st, 30) < 0
        || sqlite3_column_int64(details.st, 30)
            > static_cast<sqlite3_int64>(
                (std::numeric_limits<std::uint32_t>::max)())) {
        fail("execution-finished job has invalid runtime ordinal");
        return std::nullopt;
    }
    claimed.runtime_item_ordinal =
        static_cast<std::uint32_t>(
            sqlite3_column_int64(details.st, 30));

    if (!InsertJobActionEventAndOutbox(
            db_,
            job_id,
            claimed.job.job_set_id,
            "Execution.JobResultProcessingStarted.v1",
            "result-processing-claimed",
            error_out)
        || !Commit(db_, error_out)) {
        Rollback(db_);
        return std::nullopt;
    }
    return claimed;
}


std::vector<InterruptedResultProcessingJob>
SqliteExecutionDb::ListInterruptedResultProcessingJobs(
    std::string* error_out) {
    std::vector<InterruptedResultProcessingJob> result;
    if (db_ == nullptr) {
        if (error_out != nullptr) {
            *error_out = "invalid interrupted-result query";
        }
        return result;
    }
    Statement ids;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT job_id FROM exec_job "
            "WHERE state='EXECUTION_FINISHED' "
            "AND result_processing_state='PROCESSING' "
            "ORDER BY execution_finished_at_utc ASC,job_id ASC;",
            -1,
            &ids.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return result;
    }
    std::vector<std::int64_t> job_ids;
    while (sqlite3_step(ids.st) == SQLITE_ROW) {
        job_ids.push_back(sqlite3_column_int64(ids.st, 0));
    }
    result.reserve(job_ids.size());
    for (const auto job_id : job_ids) {
        const auto job = GetJob(job_id);
        if (!job.has_value()) {
            if (error_out != nullptr) {
                *error_out = "interrupted result job disappeared";
            }
            return {};
        }
        Statement details;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT w.workset_id,j.dispatch_attempt_id,"
                "j.reserved_attempt_id,w.workset_key,"
                "w.compatibility_key,w.module_canonical_id,"
                "w.module_version,w.module_sha256,w.entrypoint,"
                "w.verified_dependency_sha256,w.runtime_profile_sha256,"
                "w.required_capability_mask,w.execution_affinity_key,"
                "w.baseline_affinity_key,w.estimated_payload_bytes,"
                "j.worker_terminal_status,j.worker_terminal_fingerprint,"
                "j.worker_terminal_id,j.worker_terminal_error_code,"
                "j.worker_terminal_error_text,j.worker_terminal_unstarted,"
                "b.temp_blob_id,b.relative_path,b.sha256,b.size_bytes,"
                "b.format,b.cleanup_state,b.created_at_utc,"
                "j.result_processing_attempts,j.result_processing_failures,"
                "j.dispatch_item_ordinal "
                "FROM exec_job j "
                "LEFT JOIN exec_workset w ON w.workset_id=j.workset_id "
                "LEFT JOIN exec_temp_blob b "
                " ON b.temp_blob_id=j.worker_result_blob_id "
                "WHERE j.job_id=?1;",
                -1,
                &details.st,
                nullptr)
            != SQLITE_OK) {
            if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
            return {};
        }
        sqlite3_bind_int64(details.st, 1, job_id);
        if (sqlite3_step(details.st) != SQLITE_ROW) {
            if (error_out != nullptr) {
                *error_out = "interrupted result structural metadata is missing";
            }
            return {};
        }
        InterruptedResultProcessingJob interrupted{};
        auto& claimed = interrupted.claimed;
        claimed.job = *job;
        interrupted.structural_metadata_complete =
            sqlite3_column_type(details.st, 0) != SQLITE_NULL
            && sqlite3_column_type(details.st, 1) != SQLITE_NULL
            && sqlite3_column_type(details.st, 2) != SQLITE_NULL
            && sqlite3_column_type(details.st, 3) != SQLITE_NULL
            && sqlite3_column_type(details.st, 4) != SQLITE_NULL
            && sqlite3_column_type(details.st, 5) != SQLITE_NULL
            && sqlite3_column_type(details.st, 6) != SQLITE_NULL
            && sqlite3_column_type(details.st, 7) != SQLITE_NULL
            && sqlite3_column_type(details.st, 8) != SQLITE_NULL
            && sqlite3_column_type(details.st, 9) != SQLITE_NULL
            && sqlite3_column_type(details.st, 10) != SQLITE_NULL
            && sqlite3_column_type(details.st, 11) != SQLITE_NULL
            && sqlite3_column_type(details.st, 14) != SQLITE_NULL
            && sqlite3_column_type(details.st, 15) != SQLITE_NULL
            && sqlite3_column_type(details.st, 16) != SQLITE_NULL
            && sqlite3_column_type(details.st, 17) != SQLITE_NULL
            && sqlite3_column_type(details.st, 20) != SQLITE_NULL
            && sqlite3_column_type(details.st, 28) != SQLITE_NULL
            && sqlite3_column_type(details.st, 29) != SQLITE_NULL
            && sqlite3_column_type(details.st, 30) != SQLITE_NULL;
        if (!interrupted.structural_metadata_complete) {
            interrupted.structural_diagnostic =
                "PROCESSING job is missing dispatch, workset, terminal, or ordinal metadata";
        }
        claimed.workset_id = sqlite3_column_int64(details.st, 0);
        claimed.dispatch_attempt_id = sqlite3_column_int64(details.st, 1);
        claimed.reserved_attempt_id = static_cast<std::uint64_t>(
            sqlite3_column_int64(details.st, 2));
        claimed.workset_key = Text(details.st, 3);
        claimed.compatibility.compatibility_key = Text(details.st, 4);
        claimed.compatibility.module_canonical_id = Text(details.st, 5);
        claimed.compatibility.module_version = sqlite3_column_int(details.st, 6);
        claimed.compatibility.module_sha256 = Text(details.st, 7);
        claimed.compatibility.entrypoint = Text(details.st, 8);
        claimed.compatibility.verified_dependency_sha256 = Text(details.st, 9);
        claimed.compatibility.runtime_profile_sha256 = Text(details.st, 10);
        claimed.compatibility.required_capability_mask =
            static_cast<std::uint64_t>(sqlite3_column_int64(details.st, 11));
        claimed.compatibility.execution_affinity_key = OptionalText(details.st, 12);
        claimed.compatibility.baseline_affinity_key = OptionalText(details.st, 13);
        claimed.compatibility.estimated_payload_bytes =
            static_cast<std::uint64_t>(sqlite3_column_int64(details.st, 14));
        claimed.worker_terminal_status = Text(details.st, 15);
        claimed.worker_terminal_fingerprint = Text(details.st, 16);
        claimed.worker_terminal_id = Text(details.st, 17);
        claimed.worker_terminal_error_code = OptionalText(details.st, 18);
        claimed.worker_terminal_error_text = OptionalText(details.st, 19);
        claimed.worker_terminal_unstarted = sqlite3_column_int(details.st, 20) != 0;
        interrupted.has_result_blob_record =
            sqlite3_column_type(details.st, 21) != SQLITE_NULL;
        if (interrupted.has_result_blob_record) {
            claimed.result_blob.temp_blob_id = sqlite3_column_int64(details.st, 21);
            claimed.result_blob.relative_path = Text(details.st, 22);
            claimed.result_blob.sha256 = Text(details.st, 23);
            claimed.result_blob.size_bytes = static_cast<std::uint64_t>(
                sqlite3_column_int64(details.st, 24));
            claimed.result_blob.format = Text(details.st, 25);
            claimed.result_blob.cleanup_state = Text(details.st, 26);
            claimed.result_blob.created_at_utc = sqlite3_column_int64(details.st, 27);
        }
        claimed.processing_attempts = sqlite3_column_int(details.st, 28);
        claimed.processing_failures = sqlite3_column_int(details.st, 29);
        if (sqlite3_column_type(details.st, 30) != SQLITE_NULL) {
            claimed.runtime_item_ordinal = static_cast<std::uint32_t>(
                sqlite3_column_int64(details.st, 30));
        }
        result.push_back(std::move(interrupted));
    }
    if (error_out != nullptr) error_out->clear();
    return result;
}

bool SqliteExecutionDb::ResetInterruptedResultProcessing(
    const ResetInterruptedResultProcessingCommand& command,
    ResultProcessingReceipt* receipt_out,
    std::string* error_out) {
    ResultProcessingReceipt receipt{};
    receipt.job_id = command.job_id;
    if (db_ == nullptr || command.job_id <= 0 || command.requested_by.empty()) {
        receipt.disposition = ExecutionDbOperationDisposition::InvalidRequest;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) return false;
    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job SET result_processing_state='PENDING',"
            "result_processing_error_code=NULL,result_processing_error_text=NULL,"
            "result_processing_failed_at_utc=NULL "
            "WHERE job_id=?1 AND state='EXECUTION_FINISHED' "
            "AND result_processing_state='PROCESSING' "
            "AND worker_result_blob_id IS NOT NULL;",
            -1, &update.st, nullptr) != SQLITE_OK) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(update.st, 1, command.job_id);
    if (sqlite3_step(update.st) != SQLITE_DONE) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        Rollback(db_);
        receipt.disposition = ExecutionDbOperationDisposition::WrongState;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    if (!RefreshExecutionWorkAvailability(db_, error_out)
        || !Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    receipt.disposition = ExecutionDbOperationDisposition::Applied;
    receipt.durable_job_state = "EXECUTION_FINISHED";
    receipt.processing_state = "PENDING";
    if (receipt_out != nullptr) *receipt_out = receipt;
    return true;
}

bool SqliteExecutionDb::RequeueLostResultProcessing(
    const RequeueLostResultProcessingCommand& command,
    ResultProcessingReceipt* receipt_out,
    std::string* error_out) {
    ResultProcessingReceipt receipt{};
    receipt.job_id = command.job_id;
    if (db_ == nullptr || command.job_id <= 0 || command.requested_by.empty()) {
        receipt.disposition = ExecutionDbOperationDisposition::InvalidRequest;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) return false;
    const auto now = CurrentUtcMs(db_);
    Statement state;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT job_set_id,worker_result_blob_id FROM exec_job "
            "WHERE job_id=?1 AND state='EXECUTION_FINISHED' "
            "AND result_processing_state='PROCESSING';",
            -1, &state.st, nullptr) != SQLITE_OK) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(state.st, 1, command.job_id);
    if (sqlite3_step(state.st) != SQLITE_ROW) {
        Rollback(db_);
        receipt.disposition = ExecutionDbOperationDisposition::WrongState;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    const auto job_set_id = sqlite3_column_int64(state.st, 0);
    const auto blob_id = sqlite3_column_type(state.st, 1) == SQLITE_NULL
        ? std::optional<std::int64_t>{}
        : std::optional<std::int64_t>{sqlite3_column_int64(state.st, 1)};
    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job SET state='QUEUED',queued_at_utc=?1,"
            "max_attempts=max_attempts+1,ended_at_utc=NULL,"
            "claimed_by_token=NULL,lease_expires_at_utc=NULL,"
            "dispatch_attempt_id=NULL,dispatch_item_ordinal=NULL,"
            "reserved_attempt_id=NULL,execution_finished_at_utc=NULL,"
            "worker_terminal_status=NULL,worker_terminal_fingerprint=NULL,"
            "worker_terminal_id=NULL,worker_terminal_error_code=NULL,"
            "worker_terminal_error_text=NULL,worker_terminal_unstarted=NULL,"
            "worker_result_blob_id=NULL,result_processing_state='PROCESSED',"
            "result_processed_at_utc=?1 "
            "WHERE job_id=?2 AND state='EXECUTION_FINISHED' "
            "AND result_processing_state='PROCESSING';",
            -1, &update.st, nullptr) != SQLITE_OK) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(update.st, 1, now);
    sqlite3_bind_int64(update.st, 2, command.job_id);
    if (sqlite3_step(update.st) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = "lost-result requeue CAS failed";
        return false;
    }
    if (blob_id.has_value()) {
        Statement cleanup;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE exec_temp_blob SET cleanup_state='DELETE_PENDING',"
                "delete_pending_at_utc=COALESCE(delete_pending_at_utc,?1),"
                "cleanup_claim_token=NULL,cleanup_lease_expires_at_utc=NULL "
                "WHERE temp_blob_id=?2 AND cleanup_state='LIVE';",
                -1, &cleanup.st, nullptr) != SQLITE_OK) {
            Rollback(db_);
            if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(cleanup.st, 1, now);
        sqlite3_bind_int64(cleanup.st, 2, *blob_id);
        if (sqlite3_step(cleanup.st) != SQLITE_DONE) {
            Rollback(db_);
            if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
            return false;
        }
    }
    if (!InsertJobActionEventAndOutbox(
            db_, command.job_id, job_set_id,
            "Execution.JobLostResultRecovered.v1",
            "lost-result-blob-requeued-with-extra-attempt", error_out)
        || !RefreshExecutionWorkAvailability(db_, error_out)
        || !Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    receipt.disposition = ExecutionDbOperationDisposition::Applied;
    receipt.durable_job_state = "QUEUED";
    receipt.processing_state = "PROCESSED";
    if (receipt_out != nullptr) *receipt_out = receipt;
    return true;
}

bool SqliteExecutionDb::RecordResultProcessingFailure(
    const RecordResultProcessingFailureCommand& command,
    ResultProcessingReceipt* receipt_out,
    std::string* error_out) {
    ResultProcessingReceipt receipt{};
    receipt.job_id = command.job_id;
    if (db_ == nullptr || command.job_id <= 0 || command.error_code.empty()) {
        receipt.disposition = ExecutionDbOperationDisposition::InvalidRequest;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) return false;
    const auto now = CurrentUtcMs(db_);
    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job SET "
            "result_processing_failures=result_processing_failures+1,"
            "result_processing_error_code=?1,result_processing_error_text=?2,"
            "result_processing_failed_at_utc=?3 "
            "WHERE job_id=?4 AND state='EXECUTION_FINISHED' "
            "AND result_processing_state='PROCESSING';",
            -1, &update.st, nullptr) != SQLITE_OK) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(update.st, 1, command.error_code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.st, 2, command.error_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update.st, 3, now);
    sqlite3_bind_int64(update.st, 4, command.job_id);
    if (sqlite3_step(update.st) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        Rollback(db_);
        receipt.disposition = ExecutionDbOperationDisposition::WrongState;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    receipt.disposition = ExecutionDbOperationDisposition::Applied;
    receipt.durable_job_state = "EXECUTION_FINISHED";
    receipt.processing_state = "PROCESSING";
    if (receipt_out != nullptr) *receipt_out = receipt;
    return true;
}

bool SqliteExecutionDb::CommitResultFinalizationsBatch(
    const CommitResultFinalizationsBatchCommand& command,
    std::vector<ResultProcessingReceipt>* receipts_out,
    std::string* error_out) {
    if (receipts_out != nullptr) receipts_out->clear();
    if (db_ == nullptr || command.finalizations.empty()) {
        if (error_out != nullptr) *error_out = "invalid result-finalization batch";
        return false;
    }
    BatchTransactionScope batch(db_);
    if (!batch.Begin(error_out)) return false;
    std::vector<ResultProcessingReceipt> receipts;
    receipts.reserve(command.finalizations.size());
    for (const auto& finalization : command.finalizations) {
        ResultProcessingReceipt receipt{};
        if (!CommitResultFinalization(finalization, &receipt, error_out)) {
            return false;
        }
        receipts.push_back(std::move(receipt));
    }
    if (!RefreshExecutionWorkAvailability(db_, error_out)) return false;
    if (!batch.CommitAll(error_out)) return false;
    if (receipts_out != nullptr) *receipts_out = std::move(receipts);
    return true;
}

bool SqliteExecutionDb::CommitResultFinalization(
    const CommitResultFinalizationCommand& command,
    ResultProcessingReceipt* receipt_out,
    std::string* error_out) {
    ResultProcessingReceipt receipt{};
    receipt.job_id = command.job_id;
    if (receipt_out != nullptr) *receipt_out = receipt;

    const bool final =
        command.disposition == ExecutionResultFinalizationDisposition::Final;
    const bool valid_final_state = command.final_state.has_value()
        && (*command.final_state == "SUCCEEDED"
            || *command.final_state == "SUCCEEDED_WINNER"
            || *command.final_state == "SUCCEEDED_DUPLICATE"
            || *command.final_state == "FAILED"
            || *command.final_state == "CANCELED"
            || *command.final_state == "SUPERSEDED");
    std::unordered_set<std::string> output_keys;
    bool valid_outputs = true;
    for (const auto& output : command.outputs) {
        valid_outputs = valid_outputs
            && !output.output_key.empty()
            && !output.data_kind.empty()
            && !output.ref_kind.empty()
            && output.ref_id > 0
            && output_keys.insert(output.output_key).second;
    }
    std::unordered_set<std::string> cancellation_keys;
    bool valid_cancellations = true;
    for (const auto& cancellation : command.cancellation_requests) {
        const auto identity = std::to_string(cancellation.job_id)
            + "\n" + cancellation.request_key;
        valid_cancellations = valid_cancellations
            && cancellation.job_id > 0
            && !cancellation.request_key.empty()
            && !cancellation.reason_code.empty()
            && !cancellation.requested_by.empty()
            && (!cancellation.caused_by_job_id.has_value()
                || *cancellation.caused_by_job_id > 0)
            && cancellation_keys.insert(identity).second;
    }
    if (db_ == nullptr
        || command.job_id <= 0
        || command.requested_by.empty()
        || (final && !valid_final_state)
        || (!final && command.final_state.has_value())
        || (!final
            && (!command.outputs.empty()
                || !command.cancellation_requests.empty()))
        || !valid_outputs
        || !valid_cancellations) {
        receipt.disposition =
            ExecutionDbOperationDisposition::InvalidRequest;
        if (receipt_out != nullptr) *receipt_out = receipt;
        if (error_out != nullptr) {
            *error_out = "invalid result-finalization command";
        }
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }
    const auto fail = [&](std::string message) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = std::move(message);
    };
    const auto now = CurrentUtcMs(db_);

    Statement state;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT state,result_processing_state,"
            "job_set_id,attempts,max_attempts,worker_result_blob_id,"
            "result_processing_failures "
            "FROM exec_job WHERE job_id=?1;",
            -1,
            &state.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(state.st, 1, command.job_id);
    if (sqlite3_step(state.st) != SQLITE_ROW) {
        Rollback(db_);
        receipt.disposition =
            ExecutionDbOperationDisposition::Missing;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    const auto durable_state = Text(state.st, 0);
    const auto processing_state = Text(state.st, 1);
    const auto job_set_id = sqlite3_column_int64(state.st, 2);
    const auto attempts = sqlite3_column_int(state.st, 3);
    const auto max_attempts = sqlite3_column_int(state.st, 4);
    const auto blob_id =
        sqlite3_column_type(state.st, 5) == SQLITE_NULL
        ? std::optional<std::int64_t>{}
        : std::optional<std::int64_t>{
            sqlite3_column_int64(state.st, 5)};
    receipt.processing_failures = sqlite3_column_int(state.st, 6);

    const bool previously_finalized = processing_state == "PROCESSED"
        && ((final && durable_state == *command.final_state)
            || (!final
                && (durable_state == "QUEUED"
                    || durable_state == "FAILED")));
    if (previously_finalized) {
        for (const auto& cancellation : command.cancellation_requests) {
            CancellationInsertResult cancellation_result{};
            if (!InsertCancellationRequest(
                    db_, cancellation, now,
                    &cancellation_result, error_out)) {
                fail(error_out != nullptr ? *error_out
                    : "cancellation receipt recovery failed");
                return false;
            }
            if (cancellation_result.disposition
                    != ExecutionDbOperationDisposition::AlreadyApplied) {
                Rollback(db_);
                receipt.disposition =
                    cancellation_result.disposition;
                if (receipt_out != nullptr) *receipt_out = receipt;
                return true;
            }
            auto committed = MakeCommittedCancellation(
                cancellation, cancellation_result);
            if (!PopulateCommittedCancellationExecutionFacts(
                    db_, &committed, error_out)) {
                fail(error_out != nullptr ? *error_out
                    : "cancellation execution facts are missing");
                return false;
            }
            receipt.committed_cancellations.push_back(
                std::move(committed));
        }
        receipt.disposition =
            ExecutionDbOperationDisposition::AlreadyApplied;
        receipt.durable_job_state = durable_state;
        receipt.processing_state = processing_state;
        if (final || durable_state == "FAILED") {
            ClaimedExecutionJob workflow_identity{};
            if (!ResolveWorkflowStepForJobSetAncestry(
                    db_,
                    job_set_id,
                    &workflow_identity,
                    error_out)) {
                fail(error_out != nullptr
                    ? *error_out
                    : "workflow step resolution failed");
                return false;
            }
            receipt.workflow_step_id =
                workflow_identity.workflow_step_id;
            Statement sequence;
            if (sqlite3_prepare_v2(
                    db_,
                    "SELECT COALESCE(MAX(job_event_id),0) "
                    "FROM exec_job_event "
                    "WHERE job_id=?1 "
                    "AND event_kind='Execution.JobCompleted.v1';",
                    -1,
                    &sequence.st,
                    nullptr)
                != SQLITE_OK) {
                fail(sqlite3_errmsg(db_));
                return false;
            }
            sqlite3_bind_int64(sequence.st, 1, command.job_id);
            if (sqlite3_step(sequence.st) != SQLITE_ROW) {
                fail(sqlite3_errmsg(db_));
                return false;
            }
            receipt.commit_sequence =
                sqlite3_column_int64(sequence.st, 0);
            if (receipt.commit_sequence <= 0
                || receipt.workflow_step_id <= 0) {
                fail(
                    "durable finalization receipt identity is missing");
                return false;
            }
        }
        if (!Commit(db_, error_out)) {
            Rollback(db_);
            return false;
        }
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    if (durable_state != "EXECUTION_FINISHED"
        || processing_state != "PROCESSING") {
        Rollback(db_);
        receipt.disposition =
            ExecutionDbOperationDisposition::WrongState;
        receipt.durable_job_state = durable_state;
        receipt.processing_state = processing_state;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    if (!blob_id.has_value()) {
        fail("execution-finished job has no worker result blob");
        return false;
    }

    ClaimedExecutionJob workflow_identity{};
    const bool becomes_terminal =
        final || attempts >= max_attempts;
    if (becomes_terminal
        && !ResolveWorkflowStepForJobSetAncestry(
            db_,
            job_set_id,
            &workflow_identity,
            error_out)) {
        fail(error_out != nullptr
            ? *error_out
            : "workflow step resolution failed");
        return false;
    }

    Statement insert_output;
    if (final && !command.outputs.empty()
        && sqlite3_prepare_v2(
            db_,
            "INSERT INTO exec_job_output("
            "job_id,output_key,data_kind,ref_kind,ref_id,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6);",
            -1,
            &insert_output.st,
            nullptr)
            != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    for (const auto& output : command.outputs) {
        sqlite3_reset(insert_output.st);
        sqlite3_clear_bindings(insert_output.st);
        sqlite3_bind_int64(insert_output.st, 1, command.job_id);
        sqlite3_bind_text(
            insert_output.st,
            2,
            output.output_key.c_str(),
            -1,
            SQLITE_TRANSIENT);
        sqlite3_bind_text(
            insert_output.st,
            3,
            output.data_kind.c_str(),
            -1,
            SQLITE_TRANSIENT);
        sqlite3_bind_text(
            insert_output.st,
            4,
            output.ref_kind.c_str(),
            -1,
            SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert_output.st, 5, output.ref_id);
        sqlite3_bind_int64(insert_output.st, 6, now);
        if (sqlite3_step(insert_output.st) != SQLITE_DONE) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
    }

    for (const auto& cancellation : command.cancellation_requests) {
        CancellationInsertResult cancellation_result{};
        if (!InsertCancellationRequest(
                db_,
                cancellation,
                now,
                &cancellation_result,
                error_out)) {
            fail(error_out != nullptr
                ? *error_out
                : "cancellation request insert failed");
            return false;
        }
        if (cancellation_result.disposition
                != ExecutionDbOperationDisposition::Applied
            && cancellation_result.disposition
                != ExecutionDbOperationDisposition::AlreadyApplied) {
            Rollback(db_);
            receipt.disposition = cancellation_result.disposition;
            if (receipt_out != nullptr) *receipt_out = receipt;
            return true;
        }
        auto committed = MakeCommittedCancellation(
            cancellation, cancellation_result);
        if (!PopulateCommittedCancellationExecutionFacts(
                db_, &committed, error_out)) {
            fail(error_out != nullptr ? *error_out
                : "cancellation execution facts are missing");
            return false;
        }
        receipt.committed_cancellations.push_back(
            std::move(committed));
    }

    Statement event_line;
    if (!command.event_lines.empty()
        && sqlite3_prepare_v2(
            db_,
            "INSERT INTO exec_job_event("
            "job_id,event_kind,event_ts_utc,message,artifact_id) "
            "VALUES(?1,'Execution.ProgramResultEvent.v1',?2,?3,NULL);",
            -1,
            &event_line.st,
            nullptr)
            != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    for (const auto& line : command.event_lines) {
        sqlite3_reset(event_line.st);
        sqlite3_clear_bindings(event_line.st);
        sqlite3_bind_int64(event_line.st, 1, command.job_id);
        sqlite3_bind_int64(event_line.st, 2, now);
        sqlite3_bind_text(
            event_line.st, 3, line.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(event_line.st) != SQLITE_DONE) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
    }

    const bool retry_exhausted = !final && attempts >= max_attempts;
    Statement finalize;
    if (final) {
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE exec_job SET state=?1,ended_at_utc=?2,"
                "error_code=?3,error_text=?4,"
                "result_processing_state='PROCESSED',"
                "result_processed_at_utc=?2 "
                "WHERE job_id=?5 AND state='EXECUTION_FINISHED' "
                "AND result_processing_state='PROCESSING';",
                -1,
                &finalize.st,
                nullptr)
            != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_text(
            finalize.st,
            1,
            command.final_state->c_str(),
            -1,
            SQLITE_TRANSIENT);
        sqlite3_bind_int64(finalize.st, 2, now);
        BindOptionalText(finalize.st, 3, command.error_code);
        BindOptionalText(finalize.st, 4, command.error_text);
        sqlite3_bind_int64(finalize.st, 5, command.job_id);
    } else if (retry_exhausted) {
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE exec_job SET state='FAILED',ended_at_utc=?1,"
                "error_code=COALESCE(?2,'EXECUTION_ATTEMPTS_EXHAUSTED'),"
                "error_text=?3,"
                "result_processing_state='PROCESSED',"
                "result_processed_at_utc=?1 "
                "WHERE job_id=?4 AND state='EXECUTION_FINISHED' "
                "AND result_processing_state='PROCESSING';",
                -1,
                &finalize.st,
                nullptr)
            != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int64(finalize.st, 1, now);
        BindOptionalText(finalize.st, 2, command.error_code);
        BindOptionalText(finalize.st, 3, command.error_text);
        sqlite3_bind_int64(finalize.st, 4, command.job_id);
    } else {
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE exec_job SET state='QUEUED',queued_at_utc=?1,"
                "ended_at_utc=NULL,error_code=NULL,error_text=NULL,"
                "claimed_by_token=NULL,lease_expires_at_utc=NULL,"
                "dispatch_attempt_id=NULL,dispatch_item_ordinal=NULL,"
                "reserved_attempt_id=NULL,"
                "result_processing_state='PROCESSED',"
                "result_processed_at_utc=?1 "
                "WHERE job_id=?2 AND state='EXECUTION_FINISHED' "
                "AND result_processing_state='PROCESSING';",
                -1,
                &finalize.st,
                nullptr)
            != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int64(finalize.st, 1, now);
        sqlite3_bind_int64(finalize.st, 2, command.job_id);
    }
    if (sqlite3_step(finalize.st) != SQLITE_DONE
        || sqlite3_changes(db_) != 1) {
        fail("result-finalization CAS failed");
        return false;
    }

    Statement cleanup;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_temp_blob SET cleanup_state='DELETE_PENDING',"
            "delete_pending_at_utc=COALESCE(delete_pending_at_utc,?1),"
            "cleanup_claim_token=NULL,"
            "cleanup_lease_expires_at_utc=NULL "
            "WHERE temp_blob_id=?2 AND cleanup_state='LIVE';",
            -1,
            &cleanup.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(cleanup.st, 1, now);
    sqlite3_bind_int64(cleanup.st, 2, *blob_id);
    if (sqlite3_step(cleanup.st) != SQLITE_DONE
        || sqlite3_changes(db_) != 1) {
        fail("worker result blob cleanup transition failed");
        return false;
    }

    if (!InsertJobActionEventAndOutbox(
            db_,
            command.job_id,
            job_set_id,
            "Execution.JobResultProcessed.v1",
            final ? "program-result-finalized"
                  : retry_exhausted
                    ? "execution-retry-attempts-exhausted"
                    : "execution-retry-requested",
            error_out)) {
        fail(error_out != nullptr
            ? *error_out
            : "result-processed event failed");
        return false;
    }

    if (becomes_terminal) {
        std::int64_t commit_sequence = 0;
        if (!InsertJobActionEventAndOutbox(
                db_,
                command.job_id,
                job_set_id,
                "Execution.JobCompleted.v1",
                final ? "program-result-completed"
                      : "execution-retry-attempts-exhausted",
                error_out,
                &commit_sequence)) {
            fail(error_out != nullptr
                ? *error_out
                : "job-completed event failed");
            return false;
        }
        receipt.commit_sequence = commit_sequence;
        receipt.workflow_step_id =
            workflow_identity.workflow_step_id;
    } else if (!InsertJobActionEventAndOutbox(
            db_,
            command.job_id,
            job_set_id,
            "Execution.JobClaimRequeued.v1",
            "program-result-retry",
            error_out)) {
        fail(error_out != nullptr
            ? *error_out
            : "job retry event failed");
        return false;
    }

    if (!RefreshExecutionWorkAvailability(db_, error_out)
        || !Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    receipt.disposition = ExecutionDbOperationDisposition::Applied;
    receipt.durable_job_state = final
        ? *command.final_state
        : retry_exhausted ? "FAILED" : "QUEUED";
    receipt.processing_state = "PROCESSED";
    if (receipt_out != nullptr) *receipt_out = receipt;
    return true;
}

std::vector<CommittedJobCancellation>
SqliteExecutionDb::ListUnresolvedJobCancellations(
    std::string* error_out) {
    std::vector<CommittedJobCancellation> result;
    if (error_out != nullptr) error_out->clear();
    if (db_ == nullptr) {
        if (error_out != nullptr) *error_out = "database handle is null";
        return result;
    }
    Statement query;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT c.cancellation_request_id,c.job_id,c.request_key,"
            "c.reason_code,c.reason_text,c.caused_by_job_id,c.state,"
            "j.state,j.workset_id,j.dispatch_attempt_id,d.claim_token "
            "FROM exec_job_cancellation_request c "
            "JOIN exec_job j ON j.job_id=c.job_id "
            "LEFT JOIN exec_workset_dispatch_attempt d "
            " ON d.dispatch_attempt_id=j.dispatch_attempt_id "
            "WHERE c.state IN ('REQUESTED','DELIVERED') "
            "ORDER BY requested_at_utc,cancellation_request_id;",
            -1,
            &query.st,
            nullptr) != SQLITE_OK) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return {};
    }
    for (;;) {
        const auto rc = sqlite3_step(query.st);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
            return {};
        }
        result.push_back({
            .cancellation_request_id = sqlite3_column_int64(query.st, 0),
            .job_id = sqlite3_column_int64(query.st, 1),
            .request_key = Text(query.st, 2),
            .reason_code = Text(query.st, 3),
            .reason_text = OptionalText(query.st, 4),
            .caused_by_job_id = sqlite3_column_type(query.st, 5)
                == SQLITE_NULL
                ? std::optional<std::int64_t>{}
                : std::optional<std::int64_t>{
                    sqlite3_column_int64(query.st, 5)},
            .state = Text(query.st, 6),
            .durable_job_state = Text(query.st, 7),
            .workset_id = sqlite3_column_type(query.st, 8) == SQLITE_NULL
                ? std::optional<std::int64_t>{}
                : std::optional<std::int64_t>{
                    sqlite3_column_int64(query.st, 8)},
            .dispatch_attempt_id = sqlite3_column_type(query.st, 9)
                    == SQLITE_NULL
                ? std::optional<std::int64_t>{}
                : std::optional<std::int64_t>{
                    sqlite3_column_int64(query.st, 9)},
            .claim_token = OptionalText(query.st, 10),
            .disposition = ExecutionDbOperationDisposition::Applied,
        });
    }
    return result;
}

bool SqliteExecutionDb::ApplyJobCancellationOutcomeInTransaction(
    const JobCancellationOutcomeCommand& command,
    JobCancellationReceipt* receipt_out,
    std::string* error_out) {
    JobCancellationReceipt receipt{};
    receipt.cancellation_request_id = command.cancellation_request_id;
    receipt.job_id = command.job_id;
    if (receipt_out != nullptr) *receipt_out = receipt;
    const bool resolves_without_worker =
        command.kind == JobCancellationOutcomeKind::CancelWithoutWorker
        || command.kind
            == JobCancellationOutcomeKind::InitialSidecarApplied;
    const bool resolves = resolves_without_worker
        || command.kind
            == JobCancellationOutcomeKind::WorkerTerminalResolved;
    if (db_ == nullptr || command.cancellation_request_id <= 0
        || command.job_id <= 0 || command.requested_by.empty()
        || (resolves && command.resolution_code.empty())
        || (command.kind == JobCancellationOutcomeKind::DeliveryFailed
            && (!command.error_code.has_value()
                || command.error_code->empty()))) {
        receipt.disposition =
            ExecutionDbOperationDisposition::InvalidRequest;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return false;
    }
    const auto fail = [&](std::string message) {
        if (error_out != nullptr) *error_out = std::move(message);
    };
    Statement state;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT c.state,c.resolution_code,j.state,j.job_set_id,"
            "j.dispatch_attempt_id,d.claim_token,d.state "
            "FROM exec_job_cancellation_request c "
            "JOIN exec_job j ON j.job_id=c.job_id "
            "LEFT JOIN exec_workset_dispatch_attempt d "
            " ON d.dispatch_attempt_id=j.dispatch_attempt_id "
            "WHERE c.cancellation_request_id=?1 AND c.job_id=?2;",
            -1, &state.st, nullptr) != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int64(state.st, 1, command.cancellation_request_id);
    sqlite3_bind_int64(state.st, 2, command.job_id);
    if (sqlite3_step(state.st) != SQLITE_ROW) {
        receipt.disposition = ExecutionDbOperationDisposition::Missing;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    receipt.state = Text(state.st, 0);
    receipt.resolution_code = OptionalText(state.st, 1);
    const auto job_state = Text(state.st, 2);
    const auto job_set_id = sqlite3_column_int64(state.st, 3);
    const auto dispatch_attempt_id =
        sqlite3_column_type(state.st, 4) == SQLITE_NULL
        ? std::optional<std::int64_t>{}
        : std::optional<std::int64_t>{
            sqlite3_column_int64(state.st, 4)};
    const auto dispatch_token = OptionalText(state.st, 5);
    const auto dispatch_state = Text(state.st, 6);
    const bool completed = receipt.state == "RESOLVED"
        || (command.kind
                == JobCancellationOutcomeKind::WorkerDeliveryAccepted
            && receipt.state == "DELIVERED")
        || (command.kind == JobCancellationOutcomeKind::DeliveryFailed
            && (receipt.state == "DELIVERED"
                || receipt.state == "RESOLVED"));
    if (completed) {
        receipt.disposition =
            ExecutionDbOperationDisposition::AlreadyApplied;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    if (receipt.state != "REQUESTED"
        && receipt.state != "DELIVERED") {
        receipt.disposition =
            ExecutionDbOperationDisposition::WrongState;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    if (command.dispatch_attempt_id.has_value()
        && (dispatch_attempt_id != command.dispatch_attempt_id
            || (command.claim_token.has_value()
                && dispatch_token != command.claim_token))) {
        receipt.disposition =
            ExecutionDbOperationDisposition::TokenMismatch;
        if (receipt_out != nullptr) *receipt_out = receipt;
        return true;
    }
    const auto now = CurrentUtcMs(db_);
    if (resolves_without_worker) {
        const bool state_allowed = job_state == "PENDING_WORKSET"
            || job_state == "QUEUED"
            || (job_state == "CLAIMED"
                && command.dispatch_attempt_id.has_value());
        if (!state_allowed) {
            receipt.disposition =
                ExecutionDbOperationDisposition::WrongState;
            if (receipt_out != nullptr) *receipt_out = receipt;
            return true;
        }
        Statement resolve;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE exec_job_cancellation_request SET "
                "state='RESOLVED',resolved_at_utc=?1,resolution_code=?2 "
                "WHERE cancellation_request_id=?3 "
                "AND state IN ('REQUESTED','DELIVERED');",
                -1, &resolve.st, nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int64(resolve.st, 1, now);
        sqlite3_bind_text(resolve.st, 2,
            command.resolution_code.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(resolve.st, 3,
            command.cancellation_request_id);
        if (sqlite3_step(resolve.st) != SQLITE_DONE
            || sqlite3_changes(db_) != 1) {
            fail("cancellation resolution CAS failed");
            return false;
        }
        Statement cancel;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE exec_job SET state='CANCELED',ended_at_utc=?1,"
                "error_code='CANCELED',error_text=?2,"
                "cancellation_state='RESOLVED',"
                "cancellation_resolved_at_utc=?1,"
                "cancellation_resolution_code=?2 "
                "WHERE job_id=?3 AND state=?4;",
                -1, &cancel.st, nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int64(cancel.st, 1, now);
        sqlite3_bind_text(cancel.st, 2,
            command.resolution_code.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(cancel.st, 3, command.job_id);
        sqlite3_bind_text(cancel.st, 4,
            job_state.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(cancel.st) != SQLITE_DONE
            || sqlite3_changes(db_) != 1
            || !InsertJobActionEventAndOutbox(
                db_, command.job_id, job_set_id,
                "Execution.JobCancellationResolved.v1",
                "durable-cancellation-resolved", error_out)
            || !InsertJobActionEventAndOutbox(
                db_, command.job_id, job_set_id,
                "Execution.JobCompleted.v1",
                "durable-cancellation-completed", error_out)) {
            fail(error_out != nullptr && !error_out->empty()
                ? *error_out : "job cancellation finalization failed");
            return false;
        }
        if (dispatch_attempt_id.has_value()
            && dispatch_state == "CLAIMED") {
            Statement remaining;
            if (sqlite3_prepare_v2(
                    db_,
                    "SELECT COUNT(1) FROM exec_job WHERE "
                    "dispatch_attempt_id=?1 "
                    "AND state IN ('CLAIMED','RUNNING');",
                    -1, &remaining.st, nullptr) != SQLITE_OK) {
                fail(sqlite3_errmsg(db_));
                return false;
            }
            sqlite3_bind_int64(
                remaining.st, 1, *dispatch_attempt_id);
            if (sqlite3_step(remaining.st) != SQLITE_ROW) {
                fail(sqlite3_errmsg(db_));
                return false;
            }
            if (sqlite3_column_int64(remaining.st, 0) == 0) {
                Statement close;
                if (sqlite3_prepare_v2(
                        db_,
                        "UPDATE exec_workset_dispatch_attempt SET "
                        "state='CLOSED',lease_expires_at_utc=NULL,"
                        "closed_at_utc=?1,"
                        "close_reason_code='ALL_ITEMS_CANCELED' "
                        "WHERE dispatch_attempt_id=?2 "
                        "AND state='CLAIMED';",
                        -1, &close.st, nullptr) != SQLITE_OK) {
                    fail(sqlite3_errmsg(db_));
                    return false;
                }
                sqlite3_bind_int64(close.st, 1, now);
                sqlite3_bind_int64(close.st, 2, *dispatch_attempt_id);
                if (sqlite3_step(close.st) != SQLITE_DONE) {
                    fail(sqlite3_errmsg(db_));
                    return false;
                }
            }
        }
        receipt.state = "RESOLVED";
        receipt.resolution_code = command.resolution_code;
    } else if (command.kind
        == JobCancellationOutcomeKind::WorkerDeliveryAccepted) {
        if (receipt.state != "REQUESTED") {
            receipt.disposition =
                ExecutionDbOperationDisposition::WrongState;
            if (receipt_out != nullptr) *receipt_out = receipt;
            return true;
        }
        Statement delivered;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE exec_job_cancellation_request SET "
                "state='DELIVERED',delivery_attempts=delivery_attempts+1,"
                "delivered_at_utc=?1,last_delivery_error_code=NULL,"
                "last_delivery_error_text=NULL,"
                "last_delivery_failed_at_utc=NULL "
                "WHERE cancellation_request_id=?2 AND state='REQUESTED';",
                -1, &delivered.st, nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int64(delivered.st, 1, now);
        sqlite3_bind_int64(delivered.st, 2,
            command.cancellation_request_id);
        if (sqlite3_step(delivered.st) != SQLITE_DONE
            || sqlite3_changes(db_) != 1) {
            fail("cancellation delivery CAS failed");
            return false;
        }
        Statement summary;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE exec_job SET cancellation_state='DELIVERED',"
                "cancellation_delivery_attempts="
                "cancellation_delivery_attempts+1,"
                "cancellation_delivered_at_utc=?1 "
                "WHERE job_id=?2 AND cancellation_state='REQUESTED';",
                -1, &summary.st, nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int64(summary.st, 1, now);
        sqlite3_bind_int64(summary.st, 2, command.job_id);
        if (sqlite3_step(summary.st) != SQLITE_DONE
            || sqlite3_changes(db_) != 1
            || !InsertJobActionEventAndOutbox(
                db_, command.job_id, job_set_id,
                "Execution.JobCancellationDelivered.v1",
                "durable-cancellation-delivered", error_out)) {
            fail(error_out != nullptr && !error_out->empty()
                ? *error_out : "cancellation delivery persistence failed");
            return false;
        }
        receipt.state = "DELIVERED";
    } else if (command.kind
        == JobCancellationOutcomeKind::DeliveryFailed) {
        Statement failure;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE exec_job_cancellation_request SET "
                "delivery_attempts=delivery_attempts+1,"
                "last_delivery_error_code=?1,"
                "last_delivery_error_text=?2,"
                "last_delivery_failed_at_utc=?3 "
                "WHERE cancellation_request_id=?4 "
                "AND state='REQUESTED';",
                -1, &failure.st, nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_text(failure.st, 1,
            command.error_code->c_str(), -1, SQLITE_TRANSIENT);
        BindOptionalText(failure.st, 2, command.error_text);
        sqlite3_bind_int64(failure.st, 3, now);
        sqlite3_bind_int64(failure.st, 4,
            command.cancellation_request_id);
        if (sqlite3_step(failure.st) != SQLITE_DONE
            || sqlite3_changes(db_) != 1) {
            fail("cancellation failure diagnostic CAS failed");
            return false;
        }
        Statement summary;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE exec_job SET "
                "cancellation_delivery_attempts="
                "cancellation_delivery_attempts+1,"
                "cancellation_last_delivery_error_code=?1,"
                "cancellation_last_delivery_error_text=?2,"
                "cancellation_last_delivery_failed_at_utc=?3 "
                "WHERE job_id=?4 AND cancellation_state='REQUESTED';",
                -1, &summary.st, nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_text(summary.st, 1,
            command.error_code->c_str(), -1, SQLITE_TRANSIENT);
        BindOptionalText(summary.st, 2, command.error_text);
        sqlite3_bind_int64(summary.st, 3, now);
        sqlite3_bind_int64(summary.st, 4, command.job_id);
        if (sqlite3_step(summary.st) != SQLITE_DONE) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
    } else {
        Statement resolve;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE exec_job_cancellation_request SET "
                "state='RESOLVED',resolved_at_utc=?1,resolution_code=?2 "
                "WHERE cancellation_request_id=?3 "
                "AND state IN ('REQUESTED','DELIVERED');",
                -1, &resolve.st, nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int64(resolve.st, 1, now);
        sqlite3_bind_text(resolve.st, 2,
            command.resolution_code.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(resolve.st, 3,
            command.cancellation_request_id);
        if (sqlite3_step(resolve.st) != SQLITE_DONE
            || sqlite3_changes(db_) != 1) {
            fail("terminal cancellation resolution CAS failed");
            return false;
        }
        Statement summary;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE exec_job SET cancellation_state='RESOLVED',"
                "cancellation_resolved_at_utc=?1,"
                "cancellation_resolution_code=?2 "
                "WHERE job_id=?3 "
                "AND cancellation_state IN ('REQUESTED','DELIVERED');",
                -1, &summary.st, nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int64(summary.st, 1, now);
        sqlite3_bind_text(summary.st, 2,
            command.resolution_code.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(summary.st, 3, command.job_id);
        if (sqlite3_step(summary.st) != SQLITE_DONE
            || sqlite3_changes(db_) != 1
            || !InsertJobActionEventAndOutbox(
                db_, command.job_id, job_set_id,
                "Execution.JobCancellationResolved.v1",
                "durable-cancellation-terminal-resolved", error_out)) {
            fail(error_out != nullptr && !error_out->empty()
                ? *error_out
                : "terminal cancellation summary persistence failed");
            return false;
        }
        receipt.state = "RESOLVED";
        receipt.resolution_code = command.resolution_code;
    }
    receipt.disposition = ExecutionDbOperationDisposition::Applied;
    if (receipt_out != nullptr) *receipt_out = receipt;
    return true;
}

bool SqliteExecutionDb::MutateJobCancellationsBatch(
    const MutateJobCancellationsBatchCommand& command,
    std::vector<JobCancellationReceipt>* receipts_out,
    std::string* error_out) {
    if (receipts_out != nullptr) receipts_out->clear();
    if (db_ == nullptr || command.mutations.empty()) {
        if (error_out != nullptr) *error_out = "invalid cancellation mutation batch";
        return false;
    }
    BatchTransactionScope batch(db_);
    if (!batch.Begin(error_out)) return false;
    std::vector<JobCancellationReceipt> receipts;
    receipts.reserve(command.mutations.size());
    for (const auto& mutation : command.mutations) {
        JobCancellationReceipt receipt{};
        if (!ApplyJobCancellationOutcomeInTransaction(
                mutation, &receipt, error_out)) return false;
        receipts.push_back(std::move(receipt));
    }
    if (!RefreshExecutionWorkAvailability(db_, error_out)) return false;
    if (!batch.CommitAll(error_out)) return false;
    if (receipts_out != nullptr) *receipts_out = std::move(receipts);
    return true;
}

bool SqliteExecutionDb::IsTempBlobTracked(
    std::string_view relative_path,
    bool* tracked_out,
    std::string* error_out) const {
    if (tracked_out != nullptr) {
        *tracked_out = false;
    }
    if (db_ == nullptr || tracked_out == nullptr
        || !IsSafeRelativeBlobPath(relative_path)) {
        if (error_out != nullptr) {
            *error_out = "invalid temporary-blob tracking query";
        }
        return false;
    }

    Statement query;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT EXISTS("
            "  SELECT 1 FROM exec_temp_blob "
            "  WHERE relative_path=?1 "
            "    AND cleanup_state IN ('LIVE','DELETE_PENDING')"
            ");",
            -1,
            &query.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }
    sqlite3_bind_text(
        query.st,
        1,
        relative_path.data(),
        static_cast<int>(relative_path.size()),
        SQLITE_TRANSIENT);
    if (sqlite3_step(query.st) != SQLITE_ROW) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }
    *tracked_out = sqlite3_column_int(query.st, 0) != 0;
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

std::optional<ClaimedTempBlobCleanup>
SqliteExecutionDb::ClaimNextTempBlobCleanup(
    const ClaimTempBlobCleanupCommand& command,
    std::string* error_out) {
    if (db_ == nullptr
        || command.cleanup_token.empty()
        || command.lease_duration_ms <= 0) {
        if (error_out != nullptr) {
            *error_out = "invalid temporary-blob cleanup claim command";
        }
        return std::nullopt;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return std::nullopt;
    }
    const auto fail = [&](std::string message) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = std::move(message);
    };
    const auto now = CurrentUtcMs(db_);
    if (now <= 0
        || command.lease_duration_ms
            > std::numeric_limits<std::int64_t>::max() - now) {
        fail("temporary-blob cleanup lease expiration overflow");
        return std::nullopt;
    }
    const auto lease_expires = now + command.lease_duration_ms;
    Statement candidate;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT temp_blob_id FROM exec_temp_blob "
            "WHERE cleanup_state='DELETE_PENDING' "
            "AND (cleanup_claim_token IS NULL "
            " OR cleanup_lease_expires_at_utc<?1) "
            "ORDER BY delete_pending_at_utc ASC,temp_blob_id ASC "
            "LIMIT 1;",
            -1,
            &candidate.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return std::nullopt;
    }
    sqlite3_bind_int64(candidate.st, 1, now);
    const auto candidate_rc = sqlite3_step(candidate.st);
    if (candidate_rc == SQLITE_DONE) {
        if (!Commit(db_, error_out)) {
            Rollback(db_);
        }
        return std::nullopt;
    }
    if (candidate_rc != SQLITE_ROW) {
        fail(sqlite3_errmsg(db_));
        return std::nullopt;
    }
    const auto blob_id = sqlite3_column_int64(candidate.st, 0);
    Statement claim;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_temp_blob SET cleanup_claim_token=?1,"
            "cleanup_lease_expires_at_utc=?2,"
            "cleanup_attempts=cleanup_attempts+1,"
            "cleanup_error=NULL "
            "WHERE temp_blob_id=?3 AND cleanup_state='DELETE_PENDING' "
            "AND (cleanup_claim_token IS NULL "
            " OR cleanup_lease_expires_at_utc<?4);",
            -1,
            &claim.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return std::nullopt;
    }
    sqlite3_bind_text(
        claim.st,
        1,
        command.cleanup_token.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_int64(claim.st, 2, lease_expires);
    sqlite3_bind_int64(claim.st, 3, blob_id);
    sqlite3_bind_int64(claim.st, 4, now);
    if (sqlite3_step(claim.st) != SQLITE_DONE
        || sqlite3_changes(db_) != 1) {
        fail("temporary-blob cleanup claim CAS failed");
        return std::nullopt;
    }
    Statement details;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT relative_path,sha256,size_bytes,format,"
            "cleanup_state,created_at_utc,cleanup_attempts "
            "FROM exec_temp_blob WHERE temp_blob_id=?1;",
            -1,
            &details.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return std::nullopt;
    }
    sqlite3_bind_int64(details.st, 1, blob_id);
    if (sqlite3_step(details.st) != SQLITE_ROW) {
        fail("claimed temporary blob is missing");
        return std::nullopt;
    }
    ClaimedTempBlobCleanup claimed{};
    claimed.blob.temp_blob_id = blob_id;
    claimed.blob.relative_path = Text(details.st, 0);
    claimed.blob.sha256 = Text(details.st, 1);
    claimed.blob.size_bytes = static_cast<std::uint64_t>(
        sqlite3_column_int64(details.st, 2));
    claimed.blob.format = Text(details.st, 3);
    claimed.blob.cleanup_state = Text(details.st, 4);
    claimed.blob.created_at_utc =
        sqlite3_column_int64(details.st, 5);
    claimed.cleanup_token = command.cleanup_token;
    claimed.cleanup_lease_expires_at_utc = lease_expires;
    claimed.cleanup_attempts = sqlite3_column_int(details.st, 6);
    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return std::nullopt;
    }
    return claimed;
}

bool SqliteExecutionDb::CompleteTempBlobCleanup(
    const CompleteTempBlobCleanupCommand& command,
    ExecutionDbOperationDisposition* disposition_out,
    std::string* error_out) {
    if (disposition_out != nullptr) {
        *disposition_out = ExecutionDbOperationDisposition::BackendError;
    }
    if (db_ == nullptr
        || command.temp_blob_id <= 0
        || command.cleanup_token.empty()) {
        if (disposition_out != nullptr) {
            *disposition_out =
                ExecutionDbOperationDisposition::InvalidRequest;
        }
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }
    const auto now = CurrentUtcMs(db_);
    Statement state;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT cleanup_state,cleanup_claim_token,"
            "cleanup_lease_expires_at_utc "
            "FROM exec_temp_blob WHERE temp_blob_id=?1;",
            -1,
            &state.st,
            nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(state.st, 1, command.temp_blob_id);
    if (sqlite3_step(state.st) != SQLITE_ROW) {
        Rollback(db_);
        if (disposition_out != nullptr) {
            *disposition_out = ExecutionDbOperationDisposition::Missing;
        }
        return true;
    }
    const auto cleanup_state = Text(state.st, 0);
    const auto cleanup_token = Text(state.st, 1);
    const auto lease = sqlite3_column_type(state.st, 2) == SQLITE_NULL
        ? 0
        : sqlite3_column_int64(state.st, 2);
    if (cleanup_state == "DELETED" && command.deleted) {
        if (!Commit(db_, error_out)) {
            Rollback(db_);
            return false;
        }
        if (disposition_out != nullptr) {
            *disposition_out =
                ExecutionDbOperationDisposition::AlreadyApplied;
        }
        return true;
    }
    if (cleanup_token != command.cleanup_token
        || cleanup_state != "DELETE_PENDING"
        || lease < now) {
        Rollback(db_);
        if (disposition_out != nullptr) {
            *disposition_out =
                cleanup_token != command.cleanup_token
                ? ExecutionDbOperationDisposition::TokenMismatch
                : lease < now
                    ? ExecutionDbOperationDisposition::LeaseExpired
                    : ExecutionDbOperationDisposition::WrongState;
        }
        return true;
    }
    Statement update;
    const char* sql = command.deleted
        ? "UPDATE exec_temp_blob SET cleanup_state='DELETED',"
          "cleanup_claim_token=NULL,"
          "cleanup_lease_expires_at_utc=NULL,cleanup_error=NULL,"
          "deleted_at_utc=?1 "
          "WHERE temp_blob_id=?2 AND cleanup_state='DELETE_PENDING' "
          "AND cleanup_claim_token=?3 "
          "AND cleanup_lease_expires_at_utc>=?1;"
        : "UPDATE exec_temp_blob SET "
          "cleanup_claim_token=NULL,"
          "cleanup_lease_expires_at_utc=NULL,cleanup_error=?4 "
          "WHERE temp_blob_id=?2 AND cleanup_state='DELETE_PENDING' "
          "AND cleanup_claim_token=?3 "
          "AND cleanup_lease_expires_at_utc>=?1;";
    if (sqlite3_prepare_v2(
            db_, sql, -1, &update.st, nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(update.st, 1, now);
    sqlite3_bind_int64(update.st, 2, command.temp_blob_id);
    sqlite3_bind_text(
        update.st,
        3,
        command.cleanup_token.c_str(),
        -1,
        SQLITE_TRANSIENT);
    if (!command.deleted) {
        BindOptionalText(update.st, 4, command.cleanup_error);
    }
    if (sqlite3_step(update.st) != SQLITE_DONE
        || sqlite3_changes(db_) != 1
        || !Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    if (disposition_out != nullptr) {
        *disposition_out = ExecutionDbOperationDisposition::Applied;
    }
    return true;
}

bool SqliteExecutionDb::RecoverInterruptedWorksetDispatches(
    RecoverInterruptedWorksetDispatchesReceipt* receipt_out,
    std::string* error_out) {
    RecoverInterruptedWorksetDispatchesReceipt receipt{};
    if (receipt_out != nullptr) *receipt_out = receipt;
    if (db_ == nullptr) {
        if (error_out != nullptr) *error_out = "database handle is null";
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) return false;
    const auto fail = [&](std::string message) {
        Rollback(db_);
        if (error_out != nullptr) *error_out = std::move(message);
    };
    struct OpenDispatch {
        std::int64_t dispatch_attempt_id = 0;
        std::int64_t workset_id = 0;
    };
    std::vector<OpenDispatch> dispatches;
    Statement list_dispatches;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT dispatch_attempt_id,workset_id "
            "FROM exec_workset_dispatch_attempt "
            "WHERE state IN ('CLAIMED','ACTIVE','DRAINING') "
            "ORDER BY dispatch_attempt_id ASC;",
            -1,
            &list_dispatches.st,
            nullptr) != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }
    for (;;) {
        const auto rc = sqlite3_step(list_dispatches.st);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        dispatches.push_back({
            .dispatch_attempt_id = sqlite3_column_int64(list_dispatches.st, 0),
            .workset_id = sqlite3_column_int64(list_dispatches.st, 1),
        });
    }
    const auto now = CurrentUtcMs(db_);
    for (const auto& dispatch : dispatches) {
        struct InterruptedJob {
            std::int64_t job_id = 0;
            std::int64_t job_set_id = 0;
            std::string state;
            int attempts = 0;
            int max_attempts = 0;
        };
        std::vector<InterruptedJob> jobs;
        Statement list_jobs;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT j.job_id,j.job_set_id,j.state,j.attempts,j.max_attempts "
                "FROM exec_job j "
                "JOIN exec_workset_dispatch_attempt d "
                " ON d.dispatch_attempt_id=j.dispatch_attempt_id "
                "WHERE j.dispatch_attempt_id=?1 "
                "AND j.state IN ('CLAIMED','RUNNING') "
                "AND j.claimed_by_token=d.claim_token "
                "AND j.dispatch_item_ordinal IS NOT NULL "
                "AND j.reserved_attempt_id IS NOT NULL "
                "ORDER BY j.dispatch_item_ordinal ASC;",
                -1,
                &list_jobs.st,
                nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int64(
            list_jobs.st, 1, dispatch.dispatch_attempt_id);
        for (;;) {
            const auto rc = sqlite3_step(list_jobs.st);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) {
                fail(sqlite3_errmsg(db_));
                return false;
            }
            jobs.push_back({
                .job_id = sqlite3_column_int64(list_jobs.st, 0),
                .job_set_id = sqlite3_column_int64(list_jobs.st, 1),
                .state = Text(list_jobs.st, 2),
                .attempts = sqlite3_column_int(list_jobs.st, 3),
                .max_attempts = sqlite3_column_int(list_jobs.st, 4),
            });
        }
        Statement unfinished_count;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT COUNT(1) FROM exec_job "
                "WHERE dispatch_attempt_id=?1 "
                "AND state IN ('CLAIMED','RUNNING');",
                -1,
                &unfinished_count.st,
                nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int64(
            unfinished_count.st, 1, dispatch.dispatch_attempt_id);
        if (sqlite3_step(unfinished_count.st) != SQLITE_ROW
            || sqlite3_column_int(unfinished_count.st, 0)
                != static_cast<int>(jobs.size())) {
            fail(
                "interrupted dispatch has contradictory job authority: dispatch_attempt_id="
                + std::to_string(dispatch.dispatch_attempt_id));
            return false;
        }
        for (const auto& job : jobs) {
            const bool grant_attempt = job.state == "RUNNING"
                && job.attempts >= job.max_attempts;
            Statement requeue;
            if (sqlite3_prepare_v2(
                    db_,
                    "UPDATE exec_job SET state='QUEUED',queued_at_utc=?1,"
                    "max_attempts=CASE WHEN ?2<>0 THEN attempts+1 ELSE max_attempts END,"
                    "claimed_by_token=NULL,lease_expires_at_utc=NULL,"
                    "started_at_utc=NULL,"
                    "dispatch_attempt_id=NULL,dispatch_item_ordinal=NULL,"
                    "reserved_attempt_id=NULL "
                    "WHERE job_id=?3 AND dispatch_attempt_id=?4 "
                    "AND state=?5;",
                    -1,
                    &requeue.st,
                    nullptr) != SQLITE_OK) {
                fail(sqlite3_errmsg(db_));
                return false;
            }
            sqlite3_bind_int64(requeue.st, 1, now);
            sqlite3_bind_int(requeue.st, 2, grant_attempt ? 1 : 0);
            sqlite3_bind_int64(requeue.st, 3, job.job_id);
            sqlite3_bind_int64(
                requeue.st, 4, dispatch.dispatch_attempt_id);
            sqlite3_bind_text(
                requeue.st, 5, job.state.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(requeue.st) != SQLITE_DONE
                || sqlite3_changes(db_) != 1
                || !InsertJobActionEventAndOutbox(
                    db_,
                    job.job_id,
                    job.job_set_id,
                    "Execution.JobClaimRequeued.v1",
                    grant_attempt
                        ? "coordinator-restart-recovery-attempt-granted"
                        : "coordinator-restart-recovery",
                    error_out)) {
                fail(error_out != nullptr && !error_out->empty()
                    ? *error_out
                    : "interrupted workset job recovery failed");
                return false;
            }
            ++receipt.jobs_requeued;
            if (grant_attempt) ++receipt.recovery_attempts_granted;
        }
        Statement close;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE exec_workset_dispatch_attempt "
                "SET state='CLOSED',lease_expires_at_utc=NULL,"
                "closed_at_utc=?1,"
                "close_reason_code='COORDINATOR_RESTART_RECOVERY',"
                "close_reason_text='managed workers do not survive coordinator restart' "
                "WHERE dispatch_attempt_id=?2 "
                "AND state IN ('CLAIMED','ACTIVE','DRAINING');",
                -1,
                &close.st,
                nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_bind_int64(close.st, 1, now);
        sqlite3_bind_int64(
            close.st, 2, dispatch.dispatch_attempt_id);
        if (sqlite3_step(close.st) != SQLITE_DONE
            || sqlite3_changes(db_) != 1
            || !InsertAggregateOutboxEvent(
                db_,
                "Execution.WorksetReleased.v1",
                "workset",
                dispatch.workset_id,
                "workset",
                dispatch.workset_id,
                "restart-recovery-"
                    + std::to_string(dispatch.dispatch_attempt_id),
                error_out)) {
            fail(error_out != nullptr && !error_out->empty()
                ? *error_out
                : "interrupted workset dispatch close failed");
            return false;
        }
        ++receipt.dispatches_closed;
    }
    if (!RefreshExecutionWorkAvailability(db_, error_out)
        || !Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    if (receipt_out != nullptr) *receipt_out = receipt;
    return true;
}


std::optional<ClaimedExecutionJob> SqliteExecutionDb::ClaimNextReadyExecutionJob(
    std::string_view claimed_by_token,
    std::int64_t lease_duration_ms,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return std::nullopt;
    }
    if (claimed_by_token.empty()) {
        if (error_out) *error_out = "claimed_by_token is required";
        return std::nullopt;
    }
    if (lease_duration_ms <= 0) {
        if (error_out) *error_out = "lease_duration_ms must be > 0";
        return std::nullopt;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return std::nullopt;
    }
    const auto rollback = [&]() { (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr); };

    const auto now_utc = CurrentUtcMs(db_);
    const auto lease_expires_at_utc = now_utc + lease_duration_ms;

    ClaimedExecutionJob claimed{};
    {
        std::int64_t candidate_job_id = 0;
        {
            Statement candidate_st;
            if (sqlite3_prepare_v2(db_,
                "SELECT job_id, claimed_by_token, lease_expires_at_utc "
                "FROM exec_job "
                "WHERE state='QUEUED' "
                "  AND workset_id IS NULL "
                "  AND attempts < max_attempts "
                "  AND (claimed_by_token IS NULL OR claimed_by_token='') "
                "ORDER BY priority DESC, queued_at_utc ASC, job_id ASC "
                "LIMIT 1;",
                -1,
                &candidate_st.st,
                nullptr)
                != SQLITE_OK) {
                rollback();
                if (error_out) *error_out = sqlite3_errmsg(db_);
                return std::nullopt;
            }
            const auto rc = sqlite3_step(candidate_st.st);
            if (rc == SQLITE_ROW) {
                candidate_job_id = sqlite3_column_int64(candidate_st.st, 0);
                if (sqlite3_column_type(candidate_st.st, 1) != SQLITE_NULL) {
                    const auto* token = sqlite3_column_text(candidate_st.st, 1);
                    if (token != nullptr && token[0] != '\0') {
                        claimed.previous_claimed_by_token = reinterpret_cast<const char*>(token);
                    }
                }
                if (sqlite3_column_type(candidate_st.st, 2) != SQLITE_NULL) {
                    claimed.previous_lease_expires_at_utc = sqlite3_column_int64(candidate_st.st, 2);
                }
            } else if (rc != SQLITE_DONE) {
                rollback();
                if (error_out) *error_out = sqlite3_errmsg(db_);
                return std::nullopt;
            }
        }

        if (candidate_job_id <= 0) {
            char* commit_error = nullptr;
            const auto commit_rc = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &commit_error);
            if (commit_rc != SQLITE_OK) {
                const std::string message = commit_error ? commit_error : sqlite3_errmsg(db_);
                sqlite3_free(commit_error);
                rollback();
                if (error_out) *error_out = message;
                return std::nullopt;
            }
            sqlite3_free(commit_error);
            return std::nullopt;
        }

        Statement claim_st;
        if (sqlite3_prepare_v2(db_,
            "UPDATE exec_job "
                "SET state='CLAIMED', claimed_by_token=?1, lease_expires_at_utc=?2 "
                "WHERE job_id=?3 "
                "AND state='QUEUED' "
                "AND workset_id IS NULL "
                "AND attempts < max_attempts "
                "AND (claimed_by_token IS NULL OR claimed_by_token='') "
                "RETURNING job_id, job_set_id, savestate_id, program_kind, program_ref_kind, program_ref_id, "
                "priority, queued_at_utc, attempts, max_attempts;",
            -1,
            &claim_st.st,
            nullptr)
            != SQLITE_OK) {
            rollback();
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return std::nullopt;
        }

        const auto token = std::string(claimed_by_token);
        sqlite3_bind_text(claim_st.st, 1, token.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(claim_st.st, 2, lease_expires_at_utc);
        sqlite3_bind_int64(claim_st.st, 3, candidate_job_id);

        const auto rc = sqlite3_step(claim_st.st);
        if (rc == SQLITE_ROW) {
            claimed.job_id = sqlite3_column_int64(claim_st.st, 0);
            claimed.job_set_id = sqlite3_column_int64(claim_st.st, 1);
            if (sqlite3_column_type(claim_st.st, 2) != SQLITE_NULL) {
                claimed.savestate_affinity_key = "savestate:" + std::to_string(sqlite3_column_int64(claim_st.st, 2));
            }
            const auto program_kind = sqlite3_column_int(claim_st.st, 3);
            const auto* program_ref_kind_text = sqlite3_column_text(claim_st.st, 4);
            const auto program_ref_kind = program_ref_kind_text ? reinterpret_cast<const char*>(program_ref_kind_text) : "";
            const auto program_ref_id = sqlite3_column_int64(claim_st.st, 5);
            claimed.program_runtime_affinity_key =
                std::to_string(program_kind) + ":" + program_ref_kind + ":" + std::to_string(program_ref_id);
            claimed.claimed_by_token = token;
            claimed.lease_expires_at_utc = lease_expires_at_utc;
            claimed.priority = sqlite3_column_int(claim_st.st, 6);
            claimed.queued_at_utc = sqlite3_column_int64(claim_st.st, 7);
            const auto attempts = sqlite3_column_int(claim_st.st, 8);
            const auto max_attempts =
                sqlite3_column_int(claim_st.st, 9);
            if (attempts < 0 || attempts >= max_attempts) {
                rollback();
                if (error_out) {
                    *error_out =
                        "claimed execution job has no durable attempt "
                        "remaining";
                }
                return std::nullopt;
            }
            claimed.durable_attempt_id =
                static_cast<std::uint64_t>(attempts) + 1;

            std::string step_error;
            if (!ResolveWorkflowStepForJobSetAncestry(db_, claimed.job_set_id, &claimed, &step_error)) {
                rollback();
                if (error_out) *error_out = step_error;
                return std::nullopt;
            }
        } else if (rc != SQLITE_DONE) {
            rollback();
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return std::nullopt;
        }
    }

    if (claimed.job_id > 0
        && !InsertJobActionEventAndOutbox(
            db_,
            claimed.job_id,
            claimed.job_set_id,
            "Execution.JobClaimed.v1",
            "CLAIM",
            error_out)) {
        rollback();
        return std::nullopt;
    }

    char* commit_error = nullptr;
    const auto commit_rc = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &commit_error);
    if (commit_rc != SQLITE_OK) {
        const std::string message = commit_error ? commit_error : sqlite3_errmsg(db_);
        sqlite3_free(commit_error);
        rollback();
        if (error_out) *error_out = message;
        return std::nullopt;
    }
    sqlite3_free(commit_error);

    if (claimed.job_id <= 0) {
        return std::nullopt;
    }
    return claimed;
}

std::vector<ClaimedExecutionJob> SqliteExecutionDb::ClaimBatchReadyExecutionJobs(
    std::string_view claimed_by_token,
    int requested_jobs,
    std::int64_t lease_duration_ms,
    std::string* error_out) {
    std::vector<ClaimedExecutionJob> claimed;
    if (error_out != nullptr) {
        error_out->clear();
    }
    if (requested_jobs <= 0) {
        return claimed;
    }
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return claimed;
    }
    if (claimed_by_token.empty()) {
        if (error_out) *error_out = "claimed_by_token is required";
        return claimed;
    }
    if (lease_duration_ms <= 0) {
        if (error_out) *error_out = "lease_duration_ms must be > 0";
        return claimed;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return claimed;
    }

    const auto fail = [&](std::string message) {
        Rollback(db_);
        claimed.clear();
        if (error_out != nullptr) {
            *error_out = std::move(message);
        }
    };
    const auto now_utc = CurrentUtcMs(db_);
    const auto lease_expires_at_utc = now_utc + lease_duration_ms;

    Statement candidates;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT job_id, job_set_id, savestate_id, program_kind, "
            "program_ref_kind, program_ref_id, claimed_by_token, "
            "lease_expires_at_utc, priority, queued_at_utc, "
            "attempts, max_attempts "
            "FROM exec_job "
            "WHERE state='QUEUED' "
            "  AND workset_id IS NULL "
            "  AND attempts < max_attempts "
            "  AND (claimed_by_token IS NULL OR claimed_by_token='') "
            "ORDER BY priority DESC, queued_at_utc ASC, job_id ASC "
            "LIMIT ?1;",
            -1,
            &candidates.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return claimed;
    }
    sqlite3_bind_int(candidates.st, 1, requested_jobs);

    claimed.reserve(static_cast<std::size_t>(requested_jobs));
    for (;;) {
        const auto rc = sqlite3_step(candidates.st);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            fail(sqlite3_errmsg(db_));
            return claimed;
        }

        ClaimedExecutionJob row{};
        row.job_id = sqlite3_column_int64(candidates.st, 0);
        row.job_set_id = sqlite3_column_int64(candidates.st, 1);
        if (sqlite3_column_type(candidates.st, 2) != SQLITE_NULL) {
            row.savestate_affinity_key =
                "savestate:" + std::to_string(sqlite3_column_int64(candidates.st, 2));
        }
        const auto program_kind = sqlite3_column_int(candidates.st, 3);
        const auto* program_ref_kind_text = sqlite3_column_text(candidates.st, 4);
        const auto program_ref_kind = program_ref_kind_text != nullptr
            ? reinterpret_cast<const char*>(program_ref_kind_text)
            : "";
        const auto program_ref_id = sqlite3_column_int64(candidates.st, 5);
        row.program_runtime_affinity_key =
            std::to_string(program_kind) + ":" + program_ref_kind + ":"
            + std::to_string(program_ref_id);
        if (sqlite3_column_type(candidates.st, 6) != SQLITE_NULL) {
            const auto* previous = sqlite3_column_text(candidates.st, 6);
            if (previous != nullptr && previous[0] != '\0') {
                row.previous_claimed_by_token =
                    reinterpret_cast<const char*>(previous);
            }
        }
        if (sqlite3_column_type(candidates.st, 7) != SQLITE_NULL) {
            row.previous_lease_expires_at_utc =
                sqlite3_column_int64(candidates.st, 7);
        }
        row.priority = sqlite3_column_int(candidates.st, 8);
        row.queued_at_utc = sqlite3_column_int64(candidates.st, 9);
        const auto attempts = sqlite3_column_int(candidates.st, 10);
        const auto max_attempts =
            sqlite3_column_int(candidates.st, 11);
        if (attempts < 0 || attempts >= max_attempts) {
            fail("claimed execution job has no durable attempt remaining");
            return claimed;
        }
        row.durable_attempt_id =
            static_cast<std::uint64_t>(attempts) + 1;
        row.claimed_by_token = std::string(claimed_by_token) + ":"
            + std::to_string(row.job_id);
        row.lease_expires_at_utc = lease_expires_at_utc;

        std::string step_error;
        if (!ResolveWorkflowStepForJobSetAncestry(
                db_,
                row.job_set_id,
                &row,
                &step_error)) {
            fail(std::move(step_error));
            return claimed;
        }
        claimed.push_back(std::move(row));
    }

    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job "
            "SET state='CLAIMED', claimed_by_token=?1, lease_expires_at_utc=?2 "
            "WHERE job_id=?3 "
            "  AND state='QUEUED' "
            "  AND workset_id IS NULL "
            "  AND attempts < max_attempts "
            "  AND (claimed_by_token IS NULL OR claimed_by_token='');",
            -1,
            &update.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return claimed;
    }

    for (const auto& row : claimed) {
        sqlite3_reset(update.st);
        sqlite3_clear_bindings(update.st);
        sqlite3_bind_text(
            update.st,
            1,
            row.claimed_by_token.c_str(),
            -1,
            SQLITE_TRANSIENT);
        sqlite3_bind_int64(update.st, 2, lease_expires_at_utc);
        sqlite3_bind_int64(update.st, 3, row.job_id);
        if (sqlite3_step(update.st) != SQLITE_DONE
            || sqlite3_changes(db_) != 1) {
            fail(sqlite3_errmsg(db_));
            return claimed;
        }
        if (!InsertJobActionEventAndOutbox(
                db_,
                row.job_id,
                row.job_set_id,
                "Execution.JobClaimed.v1",
                "CLAIM",
                error_out)) {
            const auto error = error_out != nullptr
                ? *error_out
                : std::string(sqlite3_errmsg(db_));
            fail(error);
            return claimed;
        }
    }

    if (!Commit(db_, error_out)) {
        const auto error = error_out != nullptr
            ? *error_out
            : std::string(sqlite3_errmsg(db_));
        fail(error);
        return claimed;
    }
    return claimed;
}

bool SqliteExecutionDb::RenewExecutionJobLease(
    std::int64_t job_id,
    std::string_view claimed_by_token,
    std::int64_t lease_duration_ms,
    bool* renewed_out,
    std::string* error_out) {
    if (renewed_out) *renewed_out = false;
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (job_id <= 0) {
        if (error_out) *error_out = "job_id must be > 0";
        return false;
    }
    if (claimed_by_token.empty()) {
        if (error_out) *error_out = "claimed_by_token is required";
        return false;
    }
    if (lease_duration_ms <= 0) {
        if (error_out) *error_out = "lease_duration_ms must be > 0";
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(db_,
        "UPDATE exec_job "
            "SET lease_expires_at_utc=?1 "
            "WHERE job_id=?2 "
            "AND state IN ('CLAIMED','RUNNING') "
            "AND claimed_by_token=?3 "
            "AND workset_id IS NULL "
            "AND COALESCE(lease_expires_at_utc, 0) > ?4;",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    const auto now_utc = CurrentUtcMs(db_);
    const auto lease_expires_at_utc = now_utc + lease_duration_ms;
    const auto token = std::string(claimed_by_token);
    sqlite3_bind_int64(st.st, 1, lease_expires_at_utc);
    sqlite3_bind_int64(st.st, 2, job_id);
    sqlite3_bind_text(st.st, 3, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 4, now_utc);

    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (renewed_out) *renewed_out = sqlite3_changes(db_) > 0;
    return true;
}

std::optional<std::string> OptionalText(sqlite3_stmt* st, int index) {
    if (sqlite3_column_type(st, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    const auto* text = sqlite3_column_text(st, index);
    return text != nullptr
        ? std::optional<std::string>(
            reinterpret_cast<const char*>(text))
        : std::optional<std::string>(std::string{});
}

std::string Text(sqlite3_stmt* st, int index) {
    const auto* text = sqlite3_column_text(st, index);
    return text != nullptr ? reinterpret_cast<const char*>(text) : "";
}

void BindOptionalText(
    sqlite3_stmt* st,
    int index,
    const std::optional<std::string>& value) {
    if (value.has_value()) {
        sqlite3_bind_text(
            st,
            index,
            value->c_str(),
            -1,
            SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(st, index);
    }
}

bool IsSha256(std::string_view value) {
    return value.size() == 64
        && std::all_of(
            value.begin(),
            value.end(),
            [](unsigned char ch) {
                return std::isxdigit(ch) != 0;
            });
}

bool IsSafeRelativeBlobPath(std::string_view value) {
    if (value.empty()
        || value.front() == '/'
        || value.front() == '\\'
        || value.find('\\') != std::string_view::npos
        || value.find(':') != std::string_view::npos
        || value.find('\0') != std::string_view::npos
        || (value.size() >= 2
            && std::isalpha(
                static_cast<unsigned char>(value.front())) != 0
            && value[1] == ':')) {
        return false;
    }
    if (value.rfind("worker_results/", 0) != 0
        || value.size() <= std::string_view("worker_results/").size()) {
        return false;
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('/', start);
        const auto component = value.substr(
            start,
            end == std::string::npos
                ? std::string::npos
                : end - start);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

bool InsertAggregateOutboxEvent(
    sqlite3* db,
    const char* event_type,
    const char* aggregate_kind,
    std::int64_t aggregate_id_value,
    const char* payload_ref_kind,
    std::int64_t payload_ref_id,
    std::string_view causation_suffix,
    std::string* error_out) {
    const auto now = CurrentUtcMs(db);
    const auto aggregate_id = std::to_string(aggregate_id_value);
    const auto event_id = std::string("execution-")
        + event_type + "-" + aggregate_id + "-"
        + std::string(causation_suffix);
    const auto causation_id = std::string("execution-db-")
        + std::string(causation_suffix);

    Statement outbox;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO exec_outbox_message("
            "event_id,event_type,event_version,context_name,aggregate_kind,"
            "aggregate_id,correlation_id,causation_id,occurred_at_utc,"
            "payload_ref_kind,payload_ref_id) "
            "VALUES(?1,?2,1,'Execution',?3,?4,NULL,?5,?6,?7,?8);",
            -1,
            &outbox.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db);
        }
        return false;
    }
    sqlite3_bind_text(
        outbox.st,
        1,
        event_id.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_text(
        outbox.st,
        2,
        event_type,
        -1,
        SQLITE_STATIC);
    sqlite3_bind_text(
        outbox.st,
        3,
        aggregate_kind,
        -1,
        SQLITE_STATIC);
    sqlite3_bind_text(
        outbox.st,
        4,
        aggregate_id.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_text(
        outbox.st,
        5,
        causation_id.c_str(),
        -1,
        SQLITE_TRANSIENT);
    sqlite3_bind_int64(outbox.st, 6, now);
    sqlite3_bind_text(
        outbox.st,
        7,
        payload_ref_kind,
        -1,
        SQLITE_STATIC);
    sqlite3_bind_int64(outbox.st, 8, payload_ref_id);
    if (sqlite3_step(outbox.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db);
        }
        return false;
    }
    return true;
}

std::vector<ExecutionJobLeaseRenewalReceipt>
SqliteExecutionDb::RenewExecutionJobLeases(
    const std::vector<ExecutionJobLeaseRequest>& requests,
    std::int64_t lease_duration_ms,
    std::string* error_out) {
    std::vector<ExecutionJobLeaseRenewalReceipt> receipts;
    receipts.reserve(requests.size());
    if (error_out != nullptr) {
        error_out->clear();
    }
    if (requests.empty()) {
        return receipts;
    }
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return receipts;
    }
    if (lease_duration_ms <= 0) {
        if (error_out) *error_out = "lease_duration_ms must be > 0";
        return receipts;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return receipts;
    }
    const auto fail = [&](const std::string& message) {
        Rollback(db_);
        receipts.clear();
        receipts.reserve(requests.size());
        for (const auto& request : requests) {
            receipts.push_back(ExecutionJobLeaseRenewalReceipt{
                .job_id = request.job_id,
                .disposition =
                    ExecutionJobLeaseRenewalDisposition::BackendError,
            });
        }
        if (error_out != nullptr) {
            *error_out = message;
        }
    };

    Statement query;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT state, claimed_by_token, lease_expires_at_utc,"
            "workset_id "
            "FROM exec_job WHERE job_id=?1;",
            -1,
            &query.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return receipts;
    }
    Statement update;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE exec_job SET lease_expires_at_utc=?1 "
            "WHERE job_id=?2 "
            "  AND state IN ('CLAIMED','RUNNING') "
            "  AND claimed_by_token=?3 "
            "  AND workset_id IS NULL "
            "  AND COALESCE(lease_expires_at_utc,0)>?4;",
            -1,
            &update.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return receipts;
    }

    const auto now_utc = CurrentUtcMs(db_);
    const auto renewed_expiry = now_utc + lease_duration_ms;
    for (const auto& request : requests) {
        ExecutionJobLeaseRenewalReceipt receipt{
            .job_id = request.job_id,
        };
        if (request.job_id <= 0 || request.claimed_by_token.empty()) {
            receipt.disposition =
                ExecutionJobLeaseRenewalDisposition::InvalidRequest;
            receipts.push_back(std::move(receipt));
            continue;
        }

        sqlite3_reset(query.st);
        sqlite3_clear_bindings(query.st);
        sqlite3_bind_int64(query.st, 1, request.job_id);
        const auto query_rc = sqlite3_step(query.st);
        if (query_rc == SQLITE_DONE) {
            receipt.disposition =
                ExecutionJobLeaseRenewalDisposition::Missing;
            receipts.push_back(std::move(receipt));
            continue;
        }
        if (query_rc != SQLITE_ROW) {
            fail(sqlite3_errmsg(db_));
            return receipts;
        }

        const auto* state_text = sqlite3_column_text(query.st, 0);
        const std::string state = state_text != nullptr
            ? reinterpret_cast<const char*>(state_text)
            : "";
        const auto* token_text = sqlite3_column_text(query.st, 1);
        const std::string token = token_text != nullptr
            ? reinterpret_cast<const char*>(token_text)
            : "";
        const auto lease_expiry =
            sqlite3_column_type(query.st, 2) != SQLITE_NULL
            ? sqlite3_column_int64(query.st, 2)
            : 0;
        const bool workset_backed =
            sqlite3_column_type(query.st, 3) != SQLITE_NULL;
        receipt.lease_expires_at_utc = lease_expiry;

        if (workset_backed
            || (state != "CLAIMED" && state != "RUNNING")) {
            receipt.disposition =
                ExecutionJobLeaseRenewalDisposition::WrongState;
        } else if (token != request.claimed_by_token) {
            receipt.disposition =
                ExecutionJobLeaseRenewalDisposition::TokenMismatch;
        } else if (lease_expiry <= now_utc) {
            receipt.disposition =
                ExecutionJobLeaseRenewalDisposition::Expired;
        } else {
            sqlite3_reset(update.st);
            sqlite3_clear_bindings(update.st);
            sqlite3_bind_int64(update.st, 1, renewed_expiry);
            sqlite3_bind_int64(update.st, 2, request.job_id);
            sqlite3_bind_text(
                update.st,
                3,
                request.claimed_by_token.c_str(),
                -1,
                SQLITE_TRANSIENT);
            sqlite3_bind_int64(update.st, 4, now_utc);
            if (sqlite3_step(update.st) != SQLITE_DONE
                || sqlite3_changes(db_) != 1) {
                fail(sqlite3_errmsg(db_));
                return receipts;
            }
            receipt.disposition =
                ExecutionJobLeaseRenewalDisposition::Renewed;
            receipt.lease_expires_at_utc = renewed_expiry;
        }
        receipts.push_back(std::move(receipt));
    }

    if (!Commit(db_, error_out)) {
        const auto error = error_out != nullptr
            ? *error_out
            : std::string(sqlite3_errmsg(db_));
        fail(error);
    }
    return receipts;
}

bool SqliteExecutionDb::ValidateExecutionJobStartAuthoritySet(
    const std::vector<ExecutionJobLeaseRequest>& requests,
    ExecutionJobStartAuthoritySetReceipt* receipt_out,
    std::string* error_out) {
    ExecutionJobStartAuthoritySetReceipt receipt{};
    receipt.items.reserve(requests.size());
    if (receipt_out != nullptr) {
        *receipt_out = receipt;
    }
    if (error_out != nullptr) {
        error_out->clear();
    }
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    const auto fail = [&](const std::string& message) {
        Rollback(db_);
        receipt.all_valid = false;
        receipt.items.clear();
        receipt.items.reserve(requests.size());
        for (const auto& request : requests) {
            receipt.items.push_back(
                ExecutionJobStartAuthorityItemReceipt{
                    .job_id = request.job_id,
                    .disposition =
                        ExecutionJobStartAuthorityDisposition::
                            BackendError,
                });
        }
        if (receipt_out != nullptr) {
            *receipt_out = receipt;
        }
        if (error_out != nullptr) {
            *error_out = message;
        }
    };

    Statement query;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT state, claimed_by_token, lease_expires_at_utc,"
            "workset_id "
            "FROM exec_job WHERE job_id=?1;",
            -1,
            &query.st,
            nullptr)
        != SQLITE_OK) {
        fail(sqlite3_errmsg(db_));
        return false;
    }

    receipt.validated_at_utc = CurrentUtcMs(db_);
    receipt.all_valid = !requests.empty();
    std::unordered_set<std::int64_t> seen_job_ids;
    seen_job_ids.reserve(requests.size());
    for (const auto& request : requests) {
        ExecutionJobStartAuthorityItemReceipt item{
            .job_id = request.job_id,
        };
        if (request.job_id <= 0 || request.claimed_by_token.empty()) {
            item.disposition =
                ExecutionJobStartAuthorityDisposition::InvalidRequest;
        } else if (!seen_job_ids.emplace(request.job_id).second) {
            item.disposition =
                ExecutionJobStartAuthorityDisposition::DuplicateJob;
        } else {
            sqlite3_reset(query.st);
            sqlite3_clear_bindings(query.st);
            sqlite3_bind_int64(query.st, 1, request.job_id);
            const auto rc = sqlite3_step(query.st);
            if (rc == SQLITE_DONE) {
                item.disposition =
                    ExecutionJobStartAuthorityDisposition::Missing;
            } else if (rc != SQLITE_ROW) {
                fail(sqlite3_errmsg(db_));
                return false;
            } else {
                const auto* state_text = sqlite3_column_text(query.st, 0);
                const std::string state = state_text != nullptr
                    ? reinterpret_cast<const char*>(state_text)
                    : "";
                const auto* token_text = sqlite3_column_text(query.st, 1);
                const std::string token = token_text != nullptr
                    ? reinterpret_cast<const char*>(token_text)
                    : "";
                const auto lease_expiry =
                    sqlite3_column_type(query.st, 2) != SQLITE_NULL
                    ? sqlite3_column_int64(query.st, 2)
                    : 0;
                const bool workset_backed =
                    sqlite3_column_type(query.st, 3) != SQLITE_NULL;
                item.lease_expires_at_utc = lease_expiry;
                if (workset_backed || state != "CLAIMED") {
                    item.disposition =
                        ExecutionJobStartAuthorityDisposition::
                            WrongState;
                } else if (token != request.claimed_by_token) {
                    item.disposition =
                        ExecutionJobStartAuthorityDisposition::
                            TokenMismatch;
                } else if (lease_expiry
                    <= receipt.validated_at_utc) {
                    item.disposition =
                        ExecutionJobStartAuthorityDisposition::Expired;
                } else {
                    item.disposition =
                        ExecutionJobStartAuthorityDisposition::Valid;
                }
            }
        }
        if (item.disposition
            != ExecutionJobStartAuthorityDisposition::Valid) {
            receipt.all_valid = false;
        }
        receipt.items.push_back(std::move(item));
    }

    if (!Commit(db_, error_out)) {
        const auto error = error_out != nullptr
            ? *error_out
            : std::string(sqlite3_errmsg(db_));
        fail(error);
        return false;
    }
    if (receipt_out != nullptr) {
        *receipt_out = std::move(receipt);
    }
    return true;
}

bool SqliteExecutionDb::MarkExecutionJobStarted(
    std::int64_t job_id,
    std::string_view claimed_by_token,
    std::string_view requested_by,
    ExecutionJobStartReceipt* receipt_out,
    std::string* error_out) {
    ExecutionJobStartReceipt receipt{
        .job_id = job_id,
        .disposition = ExecutionJobStartDisposition::BackendError,
    };
    if (receipt_out != nullptr) {
        *receipt_out = receipt;
    }
    if (error_out != nullptr) {
        error_out->clear();
    }
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (job_id <= 0 || claimed_by_token.empty() || requested_by.empty()) {
        receipt.disposition = ExecutionJobStartDisposition::InvalidRequest;
        if (receipt_out != nullptr) {
            *receipt_out = receipt;
        }
        return true;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    Statement query;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT job_set_id, state, claimed_by_token, "
            "lease_expires_at_utc, started_at_utc, attempts, max_attempts,"
            "workset_id "
            "FROM exec_job WHERE job_id=?1;",
            -1,
            &query.st,
            nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(query.st, 1, job_id);
    const auto query_rc = sqlite3_step(query.st);
    if (query_rc == SQLITE_DONE) {
        receipt.disposition = ExecutionJobStartDisposition::Missing;
    } else if (query_rc != SQLITE_ROW) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    } else {
        const auto job_set_id = sqlite3_column_int64(query.st, 0);
        const auto* state_text = sqlite3_column_text(query.st, 1);
        const std::string state = state_text != nullptr
            ? reinterpret_cast<const char*>(state_text)
            : "";
        const auto* token_text = sqlite3_column_text(query.st, 2);
        const std::string token = token_text != nullptr
            ? reinterpret_cast<const char*>(token_text)
            : "";
        const auto lease_expiry =
            sqlite3_column_type(query.st, 3) != SQLITE_NULL
            ? sqlite3_column_int64(query.st, 3)
            : 0;
        const auto started_at =
            sqlite3_column_type(query.st, 4) != SQLITE_NULL
            ? std::optional<std::int64_t>(sqlite3_column_int64(query.st, 4))
            : std::nullopt;
        const auto attempts = sqlite3_column_int(query.st, 5);
        const auto max_attempts = sqlite3_column_int(query.st, 6);
        const bool workset_backed =
            sqlite3_column_type(query.st, 7) != SQLITE_NULL;
        const auto now_utc = CurrentUtcMs(db_);
        receipt.lease_expires_at_utc = lease_expiry;
        receipt.started_at_utc = started_at;
        if (attempts > 0) {
            receipt.durable_attempt_id =
                static_cast<std::uint64_t>(attempts);
        }

        if (workset_backed
            || (state != "CLAIMED" && state != "RUNNING")) {
            receipt.disposition = ExecutionJobStartDisposition::WrongState;
        } else if (token != claimed_by_token) {
            receipt.disposition = ExecutionJobStartDisposition::TokenMismatch;
        } else if (lease_expiry <= now_utc) {
            receipt.disposition = ExecutionJobStartDisposition::Expired;
        } else if (state == "RUNNING") {
            receipt.disposition =
                ExecutionJobStartDisposition::AlreadyRunning;
        } else if (attempts < 0 || attempts >= max_attempts) {
            receipt.disposition =
                ExecutionJobStartDisposition::WrongState;
        } else {
            const auto durable_attempt_id =
                static_cast<std::uint64_t>(attempts) + 1;
            Statement update;
            if (sqlite3_prepare_v2(
                    db_,
                    "UPDATE exec_job "
                    "SET state='RUNNING', started_at_utc=?1, "
                    "attempts=attempts+1 "
                    "WHERE job_id=?2 "
                    "  AND state='CLAIMED' "
                    "  AND claimed_by_token=?3 "
                    "  AND workset_id IS NULL "
                    "  AND COALESCE(lease_expires_at_utc,0)>?1 "
                    "  AND attempts=?4 "
                    "  AND attempts<max_attempts;",
                    -1,
                    &update.st,
                    nullptr)
                != SQLITE_OK) {
                Rollback(db_);
                if (error_out) *error_out = sqlite3_errmsg(db_);
                return false;
            }
            sqlite3_bind_int64(update.st, 1, now_utc);
            sqlite3_bind_int64(update.st, 2, job_id);
            const auto token = std::string(claimed_by_token);
            sqlite3_bind_text(
                update.st,
                3,
                token.c_str(),
                -1,
                SQLITE_TRANSIENT);
            sqlite3_bind_int(update.st, 4, attempts);
            if (sqlite3_step(update.st) != SQLITE_DONE
                || sqlite3_changes(db_) != 1) {
                Rollback(db_);
                if (error_out) {
                    *error_out = "execution-job start transition lost authority";
                }
                return false;
            }
            if (!InsertJobActionEventAndOutbox(
                    db_,
                    job_id,
                    job_set_id,
                    "Execution.JobStarted.v1",
                    "START",
                    error_out)) {
                Rollback(db_);
                return false;
            }
            receipt.disposition = ExecutionJobStartDisposition::Started;
            receipt.started_at_utc = now_utc;
            receipt.durable_attempt_id = durable_attempt_id;
        }
    }

    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    if (receipt_out != nullptr) {
        *receipt_out = receipt;
    }
    return true;
}

bool SqliteExecutionDb::ConfirmExecutionJobTerminalAuthority(
    std::int64_t job_id,
    std::string_view claimed_by_token,
    std::uint64_t durable_attempt_id,
    std::int64_t lease_duration_ms,
    ExecutionJobTerminalAuthorityReceipt* receipt_out,
    std::string* error_out) {
    ExecutionJobTerminalAuthorityReceipt receipt{
        .job_id = job_id,
    };
    if (receipt_out != nullptr) {
        *receipt_out = receipt;
    }
    if (error_out != nullptr) {
        error_out->clear();
    }
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (job_id <= 0 || claimed_by_token.empty()
        || durable_attempt_id == 0 || lease_duration_ms <= 0) {
        receipt.disposition =
            ExecutionJobTerminalAuthorityDisposition::InvalidRequest;
        if (receipt_out != nullptr) {
            *receipt_out = receipt;
        }
        return true;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    Statement query;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT state, claimed_by_token, lease_expires_at_utc,attempts,"
            "workset_id "
            "FROM exec_job WHERE job_id=?1;",
            -1,
            &query.st,
            nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(query.st, 1, job_id);
    const auto rc = sqlite3_step(query.st);
    if (rc == SQLITE_DONE) {
        receipt.disposition =
            ExecutionJobTerminalAuthorityDisposition::Missing;
    } else if (rc != SQLITE_ROW) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    } else {
        const auto* state_text = sqlite3_column_text(query.st, 0);
        const std::string state = state_text != nullptr
            ? reinterpret_cast<const char*>(state_text)
            : "";
        const auto* token_text = sqlite3_column_text(query.st, 1);
        const std::string token = token_text != nullptr
            ? reinterpret_cast<const char*>(token_text)
            : "";
        const auto lease_expiry =
            sqlite3_column_type(query.st, 2) != SQLITE_NULL
            ? sqlite3_column_int64(query.st, 2)
            : 0;
        const auto attempts = sqlite3_column_int(query.st, 3);
        const bool workset_backed =
            sqlite3_column_type(query.st, 4) != SQLITE_NULL;
        const auto now = CurrentUtcMs(db_);
        receipt.lease_expires_at_utc = lease_expiry;
        if (attempts > 0) {
            receipt.durable_attempt_id =
                static_cast<std::uint64_t>(attempts);
        }

        if (workset_backed || state != "RUNNING") {
            receipt.disposition =
                ExecutionJobTerminalAuthorityDisposition::WrongState;
        } else if (token != claimed_by_token) {
            receipt.disposition =
                ExecutionJobTerminalAuthorityDisposition::TokenMismatch;
        } else if (attempts <= 0
            || static_cast<std::uint64_t>(attempts)
                != durable_attempt_id) {
            receipt.disposition =
                ExecutionJobTerminalAuthorityDisposition::AttemptMismatch;
        } else if (lease_expiry <= now) {
            receipt.disposition =
                ExecutionJobTerminalAuthorityDisposition::Expired;
        } else {
            const auto renewed_expiry = now + lease_duration_ms;
            Statement update;
            if (sqlite3_prepare_v2(
                    db_,
                    "UPDATE exec_job SET lease_expires_at_utc=?1 "
                    "WHERE job_id=?2 AND state='RUNNING' "
                    "AND claimed_by_token=?3 AND attempts=?4 "
                    "AND workset_id IS NULL "
                    "AND COALESCE(lease_expires_at_utc,0)>?5;",
                    -1,
                    &update.st,
                    nullptr)
                != SQLITE_OK) {
                Rollback(db_);
                if (error_out) *error_out = sqlite3_errmsg(db_);
                return false;
            }
            const auto token_value = std::string(claimed_by_token);
            sqlite3_bind_int64(update.st, 1, renewed_expiry);
            sqlite3_bind_int64(update.st, 2, job_id);
            sqlite3_bind_text(
                update.st,
                3,
                token_value.c_str(),
                -1,
                SQLITE_TRANSIENT);
            sqlite3_bind_int64(
                update.st,
                4,
                static_cast<sqlite3_int64>(durable_attempt_id));
            sqlite3_bind_int64(update.st, 5, now);
            if (sqlite3_step(update.st) != SQLITE_DONE
                || sqlite3_changes(db_) != 1) {
                Rollback(db_);
                if (error_out) {
                    *error_out =
                        "terminal authority changed during confirmation";
                }
                return false;
            }
            receipt.disposition =
                ExecutionJobTerminalAuthorityDisposition::Valid;
            receipt.lease_expires_at_utc = renewed_expiry;
        }
    }

    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    if (receipt_out != nullptr) {
        *receipt_out = receipt;
    }
    return true;
}

bool SqliteExecutionDb::RecoverExecutionJobAfterWorkerLoss(
    std::int64_t job_id,
    std::string_view claimed_by_token,
    std::uint64_t durable_attempt_id,
    std::string_view message,
    ExecutionJobWorkerLossRecoveryReceipt* receipt_out,
    std::string* error_out) {
    ExecutionJobWorkerLossRecoveryReceipt receipt{
        .job_id = job_id,
    };
    if (receipt_out != nullptr) {
        *receipt_out = receipt;
    }
    if (error_out != nullptr) {
        error_out->clear();
    }
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (job_id <= 0 || claimed_by_token.empty()
        || durable_attempt_id == 0) {
        receipt.disposition =
            ExecutionJobWorkerLossRecoveryDisposition::InvalidRequest;
        if (receipt_out != nullptr) {
            *receipt_out = receipt;
        }
        return true;
    }
    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    std::int64_t job_set_id = 0;
    std::string state;
    std::string token;
    int attempts = 0;
    int max_attempts = 0;
    Statement query;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT job_set_id, state, claimed_by_token, attempts, "
            "max_attempts,workset_id "
            "FROM exec_job WHERE job_id=?1;",
            -1,
            &query.st,
            nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(query.st, 1, job_id);
    const auto rc = sqlite3_step(query.st);
    if (rc == SQLITE_DONE) {
        receipt.disposition =
            ExecutionJobWorkerLossRecoveryDisposition::Missing;
    } else if (rc != SQLITE_ROW) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    } else {
        job_set_id = sqlite3_column_int64(query.st, 0);
        const auto* state_text = sqlite3_column_text(query.st, 1);
        state = state_text != nullptr
            ? reinterpret_cast<const char*>(state_text)
            : "";
        const auto* token_text = sqlite3_column_text(query.st, 2);
        token = token_text != nullptr
            ? reinterpret_cast<const char*>(token_text)
            : "";
        attempts = sqlite3_column_int(query.st, 3);
        max_attempts = sqlite3_column_int(query.st, 4);
        const bool workset_backed =
            sqlite3_column_type(query.st, 5) != SQLITE_NULL;
        receipt.durable_state = state;
        if (attempts > 0) {
            receipt.durable_attempt_id =
                static_cast<std::uint64_t>(attempts);
        }

        if (workset_backed) {
            receipt.disposition =
                ExecutionJobWorkerLossRecoveryDisposition::WrongState;
        } else if (state != "CLAIMED" && state != "RUNNING") {
            receipt.disposition =
                ExecutionJobWorkerLossRecoveryDisposition::AlreadyDurable;
        } else if (token != claimed_by_token) {
            receipt.disposition =
                ExecutionJobWorkerLossRecoveryDisposition::TokenMismatch;
        } else {
            const auto expected_attempt =
                state == "RUNNING"
                ? (attempts > 0
                    ? static_cast<std::uint64_t>(attempts)
                    : 0)
                : (attempts >= 0
                    ? static_cast<std::uint64_t>(attempts) + 1
                    : 0);
            if (expected_attempt == 0
                || expected_attempt != durable_attempt_id) {
                receipt.disposition =
                    ExecutionJobWorkerLossRecoveryDisposition::
                        AttemptMismatch;
            } else {
                const bool attempts_exhausted =
                    state == "RUNNING"
                    && max_attempts > 0
                    && attempts >= max_attempts;
                Statement update;
                const char* update_sql = attempts_exhausted
                    ? "UPDATE exec_job SET state='FAILED', "
                      "claimed_by_token=NULL, lease_expires_at_utc=NULL, "
                      "ended_at_utc=?1, error_code='WORKER_LOSS', "
                      "error_text=?2 WHERE job_id=?3 AND state='RUNNING' "
                      "AND claimed_by_token=?4 AND attempts=?5 "
                      "AND workset_id IS NULL;"
                    : "UPDATE exec_job SET state='QUEUED', "
                      "claimed_by_token=NULL, lease_expires_at_utc=NULL, "
                      "started_at_utc=NULL, ended_at_utc=NULL, "
                      "error_code=NULL, error_text=NULL "
                      "WHERE job_id=?1 AND state=?2 "
                      "AND claimed_by_token=?3 AND attempts=?4 "
                      "AND workset_id IS NULL;";
                if (sqlite3_prepare_v2(
                        db_,
                        update_sql,
                        -1,
                        &update.st,
                        nullptr)
                    != SQLITE_OK) {
                    Rollback(db_);
                    if (error_out) *error_out = sqlite3_errmsg(db_);
                    return false;
                }
                const auto now = CurrentUtcMs(db_);
                const auto token_value = std::string(claimed_by_token);
                const auto message_value = std::string(
                    message.empty()
                    ? std::string_view("WORKER_LOSS")
                    : message);
                if (attempts_exhausted) {
                    sqlite3_bind_int64(update.st, 1, now);
                    sqlite3_bind_text(
                        update.st,
                        2,
                        message_value.c_str(),
                        -1,
                        SQLITE_TRANSIENT);
                    sqlite3_bind_int64(update.st, 3, job_id);
                    sqlite3_bind_text(
                        update.st,
                        4,
                        token_value.c_str(),
                        -1,
                        SQLITE_TRANSIENT);
                    sqlite3_bind_int(update.st, 5, attempts);
                } else {
                    sqlite3_bind_int64(update.st, 1, job_id);
                    sqlite3_bind_text(
                        update.st,
                        2,
                        state.c_str(),
                        -1,
                        SQLITE_TRANSIENT);
                    sqlite3_bind_text(
                        update.st,
                        3,
                        token_value.c_str(),
                        -1,
                        SQLITE_TRANSIENT);
                    sqlite3_bind_int(update.st, 4, attempts);
                }
                if (sqlite3_step(update.st) != SQLITE_DONE
                    || sqlite3_changes(db_) != 1) {
                    Rollback(db_);
                    if (error_out) {
                        *error_out =
                            "worker-loss recovery lost exact authority";
                    }
                    return false;
                }
                if (!InsertJobActionEventAndOutbox(
                        db_,
                        job_id,
                        job_set_id,
                        attempts_exhausted
                            ? "Execution.JobCompleted.v1"
                            : "Execution.JobQueued.v1",
                        message_value.c_str(),
                        error_out)) {
                    Rollback(db_);
                    return false;
                }
                receipt.disposition = attempts_exhausted
                    ? ExecutionJobWorkerLossRecoveryDisposition::
                        AttemptsExhaustedFailed
                    : ExecutionJobWorkerLossRecoveryDisposition::Requeued;
                receipt.durable_state =
                    attempts_exhausted ? "FAILED" : "QUEUED";
            }
        }
    }

    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    if (receipt_out != nullptr) {
        *receipt_out = receipt;
    }
    return true;
}

bool SqliteExecutionDb::RequeueExpiredExecutionLeases(
    int* rows_requeued_out,
    std::string* error_out) {
    if (rows_requeued_out) *rows_requeued_out = 0;
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(db_,
        "UPDATE exec_job "
            "SET state='QUEUED', claimed_by_token=NULL, lease_expires_at_utc=NULL, started_at_utc=NULL "
            "WHERE state IN ('QUEUED','CLAIMED','RUNNING') "
            "AND workset_id IS NULL "
            "AND claimed_by_token IS NOT NULL "
            "AND claimed_by_token<>'' "
            "AND COALESCE(lease_expires_at_utc, 0) <= ?1;",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, CurrentUtcMs(db_));
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (rows_requeued_out) *rows_requeued_out = sqlite3_changes(db_);
    return true;
}

bool SqliteExecutionDb::RequeueExpiredClaimedExecutionJobs(
    int* rows_requeued_out,
    std::string* error_out) {
    if (rows_requeued_out) *rows_requeued_out = 0;
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }

    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    std::vector<std::pair<std::int64_t, std::int64_t>> jobs;
    const auto now = CurrentUtcMs(db_);
    {
        Statement select;
        if (sqlite3_prepare_v2(db_,
            "SELECT job_id, job_set_id "
            "FROM exec_job "
            "WHERE state='CLAIMED' "
            "  AND workset_id IS NULL "
            "  AND claimed_by_token IS NOT NULL "
            "  AND claimed_by_token<>'' "
            "  AND COALESCE(lease_expires_at_utc, 0) <= ?1 "
            "ORDER BY job_id ASC;",
            -1,
            &select.st,
            nullptr)
            != SQLITE_OK) {
            Rollback(db_);
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(select.st, 1, now);
        while (true) {
            const auto rc = sqlite3_step(select.st);
            if (rc == SQLITE_ROW) {
                jobs.emplace_back(sqlite3_column_int64(select.st, 0), sqlite3_column_int64(select.st, 1));
                continue;
            }
            if (rc == SQLITE_DONE) {
                break;
            }
            Rollback(db_);
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
    }

    Statement update;
    if (sqlite3_prepare_v2(db_,
        "UPDATE exec_job "
            "SET state='QUEUED', claimed_by_token=NULL, lease_expires_at_utc=NULL, "
            "started_at_utc=NULL, ended_at_utc=NULL, error_code=NULL, error_text=NULL "
            "WHERE state='CLAIMED' "
            "  AND workset_id IS NULL "
            "  AND claimed_by_token IS NOT NULL "
            "  AND claimed_by_token<>'' "
            "  AND COALESCE(lease_expires_at_utc, 0) <= ?1;",
        -1,
        &update.st,
        nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(update.st, 1, now);
    if (sqlite3_step(update.st) != SQLITE_DONE) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const int changed = sqlite3_changes(db_);

    for (const auto& [job_id, job_set_id] : jobs) {
        if (!InsertJobActionEventAndOutbox(
                db_,
                job_id,
                job_set_id,
                "Execution.JobQueued.v1",
                "EXPIRED_CLAIM_RECOVERY",
                error_out)) {
            Rollback(db_);
            return false;
        }
    }

    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    if (rows_requeued_out) *rows_requeued_out = changed;
    return true;
}

bool SqliteExecutionDb::RequeueClaimedExecutionJob(
    std::int64_t job_id,
    std::string_view claimed_by_token,
    std::string_view message,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (job_id <= 0) {
        if (error_out) *error_out = "job_id must be > 0";
        return false;
    }
    if (claimed_by_token.empty()) {
        if (error_out) *error_out = "claimed_by_token is required";
        return false;
    }

    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    std::int64_t job_set_id = 0;
    {
        Statement select;
        if (sqlite3_prepare_v2(db_,
            "SELECT job_set_id FROM exec_job "
            "WHERE job_id=?1 AND state IN ('CLAIMED','RUNNING') "
            "AND workset_id IS NULL "
            "AND claimed_by_token=?2;",
            -1,
            &select.st,
            nullptr)
            != SQLITE_OK) {
            Rollback(db_);
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        const auto token = std::string(claimed_by_token);
        sqlite3_bind_int64(select.st, 1, job_id);
        sqlite3_bind_text(select.st, 2, token.c_str(), -1, SQLITE_TRANSIENT);
        const auto rc = sqlite3_step(select.st);
        if (rc == SQLITE_ROW) {
            job_set_id = sqlite3_column_int64(select.st, 0);
        } else if (rc == SQLITE_DONE) {
            Rollback(db_);
            if (error_out) *error_out = "claim-owned job not found";
            return false;
        } else {
            Rollback(db_);
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
    }

    Statement update;
    if (sqlite3_prepare_v2(db_,
        "UPDATE exec_job "
            "SET state='QUEUED', claimed_by_token=NULL, lease_expires_at_utc=NULL, "
            "started_at_utc=NULL, ended_at_utc=NULL, error_code=NULL, error_text=NULL "
            "WHERE job_id=?1 AND state IN ('CLAIMED','RUNNING') "
            "AND workset_id IS NULL "
            "AND claimed_by_token=?2;",
        -1,
        &update.st,
        nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto token = std::string(claimed_by_token);
    sqlite3_bind_int64(update.st, 1, job_id);
    sqlite3_bind_text(update.st, 2, token.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(update.st) != SQLITE_DONE) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        Rollback(db_);
        if (error_out) *error_out = "claim-owned job not found";
        return false;
    }

    const auto message_value = std::string(message.empty() ? std::string_view("CLAIM_REQUEUED") : message);
    if (!InsertJobActionEventAndOutbox(
            db_,
            job_id,
            job_set_id,
            "Execution.JobQueued.v1",
            message_value.c_str(),
            error_out)) {
        Rollback(db_);
        return false;
    }

    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    if (error_out) error_out->clear();
    return true;
}

bool SqliteExecutionDb::RequeueInterruptedExecutionJobs(
    int* rows_requeued_out,
    std::string* error_out) {
    if (rows_requeued_out) *rows_requeued_out = 0;
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }

    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    std::vector<std::pair<std::int64_t, std::int64_t>> jobs;
    {
        Statement select;
        if (sqlite3_prepare_v2(db_,
            "SELECT job_id, job_set_id "
            "FROM exec_job "
            "WHERE workset_id IS NULL "
            "  AND (state IN ('CLAIMED','RUNNING') "
            "   OR (state IN ('QUEUED','INTERRUPTED') "
            "       AND claimed_by_token IS NOT NULL "
            "       AND claimed_by_token<>'')) "
            "ORDER BY job_id ASC;",
            -1,
            &select.st,
            nullptr)
            != SQLITE_OK) {
            Rollback(db_);
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }

        while (true) {
            const auto rc = sqlite3_step(select.st);
            if (rc == SQLITE_ROW) {
                jobs.emplace_back(sqlite3_column_int64(select.st, 0), sqlite3_column_int64(select.st, 1));
                continue;
            }
            if (rc == SQLITE_DONE) {
                break;
            }
            Rollback(db_);
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
    }

    Statement update;
    if (sqlite3_prepare_v2(db_,
            "UPDATE exec_job "
            "SET state='QUEUED', claimed_by_token=NULL, lease_expires_at_utc=NULL, "
            "started_at_utc=NULL, ended_at_utc=NULL, error_code=NULL, error_text=NULL "
            "WHERE workset_id IS NULL "
            "  AND (state IN ('CLAIMED','RUNNING') "
            "   OR (state IN ('QUEUED','INTERRUPTED') "
            "       AND claimed_by_token IS NOT NULL "
            "       AND claimed_by_token<>''));",
        -1,
        &update.st,
        nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_step(update.st) != SQLITE_DONE) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const int changed = sqlite3_changes(db_);

    Statement terminal_cleanup;
    if (sqlite3_prepare_v2(db_,
        "UPDATE exec_job "
            "SET claimed_by_token=NULL, lease_expires_at_utc=NULL "
            "WHERE state NOT IN ('QUEUED','CLAIMED','RUNNING','INTERRUPTED') "
            "  AND workset_id IS NULL "
            "  AND (claimed_by_token IS NOT NULL OR lease_expires_at_utc IS NOT NULL);",
        -1,
        &terminal_cleanup.st,
        nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_step(terminal_cleanup.st) != SQLITE_DONE) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    for (const auto& [job_id, job_set_id] : jobs) {
        if (!InsertJobActionEventAndOutbox(
                db_,
                job_id,
                job_set_id,
                "Execution.JobQueued.v1",
                "STARTUP_RECOVERY",
                error_out)) {
            Rollback(db_);
            return false;
        }
    }

    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    if (rows_requeued_out) *rows_requeued_out = changed;
    return true;
}

bool SqliteExecutionDb::MarkQueuedJobsSuperseded(
    std::int64_t job_set_id,
    std::int64_t except_job_id,
    std::string* error_out,
    int* rows_superseded_out) {
    if (rows_superseded_out != nullptr) {
        *rows_superseded_out = 0;
    }
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (job_set_id <= 0) {
        if (error_out) *error_out = "job_set_id must be > 0";
        return false;
    }

    if (!ExecuteSql(db_, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    std::vector<std::int64_t> job_ids;
    {
        Statement select;
        if (sqlite3_prepare_v2(db_,
            "SELECT job_id FROM exec_job "
            "WHERE job_set_id=?1 AND state='QUEUED' AND job_id<>?2 "
            "AND workset_id IS NULL "
            "ORDER BY job_id ASC;",
            -1,
            &select.st,
            nullptr)
            != SQLITE_OK) {
            Rollback(db_);
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(select.st, 1, job_set_id);
        sqlite3_bind_int64(select.st, 2, except_job_id);
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(select.st)) == SQLITE_ROW) {
            job_ids.push_back(sqlite3_column_int64(select.st, 0));
        }
        if (rc != SQLITE_DONE) {
            Rollback(db_);
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
    }

    int changed = 0;
    for (const auto job_id : job_ids) {
        Statement update;
        if (sqlite3_prepare_v2(db_,
            "UPDATE exec_job "
            "SET state='SUPERSEDED', ended_at_utc=CAST(unixepoch('now') * 1000 AS INTEGER) "
            "WHERE job_id=?1 AND job_set_id=?2 AND state='QUEUED' "
            "AND workset_id IS NULL;",
            -1,
            &update.st,
            nullptr)
            != SQLITE_OK) {
            Rollback(db_);
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(update.st, 1, job_id);
        sqlite3_bind_int64(update.st, 2, job_set_id);
        if (sqlite3_step(update.st) != SQLITE_DONE) {
            Rollback(db_);
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        if (sqlite3_changes(db_) == 0) {
            continue;
        }
        if (!InsertJobActionEventAndOutbox(db_, job_id, job_set_id, "Execution.JobCompleted.v1", "SUPERSEDE", error_out)) {
            Rollback(db_);
            return false;
        }
        ++changed;
    }

    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }
    if (rows_superseded_out != nullptr) {
        *rows_superseded_out = changed;
    }
    return true;
}

retention::OutboxRetentionPreview SqliteExecutionDb::PreviewOutboxRetention(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy) const {
    std::int64_t max_outbox_id = 0;
    Statement st;
    if (db_ != nullptr
        && sqlite3_prepare_v2(db_, "SELECT COALESCE(MAX(outbox_id), 0) FROM exec_outbox_message;", -1, &st.st, nullptr) == SQLITE_OK
        && sqlite3_step(st.st) == SQLITE_ROW) {
        max_outbox_id = sqlite3_column_int64(st.st, 0);
    }
    return retention::BuildOutboxRetentionPreview(max_outbox_id, subscriptions, now_utc, policy);
}

bool SqliteExecutionDb::PurgeOutboxThroughRetentionFloor(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy,
    int max_rows,
    int* rows_deleted_out,
    std::string* error_out) {
    if (rows_deleted_out) *rows_deleted_out = 0;
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (max_rows <= 0) {
        if (error_out) *error_out = "max_rows must be > 0";
        return false;
    }

    const auto preview = PreviewOutboxRetention(subscriptions, now_utc, policy);
    if (preview.IsPurgeBlocked()) {
        if (error_out) *error_out = "purge blocked by required paused/error subscriptions";
        return false;
    }
    if (!preview.safe_purge_floor_outbox_id.has_value()) {
        if (error_out) *error_out = "safe purge floor unavailable";
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(db_,
        "DELETE FROM exec_outbox_message "
            "WHERE outbox_id IN ("
            "  SELECT outbox_id FROM exec_outbox_message "
            "  WHERE published_at_utc IS NOT NULL AND outbox_id < ?1 "
            "  ORDER BY outbox_id ASC LIMIT ?2"
            ");",
        -1,
        &st.st,
        nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, preview.safe_purge_floor_outbox_id.value());
    sqlite3_bind_int(st.st, 2, max_rows);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (rows_deleted_out) *rows_deleted_out = sqlite3_changes(db_);
    return true;
}

bool SqliteExecutionDb::PurgeWorkflowHandlerDedupeOlderThan(
    std::int64_t last_seen_at_utc_exclusive,
    int max_rows,
    int* rows_deleted_out,
    std::string* error_out) {
    if (rows_deleted_out) *rows_deleted_out = 0;
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
    if (sqlite3_prepare_v2(db_,
            "DELETE FROM exec_handler_dedupe WHERE dedupe_id IN ("
            "SELECT dedupe_id FROM exec_handler_dedupe "
            "WHERE last_seen_at_utc < ?1 "
            "ORDER BY last_seen_at_utc ASC, dedupe_id ASC "
            "LIMIT ?2);",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, last_seen_at_utc_exclusive);
    sqlite3_bind_int(st.st, 2, max_rows);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (rows_deleted_out) *rows_deleted_out = sqlite3_changes(db_);
    return true;
}

std::optional<events::ExecutionWorkflowJobPayloadView> SqliteExecutionDb::ResolveExecutionWorkflowJobPayload(
    const events::EventEnvelope& envelope) const {
    if (!events::ValidateExecutionWorkflowJobPayloadV1(envelope)) {
        return std::nullopt;
    }
    return ResolveExecutionWorkflowJobPayload(
        envelope.event_type,
        envelope.event_version,
        envelope.payload_ref_kind,
        envelope.payload_ref_id);
}

std::optional<events::ExecutionWorkflowJobPayloadView> SqliteExecutionDb::ResolveExecutionWorkflowJobPayload(
    std::string_view event_type,
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    const auto contract = events::ResolvePayloadResolverContract(event_type, event_version);
    if (!contract.has_value() || contract.value() != events::PayloadResolverContract::ExecutionWorkflowJobV1) {
        return std::nullopt;
    }
    if (payload_ref_id <= 0 || db_ == nullptr) {
        return std::nullopt;
    }

    if (payload_ref_kind == "workflow_event") {
        return ResolveWorkflowEventPayload(db_, payload_ref_id);
    }
    if (payload_ref_kind == "job_set") {
        return ResolveJobSetPayload(db_, payload_ref_id);
    }
    if (payload_ref_kind == "workset") {
        return ResolveWorksetPayload(db_, payload_ref_id);
    }
    if (payload_ref_kind == "job") {
        return ResolveJobPayload(db_, payload_ref_id);
    }

    return std::nullopt;
}

} // namespace savor::db::execution::workflow
