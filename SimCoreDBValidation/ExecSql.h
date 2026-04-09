#pragma once

#include <string>

#include "sqlite3.h"

inline bool ExecSql(sqlite3* db, const char* sql, std::string* error_out) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc == SQLITE_OK) {
        return true;
    }

    if (error_out != nullptr) {
        *error_out = err != nullptr ? err : "sqlite3_exec failed";
    }
    sqlite3_free(err);
    return false;
}