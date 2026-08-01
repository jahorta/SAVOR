#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <string>
#include <thread>

#include "../IExecutionDb.h"
#include "../WorkerResultBlobStore.h"

namespace savor::db::execution::programdb {

struct WorkerResultBlobCleanupConfig {
    bool enabled = true;
    std::chrono::milliseconds poll_interval{500};
    std::chrono::milliseconds lease_duration{30000};
    std::chrono::milliseconds orphan_sweep_interval{30000};
    std::chrono::milliseconds orphan_grace_period{300000};
};

struct WorkerResultBlobCleanupTelemetry {
    std::uint64_t claimed = 0;
    std::uint64_t deleted = 0;
    std::uint64_t failed = 0;
    std::uint64_t orphan_candidates = 0;
    std::uint64_t orphan_deleted = 0;
    std::uint64_t orphan_failed = 0;
    std::string last_error;
};

class WorkerResultBlobCleanupService {
public:
    WorkerResultBlobCleanupService(
        IExecutionDb* execution_db,
        WorkerResultBlobStore* blob_store,
        WorkerResultBlobCleanupConfig config = {});
    ~WorkerResultBlobCleanupService();

    WorkerResultBlobCleanupService(
        const WorkerResultBlobCleanupService&) = delete;
    WorkerResultBlobCleanupService& operator=(
        const WorkerResultBlobCleanupService&) = delete;

    bool Start(std::string* error_out = nullptr);
    void Stop();
    void Wake();

    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] WorkerResultBlobCleanupTelemetry SnapshotTelemetry() const;

private:
    void Loop();
    bool CleanOne();
    bool SweepOrphans();
    void RecordError(std::string error);

    IExecutionDb* execution_db_ = nullptr;
    WorkerResultBlobStore* blob_store_ = nullptr;
    WorkerResultBlobCleanupConfig config_{};
    std::string cleanup_token_;

    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    std::uint64_t wake_generation_ = 0;

    std::atomic<std::uint64_t> claimed_{0};
    std::atomic<std::uint64_t> deleted_{0};
    std::atomic<std::uint64_t> failed_{0};
    std::atomic<std::uint64_t> orphan_candidates_{0};
    std::atomic<std::uint64_t> orphan_deleted_{0};
    std::atomic<std::uint64_t> orphan_failed_{0};
    mutable std::mutex error_mutex_;
    std::string last_error_;
};

} // namespace savor::db::execution::programdb
