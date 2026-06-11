#include "SqliteArchiveDb.h"

#include <chrono>
#include <string>
#include <utility>

#include "../Common/Events/EventPayloadDispatch.h"
#include "../Common/Events/EventPayloadValidation.h"
#include "../Common/Events/OutboxEventIds.h"

namespace savor::db {

namespace {

struct Statement {
    Statement() {}
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* st = nullptr;
};





bool InsertArchiveOutboxEvent(
    sqlite3* db,
    std::string_view event_type,
    std::string_view aggregate_kind,
    std::string_view aggregate_id,
    std::string_view correlation_id,
    std::string_view causation_id,
    std::int64_t occurred_at_utc,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id,
    std::string* error_out) {
    constexpr int kMaxEventIdAttempts = 5;
    for (int attempt = 0; attempt < kMaxEventIdAttempts; ++attempt) {
        std::string event_id;
        if (!outbox::MakeDbOwnedEventId(
                db,
                "Archive",
                event_type,
                payload_ref_kind,
                payload_ref_id,
                &event_id,
                error_out)) {
            return false;
        }

        Statement st;
        if (sqlite3_prepare_v2(
                db,
                "INSERT INTO ar_outbox_message("
                "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
                "VALUES(?1,?2,1,'Archive',?3,?4,?5,?6,?7,?8,?9);",
                -1,
                &st.st,
                nullptr)
            != SQLITE_OK) {
            if (error_out) {
                *error_out = sqlite3_errmsg(db);
            }
            return false;
        }

        sqlite3_bind_text(st.st, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 2, event_type.data(), static_cast<int>(event_type.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 3, aggregate_kind.data(), static_cast<int>(aggregate_kind.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 4, aggregate_id.data(), static_cast<int>(aggregate_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 5, correlation_id.data(), static_cast<int>(correlation_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 6, causation_id.data(), static_cast<int>(causation_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(st.st, 7, occurred_at_utc);
        sqlite3_bind_text(st.st, 8, payload_ref_kind.data(), static_cast<int>(payload_ref_kind.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(st.st, 9, payload_ref_id);
        const auto rc = sqlite3_step(st.st);
        if (rc == SQLITE_DONE) {
            return true;
        }
        if (!outbox::IsUniqueConstraint(db)) {
            if (error_out != nullptr) {
                *error_out = sqlite3_errmsg(db);
            }
            return false;
        }
    }
    if (error_out != nullptr) {
        *error_out = "failed to generate a unique archive outbox event id";
    }
    return false;
}

std::optional<std::int64_t> PackageIdForRehydrateRequest(sqlite3* db, std::int64_t rehydrate_request_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT archive_package_id FROM ar_rehydrate_request WHERE rehydrate_request_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, rehydrate_request_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st.st, 0);
}

std::optional<events::ArchivePackagePayloadView> ResolveArchivePackage(
    sqlite3* db,
    std::int64_t payload_ref_id,
    std::string_view payload_ref_kind) {
    if (db == nullptr || payload_ref_id <= 0) {
        return std::nullopt;
    }

    if (payload_ref_kind == "archive_package" || payload_ref_kind == "package") {
        Statement st;
        if (sqlite3_prepare_v2(
                db,
                "SELECT archive_package_id FROM ar_archive_package WHERE archive_package_id=?1;",
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

        events::ArchivePackagePayloadView payload{};
        payload.archive_package_id = sqlite3_column_int64(st.st, 0);
        return payload;
    }

    if (payload_ref_kind == "archive_item" || payload_ref_kind == "package_item") {
        Statement st;
        if (sqlite3_prepare_v2(
                db,
                "SELECT archive_package_id, archive_item_id FROM ar_archive_item WHERE archive_item_id=?1;",
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

        events::ArchivePackagePayloadView payload{};
        payload.archive_package_id = sqlite3_column_int64(st.st, 0);
        payload.archive_item_id = sqlite3_column_int64(st.st, 1);
        return payload;
    }

    if (payload_ref_kind == "rehydrate_request" || payload_ref_kind == "rehydrate_result") {
        Statement st;
        if (sqlite3_prepare_v2(
                db,
                "SELECT archive_package_id, rehydrate_request_id FROM ar_rehydrate_request WHERE rehydrate_request_id=?1;",
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

        events::ArchivePackagePayloadView payload{};
        payload.archive_package_id = sqlite3_column_int64(st.st, 0);
        payload.rehydrate_request_id = sqlite3_column_int64(st.st, 1);
        return payload;
    }

    return std::nullopt;
}

bool ValidateArchiveEnvelopeShape(const events::EventEnvelope& envelope) {
    if (!events::ValidateV1EnvelopeBasics(envelope)) {
        return false;
    }

    if (envelope.context_name != "Archive") {
        return false;
    }

    if (envelope.event_type == "Archive.PackageCreated.v1") {
        return envelope.payload_ref_kind == "archive_package" || envelope.payload_ref_kind == "package";
    }
    if (envelope.event_type == "Archive.PackageIndexed.v1") {
        return envelope.payload_ref_kind == "archive_item" || envelope.payload_ref_kind == "package_item"
            || envelope.payload_ref_kind == "archive_package" || envelope.payload_ref_kind == "package";
    }
    if (envelope.event_type == "Archive.RehydrateRequested.v1") {
        return envelope.payload_ref_kind == "rehydrate_request";
    }
    if (envelope.event_type == "Archive.RehydrateCompleted.v1"
        || envelope.event_type == "Archive.RehydrateFailed.v1") {
        return envelope.payload_ref_kind == "rehydrate_result" || envelope.payload_ref_kind == "rehydrate_request";
    }

    return false;
}

} // namespace

SqliteArchiveDb::SqliteArchiveDb(sqlite3* db)
    : db_(db) {
}

bool SqliteArchiveDb::CreateArchivePackage(
    const CreateArchivePackageCommand& command,
    std::int64_t* archive_package_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.source_context.empty()
        || command.source_root_job_set_id <= 0
        || command.manifest_path.empty()
        || command.checksum_status.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_package;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ar_archive_package(source_context,source_root_job_set_id,created_at_utc,schema_version,event_catalog_version,time_range_start_utc,time_range_end_utc,manifest_path,checksum_status) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);",
            -1,
            &insert_package.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(insert_package.st, 1, command.source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_package.st, 2, command.source_root_job_set_id);
    sqlite3_bind_int64(insert_package.st, 3, command.created_at_utc.time_since_epoch().count());
    sqlite3_bind_int64(insert_package.st, 4, command.schema_version);
    sqlite3_bind_int(insert_package.st, 5, command.event_catalog_version);
    sqlite3_bind_int64(insert_package.st, 6, command.time_range_start_utc.time_since_epoch().count());
    sqlite3_bind_int64(insert_package.st, 7, command.time_range_end_utc.time_since_epoch().count());
    sqlite3_bind_text(insert_package.st, 8, command.manifest_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_package.st, 9, command.checksum_status.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(insert_package.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto archive_package_id = sqlite3_last_insert_rowid(db_);
    if (!InsertArchiveOutboxEvent(
            db_,
            "Archive.PackageCreated.v1",
            "archive_package",
            std::to_string(archive_package_id),
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "archive_package",
            archive_package_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (archive_package_id_out) {
        *archive_package_id_out = archive_package_id;
    }
    return true;
}

bool SqliteArchiveDb::AddArchiveItem(
    const AddArchiveItemCommand& command,
    std::int64_t* archive_item_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.archive_package_id <= 0 || command.item_kind.empty() || command.item_count < 0) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_item;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ar_archive_item(archive_package_id,item_kind,item_count,blob_path,checksum) "
            "VALUES(?1,?2,?3,?4,?5);",
            -1,
            &insert_item.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_item.st, 1, command.archive_package_id);
    sqlite3_bind_text(insert_item.st, 2, command.item_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insert_item.st, 3, command.item_count);
    if (command.blob_path.has_value()) sqlite3_bind_text(insert_item.st, 4, command.blob_path->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_item.st, 4);
    if (command.checksum.has_value()) sqlite3_bind_text(insert_item.st, 5, command.checksum->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_item.st, 5);
    if (sqlite3_step(insert_item.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto archive_item_id = sqlite3_last_insert_rowid(db_);
    if (!InsertArchiveOutboxEvent(
            db_,
            "Archive.PackageIndexed.v1",
            "archive_package",
            std::to_string(command.archive_package_id),
            command.correlation_id,
            command.causation_id,
            command.indexed_at_utc.time_since_epoch().count(),
            "archive_item",
            archive_item_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (archive_item_id_out) {
        *archive_item_id_out = archive_item_id;
    }
    return true;
}

bool SqliteArchiveDb::RequestRehydrate(
    const RequestRehydrateCommand& command,
    std::int64_t* rehydrate_request_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.archive_package_id <= 0
        || command.status.empty()
        || command.target_namespace.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_request;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ar_rehydrate_request(archive_package_id,status,requested_at_utc,completed_at_utc,error_text,target_namespace) "
            "VALUES(?1,?2,?3,NULL,NULL,?4);",
            -1,
            &insert_request.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_request.st, 1, command.archive_package_id);
    sqlite3_bind_text(insert_request.st, 2, command.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_request.st, 3, command.requested_at_utc.time_since_epoch().count());
    sqlite3_bind_text(insert_request.st, 4, command.target_namespace.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(insert_request.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto rehydrate_request_id = sqlite3_last_insert_rowid(db_);
    if (!InsertArchiveOutboxEvent(
            db_,
            "Archive.RehydrateRequested.v1",
            "archive_package",
            std::to_string(command.archive_package_id),
            command.correlation_id,
            command.causation_id,
            command.requested_at_utc.time_since_epoch().count(),
            "rehydrate_request",
            rehydrate_request_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (rehydrate_request_id_out) {
        *rehydrate_request_id_out = rehydrate_request_id;
    }
    return true;
}

bool SqliteArchiveDb::CompleteRehydrate(
    const CompleteRehydrateCommand& command,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.rehydrate_request_id <= 0 || command.status.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    const auto archive_package_id = PackageIdForRehydrateRequest(db_, command.rehydrate_request_id);
    if (!archive_package_id.has_value()) {
        if (error_out) *error_out = "rehydrate_request_id was not found";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement update_request;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ar_rehydrate_request SET status=?2, completed_at_utc=?3, error_text=NULL WHERE rehydrate_request_id=?1;",
            -1,
            &update_request.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(update_request.st, 1, command.rehydrate_request_id);
    sqlite3_bind_text(update_request.st, 2, command.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update_request.st, 3, command.completed_at_utc.time_since_epoch().count());
    if (sqlite3_step(update_request.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    Statement insert_map;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ar_rehydrate_map(rehydrate_request_id,entity_kind,old_id,new_id) VALUES(?1,?2,?3,?4);",
            -1,
            &insert_map.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    for (const auto& mapping : command.entity_mappings) {
        sqlite3_reset(insert_map.st);
        sqlite3_clear_bindings(insert_map.st);
        sqlite3_bind_int64(insert_map.st, 1, command.rehydrate_request_id);
        sqlite3_bind_text(insert_map.st, 2, mapping.entity_kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_map.st, 3, mapping.old_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_map.st, 4, mapping.new_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insert_map.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }

    if (!InsertArchiveOutboxEvent(
            db_,
            "Archive.RehydrateCompleted.v1",
            "archive_package",
            std::to_string(archive_package_id.value()),
            command.correlation_id,
            command.causation_id,
            command.completed_at_utc.time_since_epoch().count(),
            "rehydrate_request",
            command.rehydrate_request_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

bool SqliteArchiveDb::FailRehydrate(
    const FailRehydrateCommand& command,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.rehydrate_request_id <= 0 || command.status.empty() || command.error_text.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    const auto archive_package_id = PackageIdForRehydrateRequest(db_, command.rehydrate_request_id);
    if (!archive_package_id.has_value()) {
        if (error_out) *error_out = "rehydrate_request_id was not found";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement update_request;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ar_rehydrate_request SET status=?2, completed_at_utc=?3, error_text=?4 WHERE rehydrate_request_id=?1;",
            -1,
            &update_request.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(update_request.st, 1, command.rehydrate_request_id);
    sqlite3_bind_text(update_request.st, 2, command.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update_request.st, 3, command.completed_at_utc.time_since_epoch().count());
    sqlite3_bind_text(update_request.st, 4, command.error_text.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(update_request.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (!InsertArchiveOutboxEvent(
            db_,
            "Archive.RehydrateFailed.v1",
            "archive_package",
            std::to_string(archive_package_id.value()),
            command.correlation_id,
            command.causation_id,
            command.completed_at_utc.time_since_epoch().count(),
            "rehydrate_request",
            command.rehydrate_request_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

std::vector<events::EventEnvelope> SqliteArchiveDb::ReadUnpublishedOutboxBatch(
    std::int64_t after_outbox_id,
    int max_batch_size) {
    std::vector<events::EventEnvelope> batch;
    if (db_ == nullptr || max_batch_size <= 0) {
        return batch;
    }

    Statement st;
    constexpr const char* kSql =
        "SELECT event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,"
        "COALESCE(correlation_id,''),COALESCE(causation_id,''),occurred_at_utc,payload_ref_kind,payload_ref_id "
        "FROM ar_outbox_message "
        "WHERE outbox_id > ?1 "
        "AND published_at_utc IS NULL "
        "ORDER BY outbox_id ASC "
        "LIMIT ?2;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return batch;
    }

    sqlite3_bind_int64(st.st, 1, after_outbox_id);
    sqlite3_bind_int(st.st, 2, max_batch_size);

    while (sqlite3_step(st.st) == SQLITE_ROW) {
        events::EventEnvelope envelope{};
        const auto* event_id = sqlite3_column_text(st.st, 0);
        const auto* event_type = sqlite3_column_text(st.st, 1);
        const auto event_version = sqlite3_column_int(st.st, 2);
        const auto* context_name = sqlite3_column_text(st.st, 3);
        const auto* aggregate_kind = sqlite3_column_text(st.st, 4);
        const auto* aggregate_id = sqlite3_column_text(st.st, 5);
        const auto* correlation_id = sqlite3_column_text(st.st, 6);
        const auto* causation_id = sqlite3_column_text(st.st, 7);
        const auto occurred_at_utc = sqlite3_column_int64(st.st, 8);
        const auto* payload_ref_kind = sqlite3_column_text(st.st, 9);
        const auto payload_ref_id = sqlite3_column_int64(st.st, 10);

        envelope.event_id = event_id == nullptr ? "" : reinterpret_cast<const char*>(event_id);
        envelope.event_type = event_type == nullptr ? "" : reinterpret_cast<const char*>(event_type);
        envelope.event_version = event_version;
        envelope.context_name = context_name == nullptr ? "" : reinterpret_cast<const char*>(context_name);
        envelope.aggregate_kind = aggregate_kind == nullptr ? "" : reinterpret_cast<const char*>(aggregate_kind);
        envelope.aggregate_id = aggregate_id == nullptr ? "" : reinterpret_cast<const char*>(aggregate_id);
        envelope.correlation_id = correlation_id == nullptr ? "" : reinterpret_cast<const char*>(correlation_id);
        envelope.causation_id = causation_id == nullptr ? "" : reinterpret_cast<const char*>(causation_id);
        envelope.occurred_at_utc = types::UtcTimePoint{ std::chrono::milliseconds(occurred_at_utc) };
        envelope.payload_ref_kind = payload_ref_kind == nullptr ? "" : reinterpret_cast<const char*>(payload_ref_kind);
        envelope.payload_ref_id = payload_ref_id;

        batch.push_back(std::move(envelope));
    }

    return batch;
}

bool SqliteArchiveDb::MarkOutboxPublished(
    std::int64_t outbox_id,
    types::UtcTimePoint published_at_utc) {
    if (db_ == nullptr || outbox_id <= 0) {
        return false;
    }

    Statement st;
    constexpr const char* kSql =
        "UPDATE ar_outbox_message "
        "SET published_at_utc=?2, last_error=NULL "
        "WHERE outbox_id=?1;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st.st, 1, outbox_id);
    sqlite3_bind_int64(st.st, 2, published_at_utc.time_since_epoch().count());

    if (sqlite3_step(st.st) != SQLITE_DONE) {
        return false;
    }

    return sqlite3_changes(db_) > 0;
}

bool SqliteArchiveDb::MarkOutboxPublishFailure(
    std::int64_t outbox_id,
    std::string_view last_error) {
    if (db_ == nullptr || outbox_id <= 0) {
        return false;
    }

    Statement st;
    constexpr const char* kSql =
        "UPDATE ar_outbox_message "
        "SET attempt_count=attempt_count+1, last_error=?2 "
        "WHERE outbox_id=?1;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st.st, 1, outbox_id);
    if (last_error.empty()) {
        sqlite3_bind_null(st.st, 2);
    }
    else {
        const std::string error(last_error);
        sqlite3_bind_text(st.st, 2, error.c_str(), -1, SQLITE_TRANSIENT);
    }

    if (sqlite3_step(st.st) != SQLITE_DONE) {
        return false;
    }

    return sqlite3_changes(db_) > 0;
}

retention::OutboxRetentionPreview SqliteArchiveDb::PreviewOutboxRetention(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy) const {
    std::int64_t max_outbox_id = 0;
    Statement st;
    if (db_ != nullptr
        && sqlite3_prepare_v2(db_, "SELECT COALESCE(MAX(outbox_id), 0) FROM ar_outbox_message;", -1, &st.st, nullptr) == SQLITE_OK
        && sqlite3_step(st.st) == SQLITE_ROW) {
        max_outbox_id = sqlite3_column_int64(st.st, 0);
    }
    return retention::BuildOutboxRetentionPreview(max_outbox_id, subscriptions, now_utc, policy);
}

bool SqliteArchiveDb::PurgeOutboxThroughRetentionFloor(
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
    if (sqlite3_prepare_v2(
            db_,
            "DELETE FROM ar_outbox_message "
            "WHERE outbox_id IN ("
            "  SELECT outbox_id FROM ar_outbox_message "
            "  WHERE published_at_utc IS NOT NULL AND outbox_id < ?1 "
            "  ORDER BY outbox_id ASC LIMIT ?2"
            ");",
            -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, preview.safe_purge_floor_outbox_id.value());
    sqlite3_bind_int(st.st, 2, max_rows);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    if (rows_deleted_out) *rows_deleted_out = sqlite3_changes(db_);
    return true;
}

std::optional<ArchivePayloadRecord> SqliteArchiveDb::ResolveArchivePayload(
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (event_version != 1) {
        return std::nullopt;
    }

    return ResolveArchivePackage(db_, payload_ref_id, payload_ref_kind);
}

std::optional<ArchivePayloadRecord> SqliteArchiveDb::ResolveArchivePayload(
    const events::EventEnvelope& envelope) const {
    if (!ValidateArchiveEnvelopeShape(envelope)) {
        return std::nullopt;
    }

    const auto contract = events::ResolvePayloadResolverContract(envelope.event_type, envelope.event_version);
    if (!contract.has_value() || contract.value() != events::PayloadResolverContract::ArchivePackageV1) {
        return std::nullopt;
    }

    return ResolveArchivePayload(
        envelope.event_version,
        envelope.payload_ref_kind,
        envelope.payload_ref_id);
}

} // namespace savor::db
