#include "ExportCurrentDbSchemas.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "Common/DbService.h"

namespace {

std::string ShellQuote(const std::filesystem::path& value) {
    const std::string text = value.string();
#ifdef _WIN32
    std::string quoted = "\"";
    for (const char ch : text) {
        if (ch == '"') {
            quoted += "\"\"";
        } else {
            quoted += ch;
        }
    }
    quoted += "\"";
#else
    std::string quoted = "'";
    for (const char ch : text) {
        if (ch == '\'') {
            quoted += "'\"'\"'";
        } else {
            quoted += ch;
        }
    }
    quoted += "'";
#endif
    return quoted;
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
        const std::string command =
            "sqlite3 " + ShellQuote(db_path) + " \".schema\" > " + ShellQuote(sql_path);
        auto exit_code = std::system(command.c_str());
        if (exit_code != 0) {
            if (error_out != nullptr) {
                *error_out = std::format("schema export command failed for {}: {}: exit code {}", db_path.string(), command, exit_code).c_str();
            }
            return false;
        }
    }

    return true;
}
