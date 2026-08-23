#include "ResultStagingCleanupService.h"

#include <filesystem>
#include <random>

#include "../../../SavorCore/Utils/Hash.h"

#ifdef _WIN32
#include <Windows.h>
#endif

namespace savor::db::execution::programdb {
namespace {

enum class FileCleanupResult { Deleted, Missing, Retry, Blocked };

bool SafeRelative(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name()
        || path.has_root_directory()) return false;
    for (const auto& part : path) {
        if (part.empty() || part == "." || part == "..") return false;
    }
    return true;
}

bool IsReparsePoint(const std::filesystem::path& path, std::error_code* error) {
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            return false;
        if (error)
            *error = std::error_code(
                static_cast<int>(code), std::system_category());
        return false;
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code ec;
    const auto result = std::filesystem::is_symlink(
        std::filesystem::symlink_status(path, ec));
    if (error) *error = ec;
    return result;
#endif
}

FileCleanupResult CleanupFile(
    const ProgramKindDescriptor& descriptor,
    const ResultStagingCleanupRecord& record,
    std::string* error_out) {
    const std::filesystem::path relative(record.relative_path);
    if (descriptor.result_staging_root.empty() || !SafeRelative(relative)) {
        *error_out = "result staging cleanup path is not safely relative";
        return FileCleanupResult::Blocked;
    }
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(
        descriptor.result_staging_root, ec);
    if (ec) {
        *error_out = "result staging root cannot be resolved: " + ec.message();
        return FileCleanupResult::Retry;
    }
    const auto candidate = (root / relative).lexically_normal();
    if (candidate.lexically_relative(root) != relative.lexically_normal()) {
        *error_out = "result staging cleanup path escapes its registered root";
        return FileCleanupResult::Blocked;
    }
    auto cursor = root;
    for (const auto& part : relative) {
        cursor /= part;
        ec.clear();
        if (IsReparsePoint(cursor, &ec)) {
            *error_out = "result staging cleanup path contains a reparse point";
            return FileCleanupResult::Blocked;
        }
        if (ec) {
            *error_out = "result staging cleanup path inspection failed: "
                + ec.message();
            return FileCleanupResult::Retry;
        }
    }
    ec.clear();
    const bool exists = std::filesystem::exists(candidate, ec);
    if (ec) {
        *error_out = "result staging file existence check failed: "
            + ec.message();
        return FileCleanupResult::Retry;
    }
    if (!exists) return FileCleanupResult::Missing;
    if (!std::filesystem::is_regular_file(candidate, ec) || ec) {
        *error_out = "result staging cleanup target is not a regular file";
        return FileCleanupResult::Blocked;
    }
    const auto size = std::filesystem::file_size(candidate, ec);
    if (ec) {
        *error_out = "result staging file size check failed: " + ec.message();
        return FileCleanupResult::Retry;
    }
    std::string sha;
    try {
        sha = hash::sha256_of_file(candidate.string());
    } catch (const std::exception& ex) {
        *error_out = ex.what();
        return FileCleanupResult::Retry;
    }
    if (size != record.expected_size_bytes
        || sha != record.expected_sha256) {
        *error_out = "result staging file identity changed before cleanup: "
            + record.relative_path;
        return FileCleanupResult::Blocked;
    }
    if (!std::filesystem::remove(candidate, ec) || ec) {
        *error_out = "result staging file deletion failed: " + ec.message();
        return FileCleanupResult::Retry;
    }
    auto parent = candidate.parent_path();
    while (parent != root) {
        ec.clear();
        if (!std::filesystem::is_empty(parent, ec) || ec) break;
        if (!std::filesystem::remove(parent, ec) || ec) break;
        parent = parent.parent_path();
    }
    return FileCleanupResult::Deleted;
}

std::string Token() {
    std::random_device rd;
    return "result-staging-cleanup-" + std::to_string(rd())
        + "-" + std::to_string(rd());
}

bool Applied(ExecutionDbOperationDisposition disposition) {
    return disposition == ExecutionDbOperationDisposition::Applied
        || disposition == ExecutionDbOperationDisposition::AlreadyApplied
        || disposition == ExecutionDbOperationDisposition::Missing;
}

} // namespace

ResultStagingCleanupService::ResultStagingCleanupService(
    IExecutionDb* execution_db,
    const ProgramKindRegistry* registry,
    ResultStagingCleanupConfig config)
    : execution_db_(execution_db), registry_(registry), config_(config),
      cleanup_token_(Token()) {}

ResultStagingCleanupService::~ResultStagingCleanupService() { Stop(); }

bool ResultStagingCleanupService::Start(std::string* error_out) {
    if (running_.load()) return true;
    if (!config_.enabled) return true;
    if (!execution_db_ || !registry_
        || config_.poll_interval <= std::chrono::milliseconds::zero()
        || config_.lease_duration <= std::chrono::milliseconds::zero()) {
        if (error_out)
            *error_out = "result staging cleanup configuration is invalid";
        return false;
    }
    stop_.store(false);
    running_.store(true);
    thread_ = std::thread([this]() { Loop(); });
    if (error_out) error_out->clear();
    return true;
}

void ResultStagingCleanupService::Stop() {
    stop_.store(true);
    wait_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

void ResultStagingCleanupService::Wake() {
    {
        std::lock_guard lock(wait_mutex_);
        ++wake_generation_;
    }
    wait_cv_.notify_all();
}

bool ResultStagingCleanupService::IsRunning() const noexcept {
    return running_.load();
}

ResultStagingCleanupTelemetry
ResultStagingCleanupService::SnapshotTelemetry() const {
    ResultStagingCleanupTelemetry result{};
    result.claimed = claimed_.load();
    result.deleted = deleted_.load();
    result.missing = missing_.load();
    result.retried = retried_.load();
    result.blocked = blocked_.load();
    std::lock_guard lock(error_mutex_);
    result.last_error = last_error_;
    result.last_blocked_error = last_blocked_error_;
    return result;
}

void ResultStagingCleanupService::Loop() {
    while (!stop_.load()) {
        if (CleanOne()) continue;
        std::unique_lock lock(wait_mutex_);
        const auto generation = wake_generation_;
        wait_cv_.wait_for(lock, config_.poll_interval,
            [this, generation]() {
                return stop_.load() || wake_generation_ != generation;
            });
    }
}

bool ResultStagingCleanupService::CleanOne() {
    std::string error;
    const auto claimed = execution_db_->ClaimNextResultStagingCleanup({
        .cleanup_token = cleanup_token_,
        .lease_duration_ms = config_.lease_duration.count(),
    }, &error);
    if (!claimed) {
        if (!error.empty()) RecordError(std::move(error), false);
        return false;
    }
    ++claimed_;
    const auto* descriptor = registry_->Find(claimed->cleanup.program_kind);
    FileCleanupResult file_result = FileCleanupResult::Blocked;
    if (!descriptor)
        error = "result staging cleanup descriptor is unavailable";
    else
        file_result = CleanupFile(*descriptor, claimed->cleanup, &error);
    const auto completion = file_result == FileCleanupResult::Deleted
            || file_result == FileCleanupResult::Missing
        ? ResultStagingCleanupCompletion::Deleted
        : file_result == FileCleanupResult::Blocked
            ? ResultStagingCleanupCompletion::Blocked
            : ResultStagingCleanupCompletion::Retry;
    ExecutionDbOperationDisposition disposition{};
    std::string db_error;
    if (!execution_db_->CompleteResultStagingCleanup({
            .cleanup_id = claimed->cleanup.cleanup_id,
            .cleanup_token = claimed->cleanup_token,
            .completion = completion,
            .cleanup_error = completion == ResultStagingCleanupCompletion::Deleted
                ? std::nullopt : std::optional<std::string>(error),
        }, &disposition, &db_error) || !Applied(disposition)) {
        RecordError(db_error.empty()
            ? "result staging cleanup completion failed" : db_error, false);
        return false;
    }
    if (file_result == FileCleanupResult::Deleted) ++deleted_;
    else if (file_result == FileCleanupResult::Missing) ++missing_;
    else if (file_result == FileCleanupResult::Retry) {
        ++retried_;
        RecordError(std::move(error), false);
        return false;
    } else {
        ++blocked_;
        RecordError(std::move(error), true);
    }
    return true;
}

void ResultStagingCleanupService::RecordError(
    std::string error,
    bool blocked) {
    if (error.empty()) return;
    std::lock_guard lock(error_mutex_);
    last_error_ = error;
    if (blocked) last_blocked_error_ = std::move(error);
}

} // namespace savor::db::execution::programdb
