#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "../IExecutionDb.h"
#include "../WorkerResultBlobStore.h"
#include "ProgramKindRegistry.h"

namespace savor::db::execution::programdb {

struct ProgramResultProcessorConfig {
    bool enabled = true;
    std::chrono::milliseconds poll_interval{100};
    std::chrono::milliseconds lease_duration{30000};
    std::chrono::milliseconds recovery_interval{1000};
    int recovery_batch_size = 64;
    int max_total_processing_attempts = 1;
};

struct ProgramResultProcessorTelemetry {
    int max_total_processing_attempts = 1;
    std::uint64_t claims = 0;
    std::uint64_t finalized = 0;
    std::uint64_t execution_retries = 0;
    std::uint64_t processing_failures = 0;
    std::uint64_t descriptor_unavailable = 0;
    std::uint64_t lease_authority_lost = 0;
    std::uint64_t recovered_leases = 0;
    std::string last_error;
};

class ProgramResultProcessor {
public:
    using FinalizationCallback =
        std::function<void(
            std::uint64_t commit_sequence,
            std::int64_t workflow_step_id,
            std::int64_t job_id)>;
    using EventLineCallback = std::function<void(const std::string&)>;

    ProgramResultProcessor(
        IExecutionDb* execution_db,
        const ProgramKindRegistry* program_kind_registry,
        WorkerResultBlobStore* blob_store,
        ProgramResultProcessorConfig config = {},
        FinalizationCallback finalization_callback = {},
        EventLineCallback event_line_callback = {});
    ~ProgramResultProcessor();

    ProgramResultProcessor(const ProgramResultProcessor&) = delete;
    ProgramResultProcessor& operator=(const ProgramResultProcessor&) = delete;

    bool Start(std::string* error_out = nullptr);
    void Stop();
    void Wake();

    [[nodiscard]] bool IsRunning() const noexcept;
    void SetMaxTotalProcessingAttempts(int attempts);
    [[nodiscard]] int MaxTotalProcessingAttempts() const noexcept;
    [[nodiscard]] ProgramResultProcessorTelemetry SnapshotTelemetry() const;

private:
    void Loop();
    bool ProcessOne();
    void RecoverExpiredLeases();
    bool Park(
        const ClaimedExecutionFinishedJob& claimed,
        std::string error_code,
        std::string error_text);
    void RecordError(std::string error);

    IExecutionDb* execution_db_ = nullptr;
    const ProgramKindRegistry* program_kind_registry_ = nullptr;
    WorkerResultBlobStore* blob_store_ = nullptr;
    ProgramResultProcessorConfig config_{};
    FinalizationCallback finalization_callback_;
    EventLineCallback event_line_callback_;
    std::string processor_token_;

    std::atomic<int> max_total_processing_attempts_{1};
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    std::uint64_t wake_generation_ = 0;
    std::chrono::steady_clock::time_point next_recovery_at_{};

    std::atomic<std::uint64_t> claims_{0};
    std::atomic<std::uint64_t> finalized_{0};
    std::atomic<std::uint64_t> execution_retries_{0};
    std::atomic<std::uint64_t> processing_failures_{0};
    std::atomic<std::uint64_t> descriptor_unavailable_{0};
    std::atomic<std::uint64_t> lease_authority_lost_{0};
    std::atomic<std::uint64_t> recovered_leases_{0};
    mutable std::mutex error_mutex_;
    std::string last_error_;
};

} // namespace savor::db::execution::programdb
