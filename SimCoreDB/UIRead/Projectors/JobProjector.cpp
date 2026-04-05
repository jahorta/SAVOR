#include "JobProjector.h"

#include "../../Common/Events/OutboxRelay.h"
#include "../SqliteUiReadDb.h"

namespace simcore::db::uiread::projectors {

JobProjector::JobProjector(sqlite3* db)
    : db_(db) {
}

std::int64_t JobProjector::GetCheckpoint(const std::string& projector_name, std::string* error_out) const {
    if (projector_name.empty()) {
        if (error_out) *error_out = "projector_name is required";
        return 0;
    }

    simcore::db::SqliteUiReadDb ui_read_db(db_);
    const auto checkpoint = ui_read_db.GetProjectionCheckpoint(projector_name);
    if (!checkpoint.has_value()) {
        return 0;
    }

    return checkpoint->last_outbox_id;
}

bool JobProjector::ProjectAll(std::string* error_out) {
    char* err = nullptr;
    constexpr const char* kSql =
        "BEGIN IMMEDIATE;"
        "INSERT INTO ui_job_summary(job_id,job_set_id,program_kind,state,priority,queued_at_utc,started_at_utc,ended_at_utc,error_code) "
        "SELECT job_id,job_set_id,program_kind,state,priority,queued_at_utc,started_at_utc,ended_at_utc,error_code FROM exec_job "
        "ON CONFLICT(job_id) DO UPDATE SET "
        "job_set_id=excluded.job_set_id,program_kind=excluded.program_kind,state=excluded.state,priority=excluded.priority,"
        "queued_at_utc=excluded.queued_at_utc,started_at_utc=excluded.started_at_utc,ended_at_utc=excluded.ended_at_utc,error_code=excluded.error_code;"
        "INSERT INTO ui_job_detail(job_id,attempts,max_attempts,fingerprint,claimed_by_token,lease_expires_at_utc,error_text) "
        "SELECT job_id,attempts,max_attempts,fingerprint,claimed_by_token,lease_expires_at_utc,error_text FROM exec_job "
        "ON CONFLICT(job_id) DO UPDATE SET "
        "attempts=excluded.attempts,max_attempts=excluded.max_attempts,fingerprint=excluded.fingerprint,"
        "claimed_by_token=excluded.claimed_by_token,lease_expires_at_utc=excluded.lease_expires_at_utc,error_text=excluded.error_text;"
        "DELETE FROM ui_job_artifact;"
        "INSERT INTO ui_job_artifact(ui_job_artifact_id,job_id,artifact_id,role_kind,created_at_utc) "
        "SELECT e.job_event_id,e.job_id,e.artifact_id,e.event_kind,e.event_ts_utc "
        "FROM exec_job_event e WHERE e.artifact_id IS NOT NULL "
        "ON CONFLICT(job_id,artifact_id,role_kind) DO UPDATE SET created_at_utc=excluded.created_at_utc;"
        "COMMIT;";
    if (sqlite3_exec(db_, kSql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (error_out) *error_out = err ? err : sqlite3_errmsg(db_);
        sqlite3_free(err);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    return true;
}

bool JobProjector::ProjectFromOutbox(const std::string& projector_name, int max_batch_size, std::string* error_out, int max_attempts) {
    if (projector_name.empty()) {
        if (error_out) *error_out = "projector_name is required";
        return false;
    }
    if (max_batch_size <= 0) {
        if (error_out) *error_out = "max_batch_size must be > 0";
        return false;
    }
    if (max_attempts <= 0) {
        if (error_out) *error_out = "max_attempts must be > 0";
        return false;
    }

    const auto checkpoint = GetCheckpoint(projector_name, error_out);

    events::OutboxRelay relay({
        .db = db_,
        .outbox_table = "exec_outbox_message",
        .context_name = "Execution",
        .aggregate_kind = "",
        .payload_ref_kind = "",
        .max_attempts = max_attempts,
    });

    const auto project_jobs = [this](const events::EventEnvelope&, std::string* handler_error) {
        return ProjectAll(handler_error);
    };

    const std::vector<events::OutboxRelayDispatchBinding> bindings{
        { { "Execution.JobSetCreated.v1", 1 }, project_jobs },
        { { "Execution.JobQueued.v1", 1 }, project_jobs },
        { { "Execution.JobClaimed.v1", 1 }, project_jobs },
        { { "Execution.JobLeaseRenewed.v1", 1 }, project_jobs },
        { { "Execution.JobProgressed.v1", 1 }, project_jobs },
        { { "Execution.JobCompleted.v1", 1 }, project_jobs },
        { { "Execution.JobEventArchived.v1", 1 }, project_jobs },
        { { "Execution.JobRestored.v1", 1 }, project_jobs },
        { { "Execution.WorkflowInstanceCreated.v1", 1 }, project_jobs },
        { { "Execution.WorkflowStepReady.v1", 1 }, project_jobs },
        { { "Execution.WorkflowStepMaterialized.v1", 1 }, project_jobs },
        { { "Execution.WorkflowStepCompleted.v1", 1 }, project_jobs },
        { { "Execution.WorkflowStepFailed.v1", 1 }, project_jobs },
        { { "Execution.WorkflowInstanceCompleted.v1", 1 }, project_jobs },
    };

    events::OutboxRelayResult relay_result{};
    if (!relay.RelayBatch(checkpoint, max_batch_size, bindings, &relay_result, error_out)) {
        return false;
    }

    if (relay_result.last_scanned_outbox_id > checkpoint) {
        simcore::db::SqliteUiReadDb ui_read_db(db_);
        if (!ui_read_db.UpsertProjectionCheckpoint({
            projector_name,
            std::string{},
            relay_result.last_scanned_outbox_id,
            simcore::db::types::UtcNow(),
        })) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
    }

    return true;
}

} // namespace simcore::db::uiread::projectors
