#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "../Analysis/IAnalysisDb.h"
#include "../Analysis/QueuedAnalysisDb.h"
#include "../Analysis/SqliteAnalysisDb.h"
#include "../Archive/IArchiveDb.h"
#include "../Archive/QueuedArchiveDb.h"
#include "../Archive/SqliteArchiveDb.h"
#include "../Authoring/IAuthoringDb.h"
#include "../Authoring/QueuedAuthoringDb.h"
#include "../Authoring/SqliteAuthoringDb.h"
#include "../Execution/IExecutionDb.h"
#include "../Execution/QueuedExecutionDb.h"
#include "../Execution/Workflow/SqliteExecutionDb.h"
#include "../State/IStateDb.h"
#include "../State/QueuedStateDb.h"
#include "../State/SqliteStateDb.h"
#include "../UIRead/IUiReadDb.h"
#include "../UIRead/QueuedUiReadDb.h"
#include "../UIRead/Projectors/UiReadProjectionService.h"
#include "../UIRead/SqliteUiReadDb.h"
#include "DbConfigPaths.h"
#include "Migrations/MigrationRunner.h"

namespace savor::db::core {

struct NamedQueuedDbTelemetrySnapshot {
    std::string db_context;
    QueuedDbTelemetrySnapshot queue;
};

struct DBServicePerformanceSnapshot {
    bool running = false;
    std::vector<NamedQueuedDbTelemetrySnapshot> databases;
    uiread::projectors::UiReadProjectionTelemetrySnapshot ui_read_projection;
};

class DBService {
public:
    explicit DBService(
        DbConfigPaths config_paths,
        migrations::MigrationSourceOptions migration_options = {});

    ~DBService();

    DBService(const DBService&) = delete;
    DBService& operator=(const DBService&) = delete;

    bool Start(std::string* error_out = nullptr);
    void Stop();

    [[nodiscard]] bool IsRunning() const;

    savor::db::IExecutionDb* ExecutionDb();
    savor::db::execution::workflow::SqliteExecutionDb* RawExecutionDbForValidation();
    savor::db::IStateDb* StateDb();
    savor::db::IAnalysisDb* AnalysisDb();
    savor::db::IAuthoringDb* AuthoringDb();
    savor::db::IUiReadDb* UiReadDb();
    savor::db::IArchiveDb* ArchiveDb();
    bool RunUiReadProjectionOnce(std::string* error_out = nullptr);
    [[nodiscard]] DBServicePerformanceSnapshot SnapshotPerformance() const;

private:
    bool OpenDatabase(sqlite3** db, const std::filesystem::path& db_path, std::string* error_out);
    bool ApplyMigrations(sqlite3* db, migrations::MigrationContext context, std::string* error_out) const;
    bool ConfigureConnection(sqlite3* db, std::string* error_out) const;
    void CloseConnections();
    void ResetServices();

    DbConfigPaths config_paths_{};
    migrations::MigrationSourceOptions migration_options_{};
    bool running_ = false;

    sqlite3* execution_sqlite_ = nullptr;
    sqlite3* state_sqlite_ = nullptr;
    sqlite3* analysis_sqlite_ = nullptr;
    sqlite3* authoring_sqlite_ = nullptr;
    sqlite3* ui_read_sqlite_ = nullptr;
    sqlite3* archive_sqlite_ = nullptr;

    std::unique_ptr<savor::db::execution::workflow::SqliteExecutionDb> sqlite_execution_db_;
    std::unique_ptr<savor::db::execution::QueuedExecutionDb> execution_db_;
    std::unique_ptr<savor::db::state::SqliteStateDb> sqlite_state_db_;
    std::unique_ptr<savor::db::state::QueuedStateDb> state_db_;
    std::unique_ptr<savor::db::analysis::SqliteAnalysisDb> sqlite_analysis_db_;
    std::unique_ptr<savor::db::analysis::QueuedAnalysisDb> analysis_db_;
    std::unique_ptr<savor::db::SqliteAuthoringDb> sqlite_authoring_db_;
    std::unique_ptr<savor::db::QueuedAuthoringDb> authoring_db_;
    std::unique_ptr<savor::db::SqliteUiReadDb> sqlite_ui_read_db_;
    std::unique_ptr<savor::db::QueuedUiReadDb> ui_read_db_;
    std::unique_ptr<savor::db::uiread::projectors::UiReadProjectionService> ui_read_projection_service_;
    std::unique_ptr<savor::db::SqliteArchiveDb> sqlite_archive_db_;
    std::unique_ptr<savor::db::QueuedArchiveDb> archive_db_;
};

} // namespace savor::db::core
