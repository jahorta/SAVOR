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

} // namespace simcore::db
