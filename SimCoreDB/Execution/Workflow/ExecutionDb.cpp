#include "ExecutionDb.h"

#include "../../Common/Events/EventPayloadDispatch.h"
#include "../../Common/Events/EventPayloadValidation.h"
#include "SqliteWorkflowOrchestration.h"

namespace simcore::db::execution::workflow {
namespace {

struct Statement {
    sqlite3_stmt* st = nullptr;
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }
};

bool Prepare(sqlite3* db, const char* sql, Statement* st) {
    return sqlite3_prepare_v2(db, sql, -1, &st->st, nullptr) == SQLITE_OK;
}

std::int64_t CurrentUtcMs(sqlite3* db) {
    Statement st;
    if (!Prepare(db, "SELECT CAST(unixepoch('now') * 1000 AS INTEGER);", &st)) {
        return 0;
    }
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return 0;
    }
    return sqlite3_column_int64(st.st, 0);
bool Exec(sqlite3* db, const char* sql) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

std::int64_t NowUtcMillis() {
    return types::UtcNow().time_since_epoch().count();
}

std::optional<events::ExecutionWorkflowJobPayloadView> ResolveWorkflowEventPayload(sqlite3* db, std::int64_t payload_ref_id) {
    Statement st;
    if (!Prepare(db,
        "SELECT e.workflow_instance_id, COALESCE(e.workflow_step_id, 0), "
        "COALESCE(s.job_set_id, 0) "
        "FROM exec_workflow_event e "
        "LEFT JOIN exec_workflow_step s ON s.workflow_step_id=e.workflow_step_id "
        "WHERE e.workflow_event_id=?1;",
        &st)) {
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

std::optional<events::ExecutionWorkflowJobPayloadView> ResolveWorkflowInputEventPayload(sqlite3* db, std::int64_t payload_ref_id) {
    Statement st;
    if (!Prepare(db,
        "SELECT e.workflow_instance_id, e.workflow_step_id, "
        "COALESCE(s.job_set_id, 0) "
        "FROM exec_workflow_input_event e "
        "LEFT JOIN exec_workflow_step s ON s.workflow_step_id=e.workflow_step_id "
        "WHERE e.workflow_input_event_id=?1;",
        &st)) {
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
    if (!Prepare(db, "SELECT job_set_id FROM exec_job_set WHERE job_set_id=?1;", &st)) {
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

std::optional<events::ExecutionWorkflowJobPayloadView> ResolveJobPayload(sqlite3* db, std::int64_t payload_ref_id) {
    Statement st;
    if (!Prepare(db,
        "SELECT job_id, job_set_id "
        "FROM exec_job "
        "WHERE job_id=?1;",
        &st)) {
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

} // namespace

ExecutionDb::ExecutionDb(sqlite3* db)
    : db_(db)
    , query_service_(std::make_unique<SqliteWorkflowOrchestrationQueryService>(db_))
    , command_service_(std::make_unique<SqliteWorkflowOrchestrationCommandService>(db_))
    , job_command_service_(std::make_unique<jobs::SqliteJobEventCommandService>(db_)) {
}

IWorkflowOrchestrationQueryService* ExecutionDb::WorkflowQueryService() {
    return query_service_.get();
}

IWorkflowOrchestrationCommandService* ExecutionDb::WorkflowCommandService() {
    return command_service_.get();
}

jobs::IJobEventCommandService* ExecutionDb::JobCommandService() {
    return job_command_service_.get();
}

std::optional<ExecutionJobRecord> ExecutionDb::GetJobRecord(std::int64_t job_id) const {
    if (db_ == nullptr || job_id <= 0) {
        return std::nullopt;
    }

    Statement st;
    if (!Prepare(db_,
        "SELECT job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, "
        "fingerprint, state "
        "FROM exec_job WHERE job_id=?1;",
        &st)) {
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
    row.fingerprint = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 6));
    row.state = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 7));
    return row;
}

bool ExecutionDb::CreateJobSet(
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

    if (!Exec(db_, "BEGIN IMMEDIATE;")) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto rollback = [&]() { (void)Exec(db_, "ROLLBACK;"); };

    const auto now = command.created_at_utc > 0 ? command.created_at_utc : NowUtcMillis();
    Statement insert_set;
    if (!Prepare(db_,
        "INSERT INTO exec_job_set(parent_job_set_id,program_kind,purpose,created_by,created_at_utc,priority_boost,expected_total,domain_ref_kind,domain_ref_id,meta_note) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);",
        &insert_set)) {
        rollback();
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (command.parent_job_set_id.has_value()) sqlite3_bind_int64(insert_set.st, 1, *command.parent_job_set_id); else sqlite3_bind_null(insert_set.st, 1);
    sqlite3_bind_int(insert_set.st, 2, command.program_kind);
    sqlite3_bind_text(insert_set.st, 3, command.purpose.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_set.st, 4, command.created_by.c_str(), -1, SQLITE_TRANSIENT);
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

    if (!Exec(db_, "COMMIT;")) {
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

bool ExecutionDb::EnqueueJob(
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

    Statement st;
    if (!Prepare(db_,
        "INSERT INTO exec_job(job_set_id,parent_job_id,program_kind,program_version,program_ref_kind,program_ref_id,fingerprint,priority,state,attempts,max_attempts,queued_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,'QUEUED',0,?9,?10);",
        &st)) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(st.st, 1, command.job_set_id);
    if (command.parent_job_id.has_value()) sqlite3_bind_int64(st.st, 2, *command.parent_job_id); else sqlite3_bind_null(st.st, 2);
    sqlite3_bind_int(st.st, 3, command.program_kind);
    sqlite3_bind_int(st.st, 4, command.program_version);
    sqlite3_bind_text(st.st, 5, command.program_ref_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 6, command.program_ref_id);
    sqlite3_bind_text(st.st, 7, command.fingerprint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st.st, 8, command.priority);
    sqlite3_bind_int(st.st, 9, command.max_attempts);
    sqlite3_bind_int64(st.st, 10, CurrentUtcMs(db_));

    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    if (job_id_out) *job_id_out = sqlite3_last_insert_rowid(db_);
    return true;
}

std::optional<ExecutionJobRecord> ExecutionDb::GetJob(std::int64_t job_id) const {
    if (db_ == nullptr || job_id <= 0) {
        return std::nullopt;
    }

    Statement st;
    if (!Prepare(db_,
        "SELECT job_id, job_set_id, program_ref_kind, program_ref_id "
        "FROM exec_job WHERE job_id=?1;",
        &st)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    ExecutionJobRecord row{};
    row.job_id = sqlite3_column_int64(st.st, 0);
    row.job_set_id = sqlite3_column_int64(st.st, 1);
    row.program_ref_kind = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 2));
    row.program_ref_id = sqlite3_column_int64(st.st, 3);
    return row;
}

retention::OutboxRetentionPreview ExecutionDb::PreviewOutboxRetention(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy) const {
    std::int64_t max_outbox_id = 0;
    Statement st;
    if (db_ != nullptr
        && Prepare(db_, "SELECT COALESCE(MAX(outbox_id), 0) FROM exec_outbox_message;", &st)
        && sqlite3_step(st.st) == SQLITE_ROW) {
        max_outbox_id = sqlite3_column_int64(st.st, 0);
    }
    return retention::BuildOutboxRetentionPreview(max_outbox_id, subscriptions, now_utc, policy);
}

bool ExecutionDb::PurgeOutboxThroughRetentionFloor(
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
    if (!Prepare(
            db_,
            "DELETE FROM exec_outbox_message "
            "WHERE outbox_id IN ("
            "  SELECT outbox_id FROM exec_outbox_message "
            "  WHERE published_at_utc IS NOT NULL AND outbox_id < ?1 "
            "  ORDER BY outbox_id ASC LIMIT ?2"
            ");",
            &st)) {
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

std::optional<events::ExecutionWorkflowJobPayloadView> ExecutionDb::ResolveExecutionWorkflowJobPayload(
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

std::optional<events::ExecutionWorkflowJobPayloadView> ExecutionDb::ResolveExecutionWorkflowJobPayload(
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
    if (payload_ref_kind == "workflow_input_event") {
        return ResolveWorkflowInputEventPayload(db_, payload_ref_id);
    }
    if (payload_ref_kind == "job_set") {
        return ResolveJobSetPayload(db_, payload_ref_id);
    }
    if (payload_ref_kind == "job") {
        return ResolveJobPayload(db_, payload_ref_id);
    }

    return std::nullopt;
}

} // namespace simcore::db::execution::workflow
