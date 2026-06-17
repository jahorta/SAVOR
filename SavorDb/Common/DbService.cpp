#include "DbService.h"

#include <chrono>
#include <filesystem>
#include <utility>

namespace savor::db::core {

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

    sqlite_execution_db_ = std::make_unique<savor::db::execution::workflow::SqliteExecutionDb>(execution_sqlite_);
    if (!sqlite_execution_db_->RequeueInterruptedExecutionJobs(nullptr, error_out)) {
        return fail_start("Failed requeueing interrupted execution jobs: " + (error_out ? *error_out : std::string{}));
    }
    execution_db_ = std::make_unique<savor::db::execution::QueuedExecutionDb>(sqlite_execution_db_.get());
    if (!execution_db_->Start(error_out)) {
        return fail_start("Failed starting Execution queue workers: " + (error_out ? *error_out : std::string{}));
    }

    sqlite_state_db_ = std::make_unique<savor::db::state::SqliteStateDb>(state_sqlite_);
    state_db_ = std::make_unique<savor::db::state::QueuedStateDb>(sqlite_state_db_.get());
    if (!state_db_->Start(error_out)) {
        return fail_start("Failed starting State queue workers: " + (error_out ? *error_out : std::string{}));
    }

    sqlite_analysis_db_ = std::make_unique<savor::db::analysis::SqliteAnalysisDb>(analysis_sqlite_);
    analysis_db_ = std::make_unique<savor::db::analysis::QueuedAnalysisDb>(sqlite_analysis_db_.get());
    if (!analysis_db_->Start(error_out)) {
        return fail_start("Failed starting Analysis queue workers: " + (error_out ? *error_out : std::string{}));
    }

    sqlite_authoring_db_ = std::make_unique<savor::db::SqliteAuthoringDb>(authoring_sqlite_);
    authoring_db_ = std::make_unique<savor::db::QueuedAuthoringDb>(sqlite_authoring_db_.get());
    if (!authoring_db_->Start(error_out)) {
        return fail_start("Failed starting Authoring queue workers: " + (error_out ? *error_out : std::string{}));
    }

    sqlite_ui_read_db_ = std::make_unique<savor::db::SqliteUiReadDb>(ui_read_sqlite_);
    ui_read_db_ = std::make_unique<savor::db::QueuedUiReadDb>(sqlite_ui_read_db_.get());
    if (!ui_read_db_->Start(error_out)) {
        return fail_start("Failed starting UIRead queue workers: " + (error_out ? *error_out : std::string{}));
    }

    sqlite_archive_db_ = std::make_unique<savor::db::SqliteArchiveDb>(archive_sqlite_);
    archive_db_ = std::make_unique<savor::db::QueuedArchiveDb>(sqlite_archive_db_.get());
    if (!archive_db_->Start(error_out)) {
        return fail_start("Failed starting Archive queue workers: " + (error_out ? *error_out : std::string{}));
    }

    ui_read_projection_service_ = std::make_unique<savor::db::uiread::projectors::UiReadProjectionService>(
        savor::db::uiread::projectors::UiReadProjectionConfig{
            .ui_read_db_path = config_paths_.ui_read_db_path,
            .execution_db_path = config_paths_.execution_db_path,
            .state_db_path = config_paths_.state_db_path,
            .analysis_db_path = config_paths_.analysis_db_path,
            .archive_db_path = config_paths_.archive_db_path,
            .max_batch_size = 5000,
            .max_dirty_materialization_batch_size = 1000,
            .max_attempts = 5,
            .poll_interval = std::chrono::milliseconds{ 250 },
        });
    if (!ui_read_projection_service_->Start(error_out)) {
        return fail_start("Failed starting UIRead projection service: " + (error_out ? *error_out : std::string{}));
    }

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

savor::db::IExecutionDb* DBService::ExecutionDb() {
    return execution_db_.get();
}

savor::db::execution::workflow::SqliteExecutionDb* DBService::RawExecutionDbForValidation() {
    return sqlite_execution_db_.get();
}

savor::db::IStateDb* DBService::StateDb() {
    return state_db_.get();
}

savor::db::IAnalysisDb* DBService::AnalysisDb() {
    return analysis_db_.get();
}

savor::db::IAuthoringDb* DBService::AuthoringDb() {
    return authoring_db_.get();
}

savor::db::IUiReadDb* DBService::UiReadDb() {
    return ui_read_db_.get();
}

savor::db::IArchiveDb* DBService::ArchiveDb() {
    return archive_db_.get();
}

bool DBService::RunUiReadProjectionOnce(std::string* error_out) {
    if (ui_read_projection_service_ == nullptr) {
        if (error_out != nullptr) {
            *error_out = "UIRead projection service is not running";
        }
        return false;
    }
    return ui_read_projection_service_->RunOnce(error_out);
}

DBServicePerformanceSnapshot DBService::SnapshotPerformance() const {
    DBServicePerformanceSnapshot snapshot{};
    snapshot.running = running_;
    if (execution_db_ != nullptr) {
        const auto execution = execution_db_->GetTelemetrySnapshot();
        snapshot.databases.push_back({
            .db_context = "Execution",
            .queue = execution.queued,
        });
    }
    if (state_db_ != nullptr) {
        snapshot.databases.push_back({
            .db_context = "State",
            .queue = state_db_->GetTelemetrySnapshot(),
        });
    }
    if (analysis_db_ != nullptr) {
        snapshot.databases.push_back({
            .db_context = "Analysis",
            .queue = analysis_db_->GetTelemetrySnapshot(),
        });
    }
    if (authoring_db_ != nullptr) {
        snapshot.databases.push_back({
            .db_context = "Authoring",
            .queue = authoring_db_->GetTelemetrySnapshot(),
        });
    }
    if (ui_read_db_ != nullptr) {
        snapshot.databases.push_back({
            .db_context = "UiRead",
            .queue = ui_read_db_->GetTelemetrySnapshot(),
        });
    }
    if (archive_db_ != nullptr) {
        snapshot.databases.push_back({
            .db_context = "Archive",
            .queue = archive_db_->GetTelemetrySnapshot(),
        });
    }
    if (ui_read_projection_service_ != nullptr) {
        snapshot.ui_read_projection = ui_read_projection_service_->SnapshotTelemetry();
    }
    return snapshot;
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
    ui_read_projection_service_.reset();
    archive_db_.reset();
    sqlite_archive_db_.reset();
    ui_read_db_.reset();
    sqlite_ui_read_db_.reset();
    authoring_db_.reset();
    sqlite_authoring_db_.reset();
    analysis_db_.reset();
    sqlite_analysis_db_.reset();
    state_db_.reset();
    sqlite_state_db_.reset();
    execution_db_.reset();
    sqlite_execution_db_.reset();
}

} // namespace savor::db::core
