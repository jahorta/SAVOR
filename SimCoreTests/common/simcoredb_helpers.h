#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include <sqlite3.h>

#include "Common/DbConfigPaths.h"
#include "Common/DbService.h"
#include "Common/Migrations/MigrationRunner.h"
#include "Execution/Workflow/SqliteExecutionDb.h"

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

inline constexpr int kPhase4DedupeTtlHours = 168;
inline constexpr int kPhase4ClaimedJobStagingCleanupHours = 36;
inline constexpr int kPhase4MinDedupeTtlHours = 24;
inline constexpr int kPhase4MaxDedupeTtlHours = 24 * 30;
inline constexpr int kPhase4MinClaimedJobCleanupHours = 6;
inline constexpr int kPhase4MaxClaimedJobCleanupHours = 24 * 7;
inline constexpr double kPhase4CompletionGateMismatchWarnFrequency = 0.005;
inline constexpr double kPhase4CompletionGateMismatchPageFrequency = 0.02;
inline constexpr int kPhase4ReplayLoopWarnCount = 3;
inline constexpr int kPhase4ReplayLoopPageCount = 6;
inline constexpr double kPhase4DedupeGrowthWarnRatio = 1.4;
inline constexpr double kPhase4DedupeGrowthPageRatio = 2.0;

inline std::filesystem::path MakeTempPhase4Dir(const std::string& suffix) {
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const auto dir = std::filesystem::temp_directory_path() / ("simcoretests-phase4-" + suffix + "-" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    return dir;
}

inline simcore::db::DbConfigPaths MakePhase4DbPaths(const std::filesystem::path& base_dir) {
    simcore::db::DbConfigPaths paths{};
    paths.execution_db_path = base_dir / "execution.sqlite";
    paths.state_db_path = base_dir / "state.sqlite";
    paths.analysis_db_path = base_dir / "analysis.sqlite";
    paths.authoring_db_path = base_dir / "authoring.sqlite";
    paths.ui_read_db_path = base_dir / "uiread.sqlite";
    paths.archive_db_path = base_dir / "archive.sqlite";
    paths.object_store_root = base_dir / "object_store";
    paths.archive_store_root = base_dir / "archive_store";
    return paths;
}

inline bool OpenPhase4ExecutionDb(
    const std::string& temp_suffix,
    std::filesystem::path* temp_dir_out,
    std::unique_ptr<simcore::db::core::DBService>* service_out,
    simcore::db::execution::workflow::SqliteExecutionDb** execution_db_out,
    std::string* error_out) {
    using namespace simcore::db::migrations;

    const auto temp_dir = MakeTempPhase4Dir(temp_suffix);
    auto service = std::make_unique<simcore::db::core::DBService>(
        MakePhase4DbPaths(temp_dir),
        MigrationSourceOptions{ .source_kind = MigrationSourceKind::Embedded });
    if (!service->Start(error_out)) {
        std::filesystem::remove_all(temp_dir);
        return false;
    }

    auto* execution_db = dynamic_cast<simcore::db::execution::workflow::SqliteExecutionDb*>(service->ExecutionDb());
    if (execution_db == nullptr || execution_db->WorkflowCommandService() == nullptr || execution_db->WorkflowQueryService() == nullptr) {
        if (error_out != nullptr) {
            *error_out = "execution db services unavailable";
        }
        service->Stop();
        std::filesystem::remove_all(temp_dir);
        return false;
    }

    *temp_dir_out = temp_dir;
    *execution_db_out = execution_db;
    *service_out = std::move(service);
    return true;
}

inline void CleanupPhase4Db(std::unique_ptr<simcore::db::core::DBService>& service, const std::filesystem::path& temp_dir) {
    if (service) {
        service->Stop();
        service.reset();
    }
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
}
