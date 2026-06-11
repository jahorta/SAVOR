#include "AttachedUiReadProjectionService.h"

#include "UiOutboxRelayCoordinator.h"

#include <algorithm>
#include <utility>

namespace savor::db::uiread::projectors {
namespace {

void SetError(std::string* error_out, std::string message) {
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
}

bool Exec(sqlite3* db, const char* sql, std::string* error_out) {
    char* sqlite_error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &sqlite_error);
    if (rc == SQLITE_OK) {
        return true;
    }

    if (error_out != nullptr) {
        *error_out = sqlite_error != nullptr ? sqlite_error : sqlite3_errmsg(db);
    }

    sqlite3_free(sqlite_error);
    return false;
}

std::filesystem::path NormalizedPath(const std::filesystem::path& path) {
    if (path.empty()) {
        return {};
    }

    std::error_code ec;
    auto absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    return absolute.lexically_normal();
}

bool ContainsPath(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& path) {
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

} // namespace

AttachedUiReadProjectionService::AttachedUiReadProjectionService(AttachedUiReadProjectionConfig config)
    : config_(std::move(config)) {
}

AttachedUiReadProjectionService::~AttachedUiReadProjectionService() {
    Stop();
}

bool AttachedUiReadProjectionService::Start(std::string* error_out) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (running_) {
        return true;
    }
    if (config_.poll_interval <= std::chrono::milliseconds::zero()) {
        SetError(error_out, "UIRead projection poll interval must be positive");
        return false;
    }
    if (!OpenAndAttach(error_out)) {
        CloseConnection();
        return false;
    }

    stopping_ = false;
    running_ = true;
    worker_ = std::thread([this]() { WorkerLoop(); });
    cv_.notify_all();
    return true;
}

void AttachedUiReadProjectionService::Stop() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!running_ && !worker_.joinable()) {
            CloseConnection();
            return;
        }
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(mtx_);
    running_ = false;
    CloseConnection();
}

bool AttachedUiReadProjectionService::IsRunning() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return running_ && !stopping_;
}

bool AttachedUiReadProjectionService::RunOnce(std::string* error_out) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (db_ == nullptr) {
        SetError(error_out, "UIRead projection database is not open");
        ++run_once_count_;
        ++failed_run_once_count_;
        return false;
    }

    const auto started_at = std::chrono::steady_clock::now();
    UiOutboxRelayCoordinator coordinator(db_);
    const bool ok = coordinator.RelayAll(
        "UiReadProjector",
        config_.max_batch_size,
        error_out,
        config_.max_attempts,
        config_.include_archive);
    const auto duration_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at)
            .count());
    ++run_once_count_;
    if (ok) {
        ++succeeded_run_once_count_;
    } else {
        ++failed_run_once_count_;
    }
    last_run_duration_ms_ = duration_ms;
    max_run_duration_ms_ = std::max(max_run_duration_ms_, duration_ms);
    return ok;
}

void AttachedUiReadProjectionService::Wake() {
    cv_.notify_all();
}

AttachedUiReadProjectionTelemetrySnapshot AttachedUiReadProjectionService::SnapshotTelemetry() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return AttachedUiReadProjectionTelemetrySnapshot{
        .running = running_ && !stopping_,
        .run_once_count = run_once_count_,
        .succeeded_run_once_count = succeeded_run_once_count_,
        .failed_run_once_count = failed_run_once_count_,
        .last_run_duration_ms = last_run_duration_ms_,
        .max_run_duration_ms = max_run_duration_ms_,
        .configured_max_batch_size = config_.max_batch_size,
        .configured_max_attempts = config_.max_attempts,
    };
}

bool AttachedUiReadProjectionService::OpenAndAttach(std::string* error_out) {
    if (config_.ui_read_db_path.empty()) {
        SetError(error_out, "UIRead database path is required for projection service");
        return false;
    }

    const int rc = sqlite3_open_v2(
        config_.ui_read_db_path.string().c_str(),
        &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK) {
        SetError(error_out, db_ != nullptr ? sqlite3_errmsg(db_) : "sqlite3_open_v2 failed");
        return false;
    }

    return ConfigureConnection(error_out) && AttachSourceDatabases(error_out);
}

bool AttachedUiReadProjectionService::AttachSourceDatabases(std::string* error_out) {
    const auto ui_path = NormalizedPath(config_.ui_read_db_path);
    std::vector<std::filesystem::path> attached_paths;

    const std::vector<std::filesystem::path> source_paths{
        config_.execution_db_path,
        config_.state_db_path,
        config_.analysis_db_path,
        config_.authoring_db_path,
        config_.archive_db_path,
    };

    int next_schema = 1;
    for (const auto& raw_path : source_paths) {
        const auto path = NormalizedPath(raw_path);
        if (path.empty() || path == ui_path || ContainsPath(attached_paths, path)) {
            continue;
        }

        if (!AttachDatabase(path, "src" + std::to_string(next_schema), error_out)) {
            return false;
        }
        attached_paths.push_back(path);
        ++next_schema;
    }

    return true;
}

bool AttachedUiReadProjectionService::AttachDatabase(
    const std::filesystem::path& db_path,
    const std::string& schema_name,
    std::string* error_out) {
    sqlite3_stmt* st = nullptr;
    const std::string sql = "ATTACH DATABASE ?1 AS " + schema_name + ";";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        SetError(error_out, sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_text(st, 1, db_path.string().c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) {
        SetError(error_out, sqlite3_errmsg(db_));
    }
    sqlite3_finalize(st);
    return ok;
}

bool AttachedUiReadProjectionService::ConfigureConnection(std::string* error_out) {
    if (!Exec(db_, "PRAGMA journal_mode=WAL;", error_out)) {
        return false;
    }
    if (!Exec(db_, "PRAGMA foreign_keys=ON;", error_out)) {
        return false;
    }
    if (!Exec(db_, "PRAGMA busy_timeout=2000;", error_out)) {
        return false;
    }
    return true;
}

void AttachedUiReadProjectionService::WorkerLoop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mtx_);
            if (stopping_) {
                break;
            }
        }

        std::string ignored_error;
        RunOnce(&ignored_error);

        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait_for(lock, config_.poll_interval, [this]() { return stopping_; });
        if (stopping_) {
            break;
        }
    }
}

void AttachedUiReadProjectionService::CloseConnection() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

} // namespace savor::db::uiread::projectors
