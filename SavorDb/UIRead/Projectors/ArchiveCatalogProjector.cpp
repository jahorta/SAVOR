#include "ArchiveCatalogProjector.h"

#include "ProjectorContract.h"

namespace savor::db::uiread::projectors {

ArchiveCatalogProjector::ArchiveCatalogProjector(sqlite3* db)
    : db_(db) {
}

bool ArchiveCatalogProjector::ProjectAll(std::string* error_out) {
    char* err = nullptr;
    constexpr const char* kSql =
        "BEGIN IMMEDIATE;"
        "INSERT INTO ui_archive_catalog(archive_package_id,source_context,source_root_job_set_id,created_at_utc,schema_version,event_catalog_version,time_range_start_utc,time_range_end_utc,checksum_status) "
        "SELECT archive_package_id,source_context,source_root_job_set_id,created_at_utc,schema_version,event_catalog_version,time_range_start_utc,time_range_end_utc,checksum_status "
        "FROM ar_archive_package "
        "WHERE 1 "
        "ON CONFLICT(archive_package_id) DO UPDATE SET "
        "source_context=excluded.source_context,source_root_job_set_id=excluded.source_root_job_set_id,"
        "created_at_utc=excluded.created_at_utc,schema_version=excluded.schema_version,event_catalog_version=excluded.event_catalog_version,"
        "time_range_start_utc=excluded.time_range_start_utc,time_range_end_utc=excluded.time_range_end_utc,checksum_status=excluded.checksum_status;"
        "INSERT INTO ui_archive_rehydrate_request(rehydrate_request_id,archive_package_id,status,target_namespace,requested_at_utc,completed_at_utc,error_text) "
        "SELECT rehydrate_request_id,archive_package_id,status,target_namespace,requested_at_utc,completed_at_utc,error_text "
        "FROM ar_rehydrate_request "
        "WHERE 1 "
        "ON CONFLICT(rehydrate_request_id) DO UPDATE SET "
        "archive_package_id=excluded.archive_package_id,status=excluded.status,target_namespace=excluded.target_namespace,"
        "requested_at_utc=excluded.requested_at_utc,completed_at_utc=excluded.completed_at_utc,error_text=excluded.error_text;"
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
    if (!ValidateProjectorContractInputs(projector_name, max_batch_size, max_attempts, error_out)) {
        return false;
    }

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

    return RunProjectorRelay(
        db_,
        projector_name,
        "Archive",
        "ar_outbox_message",
        {
            .db = db_,
            .outbox_table = "ar_outbox_message",
            .context_name = "Archive",
            .aggregate_kind = "archive_package",
            .payload_ref_kind = "",
            .max_attempts = max_attempts,
        },
        bindings,
        max_batch_size,
        error_out);
}

} // namespace savor::db::uiread::projectors
