#include "JobEventOrchestration.h"

#include <sstream>

#include <sqlite3.h>

namespace simcore::db::execution::jobs {
namespace {

struct Statement {
    sqlite3_stmt* st = nullptr;
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }
};

bool Prepare(sqlite3* db, const char* sql, Statement* out, std::string* error_out) {
    if (sqlite3_prepare_v2(db, sql, -1, &out->st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

bool StepDone(sqlite3* db, sqlite3_stmt* st, std::string* error_out) {
    if (sqlite3_step(st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

std::int64_t NowUtc(sqlite3* db) {
    Statement st;
    if (sqlite3_prepare_v2(db, "SELECT CAST(unixepoch('now') * 1000 AS INTEGER);", -1, &st.st, nullptr) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return 0;
    }
    return sqlite3_column_int64(st.st, 0);
}

bool TryGetJobSetId(sqlite3* db, std::int64_t job_id, std::int64_t* job_set_id, std::string* error_out) {
    Statement st;
    if (!Prepare(db, "SELECT job_set_id FROM exec_job WHERE job_id=?1;", &st, error_out)) {
        return false;
    }

    sqlite3_bind_int64(st.st, 1, job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        if (error_out) *error_out = "job not found";
        return false;
    }

    *job_set_id = sqlite3_column_int64(st.st, 0);
    return true;
}

bool UpdateJobLifecycleColumns(sqlite3* db, const JobLifecycleEventCommand& command, std::string* error_out) {
    if (command.kind == JobLifecycleEventKind::JobSetCreated || command.job_id <= 0) {
        return true;
    }

    const auto now = NowUtc(db);
    const char* update_sql =
        "UPDATE exec_job "
        "SET state=(CASE ?2 WHEN '' THEN state ELSE ?2 END), "
        "started_at_utc=(CASE WHEN ?3 IS NULL THEN started_at_utc ELSE ?3 END), "
        "ended_at_utc=(CASE WHEN ?4 IS NULL THEN ended_at_utc ELSE ?4 END), "
        "claimed_by_token=(CASE WHEN ?5 IS NULL THEN claimed_by_token ELSE ?5 END), "
        "lease_expires_at_utc=(CASE WHEN ?6 IS NULL THEN lease_expires_at_utc ELSE ?6 END) "
        "WHERE job_id=?1;";

    const char* state = "";
    std::optional<std::int64_t> started;
    std::optional<std::int64_t> ended;

    switch (command.kind) {
    case JobLifecycleEventKind::JobQueued:
        state = "QUEUED";
        break;
    case JobLifecycleEventKind::JobClaimed:
        state = "RUNNING";
        started = now;
        break;
    case JobLifecycleEventKind::JobLeaseRenewed:
        break;
    case JobLifecycleEventKind::JobProgressed:
        state = "RUNNING";
        break;
    case JobLifecycleEventKind::JobCompleted:
        if (!command.terminal_state.has_value() || command.terminal_state->empty()) {
            if (error_out) *error_out = "terminal_state must be set for JobCompleted";
            return false;
        }
        state = command.terminal_state->c_str();
        ended = now;
        break;
    case JobLifecycleEventKind::JobEventArchived:
    case JobLifecycleEventKind::JobRestored:
    case JobLifecycleEventKind::JobSetCreated:
        break;
    }

    Statement st;
    if (!Prepare(db, update_sql, &st, error_out)) {
        return false;
    }

    sqlite3_bind_int64(st.st, 1, command.job_id);
    sqlite3_bind_text(st.st, 2, state, -1, SQLITE_STATIC);
    if (started.has_value()) {
        sqlite3_bind_int64(st.st, 3, *started);
    } else {
        sqlite3_bind_null(st.st, 3);
    }

    if (ended.has_value()) {
        sqlite3_bind_int64(st.st, 4, *ended);
    } else {
        sqlite3_bind_null(st.st, 4);
    }

    if (command.claimed_by_token.has_value()) {
        sqlite3_bind_text(st.st, 5, command.claimed_by_token->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(st.st, 5);
    }

    if (command.lease_expires_at_utc.has_value()) {
        sqlite3_bind_int64(st.st, 6, *command.lease_expires_at_utc);
    } else {
        sqlite3_bind_null(st.st, 6);
    }

    if (!StepDone(db, st.st, error_out)) {
        return false;
    }

    if (sqlite3_changes(db) == 0) {
        if (error_out) *error_out = "job not found";
        return false;
    }

    return true;
}

} // namespace

const char* ToEventType(JobLifecycleEventKind kind) {
    switch (kind) {
    case JobLifecycleEventKind::JobSetCreated: return "Execution.JobSetCreated.v1";
    case JobLifecycleEventKind::JobQueued: return "Execution.JobQueued.v1";
    case JobLifecycleEventKind::JobClaimed: return "Execution.JobClaimed.v1";
    case JobLifecycleEventKind::JobLeaseRenewed: return "Execution.JobLeaseRenewed.v1";
    case JobLifecycleEventKind::JobProgressed: return "Execution.JobProgressed.v1";
    case JobLifecycleEventKind::JobCompleted: return "Execution.JobCompleted.v1";
    case JobLifecycleEventKind::JobEventArchived: return "Execution.JobEventArchived.v1";
    case JobLifecycleEventKind::JobRestored: return "Execution.JobRestored.v1";
    }
    return "";
}

SqliteJobEventCommandService::SqliteJobEventCommandService(sqlite3* sqlite_db)
    : db_(sqlite_db) {
}

bool SqliteJobEventCommandService::AppendLifecycleEvent(const JobLifecycleEventCommand& command, std::string* error_out) {
    auto* db = db_;
    if (db == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }

    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }

    const auto rollback = [&]() {
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    };

    std::int64_t job_set_id = command.job_set_id;
    if (command.kind == JobLifecycleEventKind::JobSetCreated) {
        if (job_set_id <= 0) {
            rollback();
            if (error_out) *error_out = "job_set_id must be > 0";
            return false;
        }
    } else {
        if (command.job_id <= 0) {
            rollback();
            if (error_out) *error_out = "job_id must be > 0";
            return false;
        }
        if (!TryGetJobSetId(db, command.job_id, &job_set_id, error_out)) {
            rollback();
            return false;
        }
    }

    if (!UpdateJobLifecycleColumns(db, command, error_out)) {
        rollback();
        return false;
    }

    std::int64_t payload_ref_id = job_set_id;
    const char* payload_ref_kind = "job_set";
    const auto ts = NowUtc(db);

    if (command.kind != JobLifecycleEventKind::JobSetCreated) {
        payload_ref_kind = "job";
        payload_ref_id = command.job_id;
    }

    std::ostringstream event_id;
    event_id << "execution-" << ToEventType(command.kind) << "-" << payload_ref_id;
    const auto event_id_value = event_id.str();
    const auto aggregate_kind = command.kind == JobLifecycleEventKind::JobSetCreated ? "job_set" : "job";
    const auto aggregate_id = std::to_string(command.kind == JobLifecycleEventKind::JobSetCreated ? job_set_id : command.job_id);
    const auto correlation_id = "job-set-" + std::to_string(job_set_id);
    std::string causation_id = command.causation_id.value_or(std::string("job-") + aggregate_id);

    Statement outbox;
    if (!Prepare(db,
        "INSERT INTO exec_outbox_message("
        "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
        "VALUES(?1,?2,1,'Execution',?3,?4,?5,?6,?7,?8,?9);",
        &outbox,
        error_out)) {
        rollback();
        return false;
    }

    sqlite3_bind_text(outbox.st, 1, event_id_value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outbox.st, 2, ToEventType(command.kind), -1, SQLITE_STATIC);
    sqlite3_bind_text(outbox.st, 3, aggregate_kind, -1, SQLITE_STATIC);
    sqlite3_bind_text(outbox.st, 4, aggregate_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outbox.st, 5, correlation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(outbox.st, 6, causation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(outbox.st, 7, ts);
    sqlite3_bind_text(outbox.st, 8, payload_ref_kind, -1, SQLITE_STATIC);
    sqlite3_bind_int64(outbox.st, 9, payload_ref_id);

    if (!StepDone(db, outbox.st, error_out)) {
        rollback();
        return false;
    }

    if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        rollback();
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }

    return true;
}

} // namespace simcore::db::execution::jobs
