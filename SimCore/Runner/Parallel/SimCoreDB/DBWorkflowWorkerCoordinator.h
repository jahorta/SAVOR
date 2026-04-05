#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../PRTypes.h"
#include "../ProcessWorker.h"
#include "../TSQueue.h"
#include "../../../../SimCoreDB/Execution/IExecutionDb.h"
#include "../../../../SimCoreDB/Execution/Workflow/WorkflowOrchestration.h"
#include "WorkflowCoordinatorBridge.h"
#include "WorkflowIntegrationMode.h"
#include "WorkflowSchedulerAdapter.h"

namespace simcore::runner::parallel::simcoredb {

struct DBWorkflowWorkerCoordinatorConfig {
    size_t desired_workers = 1;
    uint32_t child_launch_timeout_ms = 30000;
    uint32_t controller_sleep_ms = 5;
    std::string worker_exe_path;
    std::string iso_path;
    std::string dolphin_base_dir;
    std::string worker_dir_root;
};

struct WorkflowCoordinatorTelemetry {
    std::int64_t ready_scan_count = 0;
    std::int64_t ready_steps_enqueued = 0;
    std::int64_t last_ready_scan_latency_ms = 0;
    std::int64_t max_ready_queue_depth = 0;
};

class DBWorkflowWorkerCoordinator {
public:
    using ReadyStepPersistFn = std::function<void(const WorkflowReadyStep&, const ScheduledJobSet&)>;
    using BuildJobPayloadFn = std::function<std::optional<simcore::PSJob>(const WorkflowReadyStep&)>;
    using ProgressCallback = std::function<void(const simcore::PRProgress&)>;
    using ResultCallback = std::function<void(const simcore::PRResult&)>;

    DBWorkflowWorkerCoordinator(
        simcore::db::IExecutionDb* execution_db,
        simcore::db::execution::workflow::IWorkflowModeProvider* mode_provider,
        DBWorkflowWorkerCoordinatorConfig worker_cfg,
        CoordinatorIntegrationConfig integration_cfg,
        WorkflowSchedulerAdapter::ScheduleFn workflow_schedule_fn,
        BuildJobPayloadFn build_job_payload_fn = {},
        ReadyStepPersistFn persist_materialization_fn = {});

    ~DBWorkflowWorkerCoordinator();

    void Start();
    void Stop();

    void SetPaused(bool paused);
    bool IsPaused() const;

    void SetWorkflowMaterializationCallback(WorkflowCoordinatorBridge::MaterializationCallback callback);
    void SetWorkflowTerminalCallback(WorkflowCoordinatorBridge::TerminalCallback callback);

    // External path for terminal notifications coming from job/job_set execution.
    bool PublishTerminalJobSet(const TerminalJobSetSignal& signal);

    // Manual injection hook for tests or explicit push-based materialization pipelines.
    void EnqueueReadyStep(const WorkflowReadyStep& step);
    std::optional<ScheduledJobSet> MaterializeWorkflowStep(const WorkflowReadyStep& step);
    bool SendJobToWorker(size_t worker_idx, uint64_t job_id, const simcore::PSJob& job);
    size_t ActiveWorkerCount() const;
    void SetProgressCallback(ProgressCallback callback);
    void SetResultCallback(ResultCallback callback);

    PRStatus SnapshotStatus() const;
    WorkflowCoordinatorTelemetry SnapshotTelemetry() const;

private:
    struct WorkerSlot {
        size_t id = 0;
        std::unique_ptr<simcore::ProcessWorker> worker;
        std::atomic<bool> ready{ false };
        std::optional<uint64_t> in_flight_job_id;
    };

    void CoordinatorLoop();
    void DrainProgressLoop();
    void DrainResultsLoop();
    bool StartWorkerSlot(size_t worker_idx);
    void StopWorkerSlot(WorkerSlot& slot);
    std::optional<size_t> AcquireAvailableWorker();
    void ReleaseWorkerByResult(const simcore::PRResult& result);
    void PollReadyStepsFromDb();
    bool TryDequeueReadyStep(WorkflowReadyStep* step_out);
    std::string ReadyDedupKey(std::int64_t workflow_step_id) const;

    simcore::db::IExecutionDb* execution_db_ = nullptr;
    simcore::db::execution::workflow::IWorkflowModeProvider* mode_provider_ = nullptr;
    DBWorkflowWorkerCoordinatorConfig worker_cfg_{};
    CoordinatorIntegrationConfig integration_cfg_{};
    WorkflowSchedulerAdapter workflow_scheduler_adapter_;
    BuildJobPayloadFn build_job_payload_fn_;
    ReadyStepPersistFn persist_materialization_fn_;
    WorkflowCoordinatorBridge workflow_bridge_;
    ProgressCallback progress_callback_;
    ResultCallback result_callback_;

    std::atomic<bool> stop_{ false };
    std::atomic<bool> paused_{ false };
    std::atomic<uint64_t> epoch_{ 1 };
    std::thread coordinator_thread_;
    std::thread progress_drainer_thread_;
    std::thread results_drainer_thread_;

    mutable std::mutex queue_mtx_;
    std::condition_variable queue_cv_;
    std::deque<WorkflowReadyStep> ready_queue_;
    std::unordered_set<std::string> seen_ready_step_ids_;
    mutable std::mutex workers_mtx_;
    std::vector<std::unique_ptr<WorkerSlot>> workers_;
    size_t rr_worker_cursor_ = 0;
    TSQueue<simcore::PRProgress> progress_q_;
    TSQueue<simcore::PRResult> results_q_;
    size_t materialized_count_ = 0;
    size_t terminal_published_count_ = 0;
    std::atomic<std::int64_t> ready_scan_count_{ 0 };
    std::atomic<std::int64_t> ready_steps_enqueued_{ 0 };
    std::atomic<std::int64_t> last_ready_scan_latency_ms_{ 0 };
    std::atomic<std::int64_t> max_ready_queue_depth_{ 0 };
};

} // namespace simcore::runner::parallel::simcoredb
