#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "../IExecutionDb.h"
#include "ProgramKindRegistry.h"

namespace savor::db::execution::programdb {

struct ResultStagingCleanupConfig {
    bool enabled = true;
    std::chrono::milliseconds poll_interval{500};
    std::chrono::milliseconds lease_duration{30000};
};

struct ResultStagingCleanupTelemetry {
    std::uint64_t claimed = 0;
    std::uint64_t deleted = 0;
    std::uint64_t missing = 0;
    std::uint64_t retried = 0;
    std::uint64_t blocked = 0;
    std::string last_error;
    std::string last_blocked_error;
};

class ResultStagingCleanupService {
public:
    ResultStagingCleanupService(
        IExecutionDb* execution_db,
        const ProgramKindRegistry* registry,
        ResultStagingCleanupConfig config = {});
    ~ResultStagingCleanupService();
    ResultStagingCleanupService(const ResultStagingCleanupService&) = delete;
    ResultStagingCleanupService& operator=(
        const ResultStagingCleanupService&) = delete;

    bool Start(std::string* error_out = nullptr);
    void Stop();
    void Wake();
    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] ResultStagingCleanupTelemetry SnapshotTelemetry() const;

private:
    void Loop();
    bool CleanOne();
    void RecordError(std::string error, bool blocked);

    IExecutionDb* execution_db_ = nullptr;
    const ProgramKindRegistry* registry_ = nullptr;
    ResultStagingCleanupConfig config_{};
    std::string cleanup_token_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    std::uint64_t wake_generation_ = 0;
    std::atomic<std::uint64_t> claimed_{0};
    std::atomic<std::uint64_t> deleted_{0};
    std::atomic<std::uint64_t> missing_{0};
    std::atomic<std::uint64_t> retried_{0};
    std::atomic<std::uint64_t> blocked_{0};
    mutable std::mutex error_mutex_;
    std::string last_error_;
    std::string last_blocked_error_;
};

} // namespace savor::db::execution::programdb
