#include "ArtifactProjector.h"

#include "../../Common/Events/OutboxRelay.h"
#include "../SqliteUiReadDb.h"

namespace simcore::db::uiread::projectors {

ArtifactProjector::ArtifactProjector(sqlite3* db)
    : db_(db) {
}

std::int64_t ArtifactProjector::GetCheckpoint(const std::string& projector_name, std::string* error_out) const {
    if (projector_name.empty()) {
        if (error_out) *error_out = "projector_name is required";
        return 0;
    }

    simcore::db::SqliteUiReadDb ui_read_db(db_);
    const auto checkpoint = ui_read_db.GetProjectionCheckpoint(projector_name);
    return checkpoint.has_value() ? checkpoint->last_outbox_id : 0;
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
        .outbox_table = "state_outbox_message",
        .context_name = "State",
        .aggregate_kind = "artifact",
        .payload_ref_kind = "artifact",
        .max_attempts = max_attempts,
    });

    const auto project_all = [this](const events::EventEnvelope&, std::string* handler_error) {
        return ProjectAll(handler_error);
    };

    const std::vector<events::OutboxRelayDispatchBinding> bindings{
        { { "State.ArtifactStored.v1", 1 }, project_all },
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
