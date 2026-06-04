#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <sqlite3.h>

#include "../Analysis/IAnalysisDb.h"
#include "../Analysis/SqliteAnalysisDb.h"
#include "../Archive/IArchiveDb.h"
#include "../Archive/SqliteArchiveDb.h"
#include "../Authoring/IAuthoringDb.h"
#include "../Authoring/SqliteAuthoringDb.h"
#include "../Execution/IExecutionDb.h"
#include "../Execution/QueuedExecutionDb.h"
#include "../Execution/Workflow/SqliteExecutionDb.h"
#include "../State/IStateDb.h"
#include "../State/SqliteStateDb.h"
#include "../UIRead/IUiReadDb.h"
#include "../UIRead/SqliteUiReadDb.h"
#include "DbConfigPaths.h"
#include "Migrations/MigrationRunner.h"

namespace simcore::db::core {

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

    simcore::db::IExecutionDb* ExecutionDb();
    simcore::db::execution::workflow::SqliteExecutionDb* RawExecutionDbForValidation();
    simcore::db::IStateDb* StateDb();
    simcore::db::IAnalysisDb* AnalysisDb();
    simcore::db::IAuthoringDb* AuthoringDb();
    simcore::db::IUiReadDb* UiReadDb();
    simcore::db::IArchiveDb* ArchiveDb();

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

    std::unique_ptr<simcore::db::execution::workflow::SqliteExecutionDb> sqlite_execution_db_;
    std::unique_ptr<simcore::db::execution::QueuedExecutionDb> execution_db_;
    std::unique_ptr<simcore::db::state::SqliteStateDb> state_db_;
    std::unique_ptr<simcore::db::analysis::SqliteAnalysisDb> analysis_db_;
    std::unique_ptr<simcore::db::SqliteAuthoringDb> authoring_db_;
    std::unique_ptr<simcore::db::SqliteUiReadDb> ui_read_db_;
    std::unique_ptr<simcore::db::SqliteArchiveDb> archive_db_;
};

} // namespace simcore::db::core
