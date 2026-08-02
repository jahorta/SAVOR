#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../IExecutionDb.h"
#include "../WorkerResultBlobStore.h"
#include "ProgramKindRegistry.h"

namespace savor::db::execution::programdb {

struct ProgramResultProcessorConfig {
    bool enabled = true;
    std::size_t result_claim_batch_size = 32;
    std::size_t result_finalization_batch_size = 32;
    std::chrono::milliseconds result_finalization_delay{5};
};

struct ProgramResultProcessorTelemetry {
    std::uint64_t claims = 0;
    std::uint64_t finalized = 0;
    std::uint64_t execution_retries = 0;
    std::uint64_t processing_failures = 0;
    std::uint64_t descriptor_unavailable = 0;
    std::uint64_t signal_wakes = 0;
    std::uint64_t drain_campaigns = 0;
    std::uint64_t empty_claims = 0;
    std::uint64_t processing_canaries = 0;
    std::uint64_t claim_batches = 0;
    std::uint64_t claim_batch_items = 0;
    std::uint64_t claim_batch_max_size = 0;
    double claim_batch_average_size = 0.0;
    std::uint64_t finalization_batches = 0;
    std::uint64_t finalization_batch_items = 0;
    std::uint64_t finalization_batch_max_size = 0;
    double finalization_batch_average_size = 0.0;
    std::uint64_t finalization_full_flushes = 0;
    std::uint64_t finalization_deadline_flushes = 0;
    std::uint64_t finalization_barrier_flushes = 0;
    std::uint64_t finalization_rollbacks = 0;
    std::size_t finalization_queue_depth = 0;
    std::size_t finalization_queue_high_water = 0;
    std::uint64_t finalization_max_collection_age_ms = 0;
    std::uint64_t startup_blob_resets = 0;
    std::uint64_t startup_program_recoveries = 0;
    std::uint64_t startup_lost_blob_requeues = 0;
    std::uint64_t startup_recovery_canaries = 0;
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
    using CancellationPrecommitCallback = std::function<void(
        std::uint64_t,
        const std::vector<ExecutionCancellationRequestSpec>&)>;
    using CancellationCommittedCallback = std::function<void(
        std::uint64_t,
        const std::vector<CommittedJobCancellation>&)>;

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

    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] ProgramResultProcessorTelemetry SnapshotTelemetry() const;
    void SetCancellationCallbacks(
        CancellationPrecommitCallback precommit,
        CancellationCommittedCallback committed);

private:
    using Clock = std::chrono::steady_clock;

    struct PendingFinalization {
        CommitResultFinalizationCommand command;
        std::vector<std::string> event_lines;
        Clock::time_point enqueued_at{};
        bool force_flush = false;
        std::uint64_t cancellation_hold_id = 0;
        mutable std::mutex mutex;
        std::condition_variable cv;
        bool completed = false;
        bool succeeded = false;
        ResultProcessingReceipt receipt;
        std::string error;
    };
    using PendingFinalizationPtr = std::shared_ptr<PendingFinalization>;

    void Loop();
    void FinalizationLoop();
    bool ProcessDrainCampaign();
    PendingFinalizationPtr ProcessClaimed(
        const ClaimedExecutionFinishedJob& claimed);
    bool EnqueueFinalization(const PendingFinalizationPtr& pending);
    bool WaitForFinalization(const PendingFinalizationPtr& pending);
    bool FlushFinalizationBarrier();
    bool ReconcileInterruptedProcessing(std::string* error_out);
    bool CommitRecoveredDecision(
        const InterruptedResultProcessingJob& interrupted,
        ProgramResultDecision decision,
        std::string* error_out);
    CommitResultFinalizationCommand BuildFinalizationCommand(
        const ExecutionJobRecord& job,
        const ClaimedExecutionFinishedJob* claimed,
        const ProgramResultDecision& decision) const;
    bool RecordFailure(
        std::int64_t job_id,
        std::string error_code,
        std::string error_text);
    void RecordError(std::string error);

    IExecutionDb* execution_db_ = nullptr;
    const ProgramKindRegistry* program_kind_registry_ = nullptr;
    WorkerResultBlobStore* blob_store_ = nullptr;
    ProgramResultProcessorConfig config_{};
    FinalizationCallback finalization_callback_;
    EventLineCallback event_line_callback_;
    CancellationPrecommitCallback cancellation_precommit_callback_;
    CancellationCommittedCallback cancellation_committed_callback_;
    std::atomic<std::uint64_t> cancellation_hold_sequence_{1};

    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::thread finalization_thread_;
    ExecutionWorkAvailabilitySubscription availability_subscription_ = 0;
    mutable std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    ExecutionWorkAvailabilitySnapshot availability_{};
    std::uint64_t wake_generation_ = 0;

    mutable std::mutex finalization_mutex_;
    std::condition_variable finalization_cv_;
    std::condition_variable finalization_drained_cv_;
    std::deque<PendingFinalizationPtr> finalization_queue_;
    bool finalization_active_ = false;
    bool finalization_force_flush_ = false;
    std::size_t finalization_queue_high_water_ = 0;

    std::atomic<std::uint64_t> claims_{0};
    std::atomic<std::uint64_t> finalized_{0};
    std::atomic<std::uint64_t> execution_retries_{0};
    std::atomic<std::uint64_t> processing_failures_{0};
    std::atomic<std::uint64_t> descriptor_unavailable_{0};
    std::atomic<std::uint64_t> signal_wakes_{0};
    std::atomic<std::uint64_t> drain_campaigns_{0};
    std::atomic<std::uint64_t> empty_claims_{0};
    std::atomic<std::uint64_t> processing_canaries_{0};
    std::atomic<std::uint64_t> claim_batches_{0};
    std::atomic<std::uint64_t> claim_batch_items_{0};
    std::atomic<std::uint64_t> claim_batch_max_size_{0};
    std::atomic<std::uint64_t> finalization_batches_{0};
    std::atomic<std::uint64_t> finalization_batch_items_{0};
    std::atomic<std::uint64_t> finalization_batch_max_size_{0};
    std::atomic<std::uint64_t> finalization_full_flushes_{0};
    std::atomic<std::uint64_t> finalization_deadline_flushes_{0};
    std::atomic<std::uint64_t> finalization_barrier_flushes_{0};
    std::atomic<std::uint64_t> finalization_rollbacks_{0};
    std::atomic<std::uint64_t> finalization_max_collection_age_ms_{0};
    std::atomic<std::uint64_t> startup_blob_resets_{0};
    std::atomic<std::uint64_t> startup_program_recoveries_{0};
    std::atomic<std::uint64_t> startup_lost_blob_requeues_{0};
    std::atomic<std::uint64_t> startup_recovery_canaries_{0};
    mutable std::mutex error_mutex_;
    std::string last_error_;
};

} // namespace savor::db::execution::programdb
