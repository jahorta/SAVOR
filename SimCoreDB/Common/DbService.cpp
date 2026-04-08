#include "DbService.h"

#include <utility>

namespace simcore::db::core {

namespace {

bool Exec(sqlite3* db, const char* sql, std::string* error_out) {
    char* sqlite_error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &sqlite_error);
    if (rc == SQLITE_OK) {
        return true;
    }

    if (error_out != nullptr) {
        *error_out = sqlite_error != nullptr ? sqlite_error : "sqlite3_exec failed";
    }

    sqlite3_free(sqlite_error);
    return false;
}

} // namespace

DBService::DBService(
    DbConfigPaths config_paths,
    migrations::MigrationSourceOptions migration_options)
    : config_paths_(std::move(config_paths))
    , migration_options_(std::move(migration_options)) {
}

DBService::~DBService() {
    Stop();
}

bool DBService::Start(std::string* error_out) {
    if (running_) {
        return true;
    }

    auto fail_start = [this, error_out](const std::string& message) {
        if (error_out != nullptr) {
            *error_out = message;
        }
        ResetServices();
        CloseConnections();
        running_ = false;
        return false;
    };

    if (!OpenDatabase(&execution_sqlite_, config_paths_.execution_db_path, error_out)) {
        return fail_start("Failed opening Execution database: " + (error_out ? *error_out : std::string{}));
    }
    if (!OpenDatabase(&state_sqlite_, config_paths_.state_db_path, error_out)) {
        return fail_start("Failed opening State database: " + (error_out ? *error_out : std::string{}));
    }
    if (!OpenDatabase(&analysis_sqlite_, config_paths_.analysis_db_path, error_out)) {
        return fail_start("Failed opening Analysis database: " + (error_out ? *error_out : std::string{}));
    }
    if (!OpenDatabase(&authoring_sqlite_, config_paths_.authoring_db_path, error_out)) {
        return fail_start("Failed opening Authoring database: " + (error_out ? *error_out : std::string{}));
    }
    if (!OpenDatabase(&ui_read_sqlite_, config_paths_.ui_read_db_path, error_out)) {
        return fail_start("Failed opening UIRead database: " + (error_out ? *error_out : std::string{}));
    }
    if (!OpenDatabase(&archive_sqlite_, config_paths_.archive_db_path, error_out)) {
        return fail_start("Failed opening Archive database: " + (error_out ? *error_out : std::string{}));
    }

    if (!ApplyMigrations(execution_sqlite_, migrations::MigrationContext::Execution, error_out)) {
        return fail_start("Failed applying Execution migrations: " + (error_out ? *error_out : std::string{}));
    }
    if (!ApplyMigrations(state_sqlite_, migrations::MigrationContext::State, error_out)) {
        return fail_start("Failed applying State migrations: " + (error_out ? *error_out : std::string{}));
    }
    if (!ApplyMigrations(analysis_sqlite_, migrations::MigrationContext::AnalysisSpine, error_out)
        || !ApplyMigrations(analysis_sqlite_, migrations::MigrationContext::AnalysisSeedProbe, error_out)
        || !ApplyMigrations(analysis_sqlite_, migrations::MigrationContext::AnalysisBattle, error_out)) {
        return fail_start("Failed applying Analysis migrations: " + (error_out ? *error_out : std::string{}));
    }
    if (!ApplyMigrations(authoring_sqlite_, migrations::MigrationContext::Authoring, error_out)) {
        return fail_start("Failed applying Authoring migrations: " + (error_out ? *error_out : std::string{}));
    }
    if (!ApplyMigrations(ui_read_sqlite_, migrations::MigrationContext::UIRead, error_out)) {
        return fail_start("Failed applying UIRead migrations: " + (error_out ? *error_out : std::string{}));
    }
    if (!ApplyMigrations(archive_sqlite_, migrations::MigrationContext::Archive, error_out)) {
        return fail_start("Failed applying Archive migrations: " + (error_out ? *error_out : std::string{}));
    }

    execution_db_ = std::make_unique<simcore::db::execution::workflow::SqliteExecutionDb>(execution_sqlite_);
    state_db_ = std::make_unique<simcore::db::state::SqliteStateDb>(state_sqlite_);
    analysis_db_ = std::make_unique<simcore::db::analysis::SqliteAnalysisDb>(analysis_sqlite_);
    authoring_db_ = std::make_unique<simcore::db::SqliteAuthoringDb>(authoring_sqlite_);
    ui_read_db_ = std::make_unique<simcore::db::SqliteUiReadDb>(ui_read_sqlite_);
    archive_db_ = std::make_unique<simcore::db::SqliteArchiveDb>(archive_sqlite_);

    running_ = true;
    return true;
}

void DBService::Stop() {
    if (!running_ && execution_sqlite_ == nullptr && state_sqlite_ == nullptr && analysis_sqlite_ == nullptr
        && authoring_sqlite_ == nullptr && ui_read_sqlite_ == nullptr && archive_sqlite_ == nullptr) {
        return;
    }

    ResetServices();
    CloseConnections();
    running_ = false;
}

bool DBService::IsRunning() const {
    return running_;
}

simcore::db::IExecutionDb* DBService::ExecutionDb() {
    return execution_db_.get();
}

simcore::db::IStateDb* DBService::StateDb() {
    return state_db_.get();
}

simcore::db::IAnalysisDb* DBService::AnalysisDb() {
    return analysis_db_.get();
}

simcore::db::IAuthoringDb* DBService::AuthoringDb() {
    return authoring_db_.get();
}

simcore::db::IUiReadDb* DBService::UiReadDb() {
    return ui_read_db_.get();
}

simcore::db::IArchiveDb* DBService::ArchiveDb() {
    return archive_db_.get();
}

bool DBService::OpenDatabase(sqlite3** db, const std::filesystem::path& db_path, std::string* error_out) {
    if (db == nullptr) {
        if (error_out != nullptr) {
            *error_out = "Internal error: null sqlite3** receiver";
        }
        return false;
    }

    *db = nullptr;

    if (db_path.empty()) {
        if (error_out != nullptr) {
            *error_out = "Database path is empty";
        }
        return false;
    }

    std::error_code ec;
    const auto parent = db_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error_out != nullptr) {
                *error_out = "Failed creating parent directory: " + parent.string();
            }
            return false;
        }
    }

    const int rc = sqlite3_open_v2(
        db_path.string().c_str(),
        db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = *db != nullptr ? sqlite3_errmsg(*db) : "sqlite3_open_v2 failed";
        }
        if (*db != nullptr) {
            sqlite3_close(*db);
            *db = nullptr;
        }
        return false;
    }

    if (!ConfigureConnection(*db, error_out)) {
        sqlite3_close(*db);
        *db = nullptr;
        return false;
    }

    return true;
}

bool DBService::ApplyMigrations(sqlite3* db, migrations::MigrationContext context, std::string* error_out) const {
    if (db == nullptr) {
        if (error_out != nullptr) {
            *error_out = "Null sqlite3* while applying migrations";
        }
        return false;
    }

    return migrations::ApplyContextMigrations(db, context, migration_options_, error_out);
}

bool DBService::ConfigureConnection(sqlite3* db, std::string* error_out) const {
    if (db == nullptr) {
        if (error_out != nullptr) {
            *error_out = "Null sqlite3* while configuring connection";
        }
        return false;
    }

    if (!Exec(db, "PRAGMA journal_mode=WAL;", error_out)) {
        return false;
    }
    if (!Exec(db, "PRAGMA foreign_keys=ON;", error_out)) {
        return false;
    }
    if (!Exec(db, "PRAGMA busy_timeout=2000;", error_out)) {
        return false;
    }

    return true;
}

void DBService::CloseConnections() {
    if (execution_sqlite_ != nullptr) {
        sqlite3_close(execution_sqlite_);
        execution_sqlite_ = nullptr;
    }
    if (state_sqlite_ != nullptr) {
        sqlite3_close(state_sqlite_);
        state_sqlite_ = nullptr;
    }
    if (analysis_sqlite_ != nullptr) {
        sqlite3_close(analysis_sqlite_);
        analysis_sqlite_ = nullptr;
    }
    if (authoring_sqlite_ != nullptr) {
        sqlite3_close(authoring_sqlite_);
        authoring_sqlite_ = nullptr;
    }
    if (ui_read_sqlite_ != nullptr) {
        sqlite3_close(ui_read_sqlite_);
        ui_read_sqlite_ = nullptr;
    }
    if (archive_sqlite_ != nullptr) {
        sqlite3_close(archive_sqlite_);
        archive_sqlite_ = nullptr;
    }
}

void DBService::ResetServices() {
    archive_db_.reset();
    ui_read_db_.reset();
    authoring_db_.reset();
    analysis_db_.reset();
    state_db_.reset();
    execution_db_.reset();
}

} // namespace simcore::db::core
