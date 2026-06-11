#include "ExportCurrentDbSchemas.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <sqlite3.h>

#include "Common/DbService.h"

namespace {

bool EndsWithSemicolon(const std::string& statement) {
    for (auto it = statement.rbegin(); it != statement.rend(); ++it) {
        if (*it == ' ' || *it == '\t' || *it == '\r' || *it == '\n') {
            continue;
        }
        return *it == ';';
    }
    return false;
}

bool ExportSqliteSchema(
    const std::filesystem::path& db_path,
    const std::filesystem::path& sql_path,
    std::string* error_out) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(db_path.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = "failed opening database " + db_path.string() + ": "
                + (db != nullptr ? sqlite3_errmsg(db) : "sqlite3_open_v2 failed");
        }
        sqlite3_close(db);
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    constexpr const char* kSchemaSql = R"SQL(
SELECT sql
FROM sqlite_schema
WHERE sql IS NOT NULL
  AND name NOT LIKE 'sqlite_%'
ORDER BY rowid;
)SQL";

    if (sqlite3_prepare_v2(db, kSchemaSql, -1, &stmt, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = "failed preparing schema query for " + db_path.string() + ": " + sqlite3_errmsg(db);
        }
        sqlite3_close(db);
        return false;
    }

    std::ofstream out(sql_path, std::ios::trunc);
    if (!out) {
        if (error_out != nullptr) {
            *error_out = "failed opening schema output: " + sql_path.string();
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return false;
    }

    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            if (error_out != nullptr) {
                *error_out = "failed reading schema for " + db_path.string() + ": " + sqlite3_errmsg(db);
            }
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            return false;
        }

        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const std::string statement = text != nullptr ? text : "";
        out << statement;
        if (!EndsWithSemicolon(statement)) {
            out << ';';
        }
        out << '\n';
    }

    if (!out) {
        if (error_out != nullptr) {
            *error_out = "failed writing schema output: " + sql_path.string();
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return false;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return true;
}

} // namespace

bool ExportCurrentDbSchemas(
    const std::filesystem::path& migration_root,
    std::string* error_out) {
    using namespace savor::db;
    using namespace savor::db::core;
    using namespace savor::db::migrations;

    const std::filesystem::path db_root = ".db_val";
    const std::filesystem::path schema_root = ".db_schema_cur";
    const DbConfigPaths config_paths{
        .execution_db_path = db_root / "execution.db",
        .state_db_path = db_root / "state.db",
        .analysis_db_path = db_root / "analysis.db",
        .authoring_db_path = db_root / "authoring.db",
        .ui_read_db_path = db_root / "ui_read.db",
        .archive_db_path = db_root / "archive.db",
        .object_store_root = db_root / "object_store",
        .archive_store_root = db_root / "archive_store",
    };

    std::error_code fs_error;
    std::filesystem::remove_all(db_root, fs_error);
    fs_error.clear();
    std::filesystem::remove_all(schema_root, fs_error);
    fs_error.clear();
    std::filesystem::create_directories(db_root, fs_error);
    if (fs_error) {
        if (error_out != nullptr) {
            *error_out = "failed creating db root: " + db_root.string() + " (" + fs_error.message() + ")";
        }
        return false;
    }
    std::filesystem::create_directories(config_paths.object_store_root, fs_error);
    if (fs_error) {
        if (error_out != nullptr) {
            *error_out = "failed creating object store root: " + config_paths.object_store_root.string() + " (" + fs_error.message() + ")";
        }
        return false;
    }
    std::filesystem::create_directories(config_paths.archive_store_root, fs_error);
    if (fs_error) {
        if (error_out != nullptr) {
            *error_out = "failed creating archive store root: " + config_paths.archive_store_root.string() + " (" + fs_error.message() + ")";
        }
        return false;
    }
    std::filesystem::create_directories(schema_root, fs_error);
    if (fs_error) {
        if (error_out != nullptr) {
            *error_out = "failed creating schema root: " + schema_root.string() + " (" + fs_error.message() + ")";
        }
        return false;
    }

    const MigrationSourceOptions migration_options{
        .source_kind = MigrationSourceKind::Filesystem,
        .filesystem_root = migration_root,
    };

    DBService db_service(config_paths, migration_options);
    std::string startup_error;
    if (!db_service.Start(&startup_error)) {
        if (error_out != nullptr) {
            *error_out = "db service startup failed: " + startup_error;
        }
        return false;
    }

    const std::vector<std::filesystem::path> db_paths{
        config_paths.execution_db_path,
        config_paths.state_db_path,
        config_paths.analysis_db_path,
        config_paths.authoring_db_path,
        config_paths.ui_read_db_path,
        config_paths.archive_db_path,
    };
    db_service.Stop();

    for (const auto& db_path : db_paths) {
        if (!std::filesystem::exists(db_path)) {
            if (error_out != nullptr) {
                *error_out = "database file missing after db service startup: " + db_path.string();
            }
            return false;
        }

        const auto sql_path = schema_root / (db_path.stem().string() + ".sql");
        std::string schema_error;
        if (!ExportSqliteSchema(db_path, sql_path, &schema_error)) {
            if (error_out != nullptr) {
                *error_out = schema_error;
            }
            return false;
        }
    }

    return true;
}
