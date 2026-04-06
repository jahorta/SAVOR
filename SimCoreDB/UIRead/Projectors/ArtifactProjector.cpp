#include "ArtifactProjector.h"

#include "ProjectorContract.h"

namespace simcore::db::uiread::projectors {

ArtifactProjector::ArtifactProjector(sqlite3* db)
    : db_(db) {
}

bool ArtifactProjector::ProjectAll(std::string* error_out) {
    char* err = nullptr;
    constexpr const char* kSql =
        "BEGIN IMMEDIATE;"
        "INSERT INTO ui_artifact_browser(artifact_id,sha256,size_bytes,artifact_kind,filename,created_at_utc) "
        "SELECT artifact_id,sha256,size_bytes,artifact_kind,filename,created_at_utc FROM state_artifact "
        "ON CONFLICT(artifact_id) DO UPDATE SET "
        "sha256=excluded.sha256,size_bytes=excluded.size_bytes,artifact_kind=excluded.artifact_kind,"
        "filename=excluded.filename,created_at_utc=excluded.created_at_utc;"
        "COMMIT;";
    if (sqlite3_exec(db_, kSql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (error_out) *error_out = err ? err : sqlite3_errmsg(db_);
        sqlite3_free(err);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    return true;
}

bool ArtifactProjector::ProjectFromOutbox(const std::string& projector_name, int max_batch_size, std::string* error_out, int max_attempts) {
    if (!ValidateProjectorContractInputs(projector_name, max_batch_size, max_attempts, error_out)) {
        return false;
    }

    const auto project_all = [this](const events::EventEnvelope&, std::string* handler_error) {
        return ProjectAll(handler_error);
    };

    const std::vector<events::OutboxRelayDispatchBinding> bindings{
        { { "State.ArtifactStored.v1", 1 }, project_all },
    };

    return RunProjectorRelay(
        db_,
        projector_name,
        "State",
        "state_outbox_message",
        {
            .db = db_,
            .outbox_table = "state_outbox_message",
            .context_name = "State",
            .aggregate_kind = "artifact",
            .payload_ref_kind = "artifact",
            .max_attempts = max_attempts,
        },
        bindings,
        max_batch_size,
        error_out);
}

} // namespace simcore::db::uiread::projectors
