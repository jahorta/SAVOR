#include "ArchiveCatalogProjector.h"

#include "../../Common/Events/OutboxRelay.h"
#include "../SqliteUiReadDb.h"

namespace simcore::db::uiread::projectors {

ArchiveCatalogProjector::ArchiveCatalogProjector(sqlite3* db)
    : db_(db) {
}

std::int64_t ArchiveCatalogProjector::GetCheckpoint(const std::string& projector_name, std::string* error_out) const {
    if (projector_name.empty()) {
        if (error_out) *error_out = "projector_name is required";
        return 0;
    }

    simcore::db::SqliteUiReadDb ui_read_db(db_);
    const auto checkpoint = ui_read_db.GetProjectionCheckpoint(projector_name);
    return checkpoint.has_value() ? checkpoint->last_outbox_id : 0;
}

bool ArchiveCatalogProjector::ProjectAll(std::string* error_out) {
    char* err = nullptr;
    constexpr const char* kSql =
        "BEGIN IMMEDIATE;"
        "INSERT INTO ui_archive_catalog(archive_package_id,source_context,source_root_job_set_id,created_at_utc,schema_version,event_catalog_version,time_range_start_utc,time_range_end_utc,checksum_status) "
        "SELECT archive_package_id,source_context,source_root_job_set_id,created_at_utc,schema_version,event_catalog_version,time_range_start_utc,time_range_end_utc,checksum_status "
        "FROM ar_archive_package "
        "ON CONFLICT(archive_package_id) DO UPDATE SET "
        "source_context=excluded.source_context,source_root_job_set_id=excluded.source_root_job_set_id,"
        "created_at_utc=excluded.created_at_utc,schema_version=excluded.schema_version,event_catalog_version=excluded.event_catalog_version,"
        "time_range_start_utc=excluded.time_range_start_utc,time_range_end_utc=excluded.time_range_end_utc,checksum_status=excluded.checksum_status;"
        "COMMIT;";
    if (sqlite3_exec(db_, kSql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (error_out) *error_out = err ? err : sqlite3_errmsg(db_);
        sqlite3_free(err);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    return true;
}

bool ArchiveCatalogProjector::ProjectFromOutbox(const std::string& projector_name, int max_batch_size, std::string* error_out, int max_attempts) {
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
        .outbox_table = "ar_outbox_message",
        .context_name = "Archive",
        .aggregate_kind = "archive_package",
        .payload_ref_kind = "archive_package",
        .max_attempts = max_attempts,
    });

    const auto project_all = [this](const events::EventEnvelope&, std::string* handler_error) {
        return ProjectAll(handler_error);
    };

    const std::vector<events::OutboxRelayDispatchBinding> bindings{
        { { "Archive.PackageCreated.v1", 1 }, project_all },
        { { "Archive.PackageIndexed.v1", 1 }, project_all },
        { { "Archive.RehydrateRequested.v1", 1 }, project_all },
        { { "Archive.RehydrateCompleted.v1", 1 }, project_all },
        { { "Archive.RehydrateFailed.v1", 1 }, project_all },
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
