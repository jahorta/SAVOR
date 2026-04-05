#include "SqliteArchiveDb.h"

#include <chrono>
#include <string>
#include <utility>

#include "../Common/Events/EventPayloadDispatch.h"
#include "../Common/Events/EventPayloadValidation.h"

namespace simcore::db {

namespace {

struct Statement {
    sqlite3_stmt* st = nullptr;

    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
};

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

} // namespace simcore::db
