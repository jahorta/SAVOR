#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <sqlite3.h>

namespace savor::db::outbox {

inline bool MakeDbOwnedEventId(
    sqlite3* db,
    std::string_view context_name,
    std::string_view event_type,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id,
    std::string* event_id_out,
    std::string* error_out) {
    if (db == nullptr || event_id_out == nullptr || context_name.empty() || event_type.empty()
        || payload_ref_kind.empty() || payload_ref_id <= 0) {
        if (error_out) *error_out = "invalid outbox event id inputs";
        return false;
    }

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT lower(hex(randomblob(16)));", -1, &st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }

    const auto rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        sqlite3_finalize(st);
        return false;
    }

    const auto* nonce_text = sqlite3_column_text(st, 0);
    const std::string nonce = nonce_text == nullptr
        ? std::string{}
        : reinterpret_cast<const char*>(nonce_text);
    sqlite3_finalize(st);

    if (nonce.empty()) {
        if (error_out) *error_out = "sqlite randomblob did not produce an event id suffix";
        return false;
    }

    *event_id_out = std::string(context_name) + "." + std::string(event_type) + "."
        + std::string(payload_ref_kind) + "." + std::to_string(payload_ref_id) + "." + nonce;
    return true;
}

inline bool IsUniqueConstraint(sqlite3* db) {
    return sqlite3_extended_errcode(db) == SQLITE_CONSTRAINT_UNIQUE;
}

} // namespace savor::db::outbox
