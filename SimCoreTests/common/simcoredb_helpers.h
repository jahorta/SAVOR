#pragma once

#include <string>

#include <sqlite3.h>

inline bool ExecSql(sqlite3* db, const char* sql) {
    if (db == nullptr || sql == nullptr) {
        return false;
    }

    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err != nullptr) {
        sqlite3_free(err);
    }
    return rc == SQLITE_OK;
}

inline bool TableExists(sqlite3* db, const char* table_name) {
    if (db == nullptr || table_name == nullptr) {
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    constexpr const char* kSql =
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1;";
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, table_name, -1, SQLITE_TRANSIENT);
    const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return exists;
}

inline bool ColumnExists(sqlite3* db, const char* table_name, const char* column_name) {
    if (db == nullptr || table_name == nullptr || column_name == nullptr) {
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    constexpr const char* kSql =
        "SELECT 1 FROM pragma_table_info(?1) WHERE name=?2 LIMIT 1;";
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, table_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, column_name, -1, SQLITE_TRANSIENT);
    const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return exists;
}

inline bool IndexExists(sqlite3* db, const char* index_name) {
    if (db == nullptr || index_name == nullptr) {
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    constexpr const char* kSql =
        "SELECT 1 FROM sqlite_master WHERE type='index' AND name=?1 LIMIT 1;";
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, index_name, -1, SQLITE_TRANSIENT);
    const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return exists;
}

inline std::string TableCreateSql(sqlite3* db, const char* table_name) {
    if (db == nullptr || table_name == nullptr) {
        return {};
    }

    sqlite3_stmt* stmt = nullptr;
    constexpr const char* kSql =
        "SELECT sql FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1;";
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }

    sqlite3_bind_text(stmt, 1, table_name, -1, SQLITE_TRANSIENT);
    std::string create_sql;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* text = sqlite3_column_text(stmt, 0);
        if (text != nullptr) {
            create_sql = reinterpret_cast<const char*>(text);
        }
    }
    sqlite3_finalize(stmt);
    return create_sql;
}
