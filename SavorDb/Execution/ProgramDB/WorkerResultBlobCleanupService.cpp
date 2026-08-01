#include "WorkerResultBlobCleanupService.h"

#include <algorithm>
#include <sstream>
#include <utility>
#include <vector>

namespace savor::db::execution::programdb {
namespace {

std::atomic<std::uint64_t> g_cleanup_sequence{1};

std::string MakeCleanupToken(const void* owner) {
    std::ostringstream token;
    token << "worker-result-cleanup-"
          << std::chrono::steady_clock::now().time_since_epoch().count()
          << '-' << reinterpret_cast<std::uintptr_t>(owner)
          << '-' << g_cleanup_sequence.fetch_add(1);
    return token.str();
}

} // namespace

WorkerResultBlobCleanupService::WorkerResultBlobCleanupService(
    IExecutionDb* execution_db,
    WorkerResultBlobStore* blob_store,
    WorkerResultBlobCleanupConfig config)
    : execution_db_(execution_db)
    , blob_store_(blob_store)
    , config_(std::move(config))
    , cleanup_token_(MakeCleanupToken(this)) {
}

WorkerResultBlobCleanupService::~WorkerResultBlobCleanupService() {
    Stop();
}

bool WorkerResultBlobCleanupService::Start(std::string* error_out) {
    if (running_.load()) {
        return true;
    }
    if (!config_.enabled) {
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }
    if (execution_db_ == nullptr || blob_store_ == nullptr) {
        if (error_out != nullptr) {
            *error_out =
                "worker result cleanup requires execution DB and blob store";
        }
        return false;
    }
    if (config_.lease_duration <= std::chrono::milliseconds::zero()) {
        if (error_out != nullptr) {
            *error_out = "worker result cleanup lease must be positive";
        }
        return false;
    }
    if (config_.orphan_sweep_interval
            <= std::chrono::milliseconds::zero()
        || config_.orphan_grace_period
            < std::chrono::milliseconds::zero()) {
        if (error_out != nullptr) {
            *error_out =
                "worker result orphan cleanup intervals are invalid";
        }
        return false;
    }

    stop_.store(false);
    running_.store(true);
    thread_ = std::thread([this]() { Loop(); });
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

void WorkerResultBlobCleanupService::Stop() {
    stop_.store(true);
    wait_cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false);
}

void WorkerResultBlobCleanupService::Wake() {
    {
        std::lock_guard lock(wait_mutex_);
        ++wake_generation_;
    }
    wait_cv_.notify_all();
}

bool WorkerResultBlobCleanupService::IsRunning() const noexcept {
    return running_.load();
}

WorkerResultBlobCleanupTelemetry
WorkerResultBlobCleanupService::SnapshotTelemetry() const {
    WorkerResultBlobCleanupTelemetry telemetry{};
    telemetry.claimed = claimed_.load();
    telemetry.deleted = deleted_.load();
    telemetry.failed = failed_.load();
    telemetry.orphan_candidates = orphan_candidates_.load();
    telemetry.orphan_deleted = orphan_deleted_.load();
    telemetry.orphan_failed = orphan_failed_.load();
    {
        std::lock_guard lock(error_mutex_);
        telemetry.last_error = last_error_;
    }
    return telemetry;
}

void WorkerResultBlobCleanupService::Loop() {
    auto next_orphan_sweep = std::chrono::steady_clock::now();
    while (!stop_.load()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= next_orphan_sweep) {
            (void)SweepOrphans();
            next_orphan_sweep =
                std::chrono::steady_clock::now()
                + config_.orphan_sweep_interval;
        }
        if (CleanOne()) {
            continue;
        }
        now = std::chrono::steady_clock::now();
        auto until_orphan_sweep =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                next_orphan_sweep - now);
        if (until_orphan_sweep
            <= std::chrono::milliseconds::zero()) {
            until_orphan_sweep = std::chrono::milliseconds(1);
        }
        const auto wait_duration = std::min(
            std::max(
                config_.poll_interval,
                std::chrono::milliseconds(1)),
            until_orphan_sweep);
        std::unique_lock lock(wait_mutex_);
        const auto observed_generation = wake_generation_;
        wait_cv_.wait_for(
            lock,
            wait_duration,
            [this, observed_generation]() {
                return stop_.load()
                    || wake_generation_ != observed_generation;
            });
    }
}

bool WorkerResultBlobCleanupService::CleanOne() {
    std::string error;
    const auto claimed = execution_db_->ClaimNextTempBlobCleanup(
        {
            .cleanup_token = cleanup_token_,
            .lease_duration_ms = config_.lease_duration.count(),
        },
        &error);
    if (!claimed.has_value()) {
        if (!error.empty()) {
            RecordError(std::move(error));
        }
        return false;
    }
    ++claimed_;

    const bool removed =
        blob_store_->Remove(claimed->blob.relative_path, &error);
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
    std::string db_error;
    if (!execution_db_->CompleteTempBlobCleanup(
            {
                .temp_blob_id = claimed->blob.temp_blob_id,
                .cleanup_token = claimed->cleanup_token,
                .deleted = removed,
                .cleanup_error = removed
                    ? std::nullopt
                    : std::optional<std::string>(error),
            },
            &disposition,
            &db_error)
        || (disposition != ExecutionDbOperationDisposition::Applied
            && disposition
                != ExecutionDbOperationDisposition::AlreadyApplied)) {
        ++failed_;
        RecordError(
            db_error.empty()
                ? "failed completing worker result blob cleanup"
                : std::move(db_error));
        return false;
    }

    if (removed) {
        ++deleted_;
    } else {
        ++failed_;
        RecordError(std::move(error));
        return false;
    }
    return true;
}

bool WorkerResultBlobCleanupService::SweepOrphans() {
    std::vector<std::string> candidates;
    std::string error;
    if (!blob_store_->ListFilesOlderThan(
            config_.orphan_grace_period,
            &candidates,
            &error)) {
        ++orphan_failed_;
        RecordError(
            error.empty()
                ? "failed listing orphan worker result blobs"
                : std::move(error));
        return false;
    }
    orphan_candidates_.fetch_add(candidates.size());

    bool all_succeeded = true;
    for (const auto& relative_path : candidates) {
        if (stop_.load()) {
            break;
        }
        bool removed = false;
        if (!blob_store_->RemoveIfUnpinnedAndUntracked(
                relative_path,
                [this](
                    std::string_view candidate,
                    bool* tracked_out,
                    std::string* probe_error_out) {
                    return execution_db_->IsTempBlobTracked(
                        candidate,
                        tracked_out,
                        probe_error_out);
                },
                &removed,
                &error)) {
            ++orphan_failed_;
            RecordError(
                error.empty()
                    ? "failed removing orphan worker result blob"
                    : std::move(error));
            all_succeeded = false;
            continue;
        }
        if (removed) {
            ++orphan_deleted_;
        }
    }
    return all_succeeded;
}

void WorkerResultBlobCleanupService::RecordError(std::string error) {
    if (error.empty()) {
        return;
    }
    std::lock_guard lock(error_mutex_);
    last_error_ = std::move(error);
}

} // namespace savor::db::execution::programdb
