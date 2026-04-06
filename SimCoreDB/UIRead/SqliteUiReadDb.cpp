#include "SqliteUiReadDb.h"

#include <chrono>

namespace simcore::db {

namespace {

std::int64_t ToEpochMillis(types::UtcTimePoint value) {
    return value.time_since_epoch().count();
}

types::UtcTimePoint FromEpochMillis(std::int64_t value) {
    return types::UtcTimePoint{ std::chrono::milliseconds(value) };
}

bool IsSubscriptionKeyValid(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table) {
    return !projector_name.empty() && !source_context.empty() && !source_outbox_table.empty();
}

bool ExecSql(sqlite3* db, const char* sql) {
    char* err_msg = nullptr;
    const auto rc = sqlite3_exec(db, sql, nullptr, nullptr, &err_msg);
    sqlite3_free(err_msg);
    return rc == SQLITE_OK;
}

std::optional<UiProjectionSubscription> ReadSubscription(
    sqlite3* db,
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table) {
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT projector_name, source_context, source_outbox_table, "
        "COALESCE(last_outbox_id, 0), COALESCE(last_event_id, ''), updated_at_utc, "
        "COALESCE(status, 'ACTIVE'), COALESCE(last_error, '') "
        "FROM ui_projection_subscription "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";

    if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(st, 1, projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<UiProjectionSubscription> subscription;
    if (sqlite3_step(st) == SQLITE_ROW) {
        subscription = UiProjectionSubscription{};
        const auto* projector_name_text = sqlite3_column_text(st, 0);
        const auto* source_context_text = sqlite3_column_text(st, 1);
        const auto* source_outbox_table_text = sqlite3_column_text(st, 2);
        const auto* last_event_id_text = sqlite3_column_text(st, 4);
        const auto* status_text = sqlite3_column_text(st, 6);
        const auto* last_error_text = sqlite3_column_text(st, 7);

        subscription->projector_name = projector_name_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(projector_name_text);
        subscription->source_context = source_context_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(source_context_text);
        subscription->source_outbox_table = source_outbox_table_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(source_outbox_table_text);
        subscription->last_outbox_id = sqlite3_column_int64(st, 3);
        subscription->last_event_id = last_event_id_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(last_event_id_text);
        subscription->updated_at_utc = FromEpochMillis(sqlite3_column_int64(st, 5));
        subscription->status = status_text == nullptr
            ? std::string{ "ACTIVE" }
            : reinterpret_cast<const char*>(status_text);
        subscription->last_error = last_error_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(last_error_text);
    }

    sqlite3_finalize(st);
    return subscription;
}

} // namespace

SqliteUiReadDb::SqliteUiReadDb(sqlite3* db)
    : db_(db) {
}

std::optional<UiProjectionCheckpoint> SqliteUiReadDb::GetProjectionCheckpoint(
    const std::string& projector_name) const {
    if (projector_name.empty()) {
        return std::nullopt;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT projector_name, COALESCE(last_event_id, ''), COALESCE(last_outbox_id, 0), updated_at_utc "
        "FROM ui_projection_checkpoint "
        "WHERE projector_name=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(st, 1, projector_name.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<UiProjectionCheckpoint> checkpoint;
    if (sqlite3_step(st) == SQLITE_ROW) {
        checkpoint = UiProjectionCheckpoint{};
        const auto* projector_name_text = sqlite3_column_text(st, 0);
        const auto* event_id_text = sqlite3_column_text(st, 1);

        checkpoint->projector_name = projector_name_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(projector_name_text);
        checkpoint->last_event_id = event_id_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(event_id_text);
        checkpoint->last_outbox_id = sqlite3_column_int64(st, 2);
        checkpoint->updated_at_utc = FromEpochMillis(sqlite3_column_int64(st, 3));
    }

    sqlite3_finalize(st);
    return checkpoint;
}

bool SqliteUiReadDb::UpsertProjectionCheckpoint(const UiProjectionCheckpoint& checkpoint) {
    if (checkpoint.projector_name.empty()) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "INSERT INTO ui_projection_checkpoint(projector_name, last_event_id, last_outbox_id, updated_at_utc) "
        "VALUES(?1, ?2, ?3, ?4) "
        "ON CONFLICT(projector_name) DO UPDATE SET "
        "last_event_id=excluded.last_event_id, "
        "last_outbox_id=excluded.last_outbox_id, "
        "updated_at_utc=excluded.updated_at_utc;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(st, 1, checkpoint.projector_name.c_str(), -1, SQLITE_TRANSIENT);
    if (checkpoint.last_event_id.empty()) {
        sqlite3_bind_null(st, 2);
    }
    else {
        sqlite3_bind_text(st, 2, checkpoint.last_event_id.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(st, 3, checkpoint.last_outbox_id);
    sqlite3_bind_int64(st, 4, ToEpochMillis(checkpoint.updated_at_utc));

    const auto rc = sqlite3_step(st);
    sqlite3_finalize(st);

    return rc == SQLITE_DONE;
}

std::optional<UiProjectionSubscription> SqliteUiReadDb::GetProjectionSubscription(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table) const {
    if (!IsSubscriptionKeyValid(projector_name, source_context, source_outbox_table)) {
        return std::nullopt;
    }

    return ReadSubscription(db_, projector_name, source_context, source_outbox_table);
}

std::vector<UiProjectionSubscription> SqliteUiReadDb::ListProjectionSubscriptions(
    const std::string& source_context,
    const std::string& source_outbox_table) const {
    std::vector<UiProjectionSubscription> subscriptions;
    if (source_context.empty() || source_outbox_table.empty()) {
        return subscriptions;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT projector_name, source_context, source_outbox_table, "
        "COALESCE(last_outbox_id, 0), COALESCE(last_event_id, ''), updated_at_utc, "
        "COALESCE(status, 'ACTIVE'), COALESCE(last_error, '') "
        "FROM ui_projection_subscription "
        "WHERE source_context=?1 AND source_outbox_table=?2 "
        "ORDER BY projector_name ASC;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return subscriptions;
    }
    sqlite3_bind_text(st, 1, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(st) == SQLITE_ROW) {
        UiProjectionSubscription subscription{};
        const auto* projector_name_text = sqlite3_column_text(st, 0);
        const auto* source_context_text = sqlite3_column_text(st, 1);
        const auto* source_outbox_table_text = sqlite3_column_text(st, 2);
        const auto* last_event_id_text = sqlite3_column_text(st, 4);
        const auto* status_text = sqlite3_column_text(st, 6);
        const auto* last_error_text = sqlite3_column_text(st, 7);
        subscription.projector_name = projector_name_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(projector_name_text);
        subscription.source_context = source_context_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(source_context_text);
        subscription.source_outbox_table = source_outbox_table_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(source_outbox_table_text);
        subscription.last_outbox_id = sqlite3_column_int64(st, 3);
        subscription.last_event_id = last_event_id_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(last_event_id_text);
        subscription.updated_at_utc = FromEpochMillis(sqlite3_column_int64(st, 5));
        subscription.status = status_text == nullptr
            ? std::string{ "ACTIVE" }
            : reinterpret_cast<const char*>(status_text);
        subscription.last_error = last_error_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(last_error_text);
        subscriptions.push_back(std::move(subscription));
    }

    sqlite3_finalize(st);
    return subscriptions;
}

std::optional<std::int64_t> SqliteUiReadDb::ComputeSafeFloorOutboxId(
    const std::string& source_context,
    const std::string& source_outbox_table) const {
    if (source_context.empty() || source_outbox_table.empty()) {
        return std::nullopt;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT MIN(last_outbox_id) "
        "FROM ui_projection_subscription "
        "WHERE source_context=?1 AND source_outbox_table=?2 AND status='ACTIVE';";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(st, 1, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<std::int64_t> safe_floor;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
        safe_floor = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return safe_floor;
}

std::optional<UiProjectionSubscription> SqliteUiReadDb::GetOrCreateProjectionSubscription(
    const UiProjectionSubscription& subscription) {
    if (!IsSubscriptionKeyValid(
            subscription.projector_name,
            subscription.source_context,
            subscription.source_outbox_table)) {
        return std::nullopt;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "INSERT INTO ui_projection_subscription("
        "projector_name, source_context, source_outbox_table, last_outbox_id, last_event_id, updated_at_utc, status, last_error) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
        "ON CONFLICT(projector_name, source_context, source_outbox_table) DO NOTHING;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(st, 1, subscription.projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, subscription.source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, subscription.source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, subscription.last_outbox_id);
    if (subscription.last_event_id.empty()) {
        sqlite3_bind_null(st, 5);
    }
    else {
        sqlite3_bind_text(st, 5, subscription.last_event_id.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(st, 6, ToEpochMillis(subscription.updated_at_utc));
    const auto status = subscription.status.empty() ? std::string{ "ACTIVE" } : subscription.status;
    sqlite3_bind_text(st, 7, status.c_str(), -1, SQLITE_TRANSIENT);
    if (subscription.last_error.empty()) {
        sqlite3_bind_null(st, 8);
    }
    else {
        sqlite3_bind_text(st, 8, subscription.last_error.c_str(), -1, SQLITE_TRANSIENT);
    }

    const auto rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return std::nullopt;
    }

    return GetProjectionSubscription(
        subscription.projector_name,
        subscription.source_context,
        subscription.source_outbox_table);
}

bool SqliteUiReadDb::AdvanceProjectionSubscriptionCursor(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    std::int64_t last_outbox_id,
    const std::string& last_event_id,
    types::UtcTimePoint updated_at_utc,
    const std::optional<UiProjectionSubscriptionBatchAudit>& batch_audit) {
    if (!IsSubscriptionKeyValid(projector_name, source_context, source_outbox_table)) {
        return false;
    }

    if (!ExecSql(db_, "BEGIN IMMEDIATE;")) {
        return false;
    }

    bool success = true;
    sqlite3_stmt* st = nullptr;
    constexpr const char* kUpdateSql =
        "UPDATE ui_projection_subscription "
        "SET last_outbox_id=?4, last_event_id=?5, updated_at_utc=?6, status='ACTIVE', last_error=NULL "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";

    if (sqlite3_prepare_v2(db_, kUpdateSql, -1, &st, nullptr) != SQLITE_OK) {
        success = false;
    }
    else {
        sqlite3_bind_text(st, 1, projector_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, source_context.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, last_outbox_id);
        if (last_event_id.empty()) {
            sqlite3_bind_null(st, 5);
        }
        else {
            sqlite3_bind_text(st, 5, last_event_id.c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_bind_int64(st, 6, ToEpochMillis(updated_at_utc));

        if (sqlite3_step(st) != SQLITE_DONE || sqlite3_changes(db_) == 0) {
            success = false;
        }
    }
    sqlite3_finalize(st);

    if (success && batch_audit.has_value()) {
        sqlite3_stmt* audit_st = nullptr;
        constexpr const char* kAuditSql =
            "INSERT INTO ui_projection_subscription_audit("
            "projector_name, source_context, source_outbox_table, from_outbox_id, to_outbox_id, processed_count, failed_count, recorded_at_utc) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);";

        if (sqlite3_prepare_v2(db_, kAuditSql, -1, &audit_st, nullptr) != SQLITE_OK) {
            success = false;
        }
        else {
            sqlite3_bind_text(audit_st, 1, batch_audit->projector_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(audit_st, 2, batch_audit->source_context.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(audit_st, 3, batch_audit->source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(audit_st, 4, batch_audit->from_outbox_id);
            sqlite3_bind_int64(audit_st, 5, batch_audit->to_outbox_id);
            sqlite3_bind_int64(audit_st, 6, batch_audit->processed_count);
            sqlite3_bind_int64(audit_st, 7, batch_audit->failed_count);
            sqlite3_bind_int64(audit_st, 8, ToEpochMillis(batch_audit->recorded_at_utc));

            if (sqlite3_step(audit_st) != SQLITE_DONE) {
                success = false;
            }
        }
        sqlite3_finalize(audit_st);
    }

    if (success) {
        success = ExecSql(db_, "COMMIT;");
    }
    else {
        ExecSql(db_, "ROLLBACK;");
    }

    return success;
}

bool SqliteUiReadDb::SetProjectionSubscriptionError(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    const std::string& last_error,
    types::UtcTimePoint updated_at_utc) {
    if (!IsSubscriptionKeyValid(projector_name, source_context, source_outbox_table)) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "UPDATE ui_projection_subscription "
        "SET status='ERROR', last_error=?4, updated_at_utc=?5 "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(st, 1, projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, last_error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, ToEpochMillis(updated_at_utc));

    const auto rc = sqlite3_step(st);
    const auto rows = sqlite3_changes(db_);
    sqlite3_finalize(st);

    return rc == SQLITE_DONE && rows > 0;
}

bool SqliteUiReadDb::PauseProjectionSubscription(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    types::UtcTimePoint updated_at_utc,
    const std::string& reason) {
    if (!IsSubscriptionKeyValid(projector_name, source_context, source_outbox_table)) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "UPDATE ui_projection_subscription "
        "SET status='PAUSED', last_error=?4, updated_at_utc=?5 "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(st, 1, projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    if (reason.empty()) {
        sqlite3_bind_null(st, 4);
    }
    else {
        sqlite3_bind_text(st, 4, reason.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(st, 5, ToEpochMillis(updated_at_utc));

    const auto rc = sqlite3_step(st);
    const auto rows = sqlite3_changes(db_);
    sqlite3_finalize(st);

    return rc == SQLITE_DONE && rows > 0;
}

bool SqliteUiReadDb::ResumeProjectionSubscription(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    types::UtcTimePoint updated_at_utc) {
    if (!IsSubscriptionKeyValid(projector_name, source_context, source_outbox_table)) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "UPDATE ui_projection_subscription "
        "SET status='ACTIVE', last_error=NULL, updated_at_utc=?4 "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(st, 1, projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, ToEpochMillis(updated_at_utc));

    const auto rc = sqlite3_step(st);
    const auto rows = sqlite3_changes(db_);
    sqlite3_finalize(st);

    return rc == SQLITE_DONE && rows > 0;
}

} // namespace simcore::db
