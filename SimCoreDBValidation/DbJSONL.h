#pragma once

#include <string>

inline std::string EscapeIdentifier(const std::string& identifier) {
    std::string escaped;
    escaped.reserve(identifier.size() + 4);
    escaped.push_back('"');
    for (char c : identifier) {
        if (c == '"') {
            escaped += "\"\"";
        }
        else {
            escaped.push_back(c);
        }
    }
    escaped.push_back('"');
    return escaped;
}

inline std::string EscapeJsonPathKey(const std::string& key) {
    std::string escaped;
    escaped.reserve(key.size() + 4);
    escaped += "$.\"";
    for (char c : key) {
        if (c == '"' || c == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

inline bool CollectObjectKeys(sqlite3* db, const std::string& object_json, std::vector<std::string>* keys_out, std::string* error_out) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT key FROM json_each(?1) WHERE key IS NOT NULL;", -1, &st, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db);
        }
        return false;
    }
    sqlite3_bind_text(st, 1, object_json.c_str(), static_cast<int>(object_json.size()), SQLITE_TRANSIENT);
    keys_out->clear();
    while (sqlite3_step(st) == SQLITE_ROW) {
        const auto* key = sqlite3_column_text(st, 0);
        if (key != nullptr) {
            keys_out->emplace_back(reinterpret_cast<const char*>(key));
        }
    }
    const int rc = sqlite3_errcode(db);
    sqlite3_finalize(st);
    if (rc != SQLITE_OK && rc != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db);
        }
        return false;
    }
    return true;
}