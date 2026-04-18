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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../PRTypes.h"
#include "../ProcessWorker.h"
#include "../TSQueue.h"
#include "../WorkerStatusRegistry.h"
#include "../../../../SimCoreDB/Execution/IExecutionDb.h"
#include "../../../../SimCoreDB/Execution/ProgramDB/ProgramKindRegistry.h"
#include "../../../../SimCoreDB/Execution/Workflow/AdapterChainOrchestrator.h"
#include "../../../../SimCoreDB/Execution/Workflow/WorkflowOrchestration.h"
#include "WorkflowCoordinatorBridge.h"
#include "WorkflowIntegrationMode.h"
#include "WorkflowSchedulerAdapter.h"
#include "StepInputAggregationService.h"
#include "WorkflowDispatchCoordinator.h"
#include "WorkflowMaterializationService.h"

namespace simcore::runner::parallel::simcoredb {

struct DBWorkflowWorkerCoordinatorConfig {
    size_t desired_workers = 1;
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
    std::int64_t input_complete_count = 0;
    std::int64_t input_timeout_count = 0;
    std::int64_t terminal_input_failure_count = 0;
    std::int64_t last_input_latency_ms = 0;
    std::int64_t materialization_count = 0;
    std::int64_t last_materialization_latency_ms = 0;
    std::int64_t max_materialization_latency_ms = 0;
    std::int64_t stale_claim_count = 0;
    std::int64_t dispatch_attempt_count = 0;
    std::int64_t dispatch_miss_count = 0;
    std::int64_t dispatch_miss_rate_basis_points = 0;
    std::int64_t progress_batch_count = 0;
    std::int64_t max_progress_batch_size = 0;
    std::int64_t workflow_created_signal_count = 0;
    std::int64_t materialization_failure_count = 0;
    std::int64_t payload_materialization_failure_count = 0;
};

class DBWorkflowWorkerCoordinator {
public:
    using ReadyStepPersistFn = std::function<void(const WorkflowReadyStep&, const ScheduledJobSet&)>;
    using BuildJobPayloadFn = WorkflowMaterializationService::BuildJobPayloadFn;
    using ClaimJobsFn = WorkflowMaterializationService::ClaimJobsFn;
    using ProgressCallback = std::function<void(const simcore::PRProgress&)>;
    using ResultCallback = std::function<void(const simcore::PRResult&)>;

    DBWorkflowWorkerCoordinator(
        simcore::db::IExecutionDb* execution_db,
        simcore::db::execution::workflow::IWorkflowModeProvider* mode_provider,
        DBWorkflowWorkerCoordinatorConfig worker_cfg,
        CoordinatorIntegrationConfig integration_cfg,
        WorkflowSchedulerAdapter::ScheduleFn workflow_schedule_fn,
        ClaimJobsFn claim_jobs_fn = {},
        BuildJobPayloadFn build_job_payload_fn = {},
        ReadyStepPersistFn persist_materialization_fn = {},
        const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry = nullptr,
        simcore::db::execution::workflow::StepCompletionGateService* step_completion_gate = nullptr);

    ~DBWorkflowWorkerCoordinator();

    void Start();
    void Stop();

    void SetPaused(bool paused);
    bool IsPaused() const;

    void SetWorkflowMaterializationCallback(WorkflowCoordinatorBridge::MaterializationCallback callback);
    void SetWorkflowTerminalCallback(WorkflowCoordinatorBridge::TerminalCallback callback);
    void SetWorkflowCreatedCallback(WorkflowCoordinatorBridge::WorkflowCreatedCallback callback);

    // External path for terminal notifications coming from job/job_set execution.
    bool PublishTerminalJobSet(const TerminalJobSetSignal& signal);
    bool PublishWorkflowCreated(const WorkflowCreatedSignal& signal);

    // Manual injection hook for tests or explicit push-based materialization pipelines.
    void EnqueueReadyStep(const WorkflowReadyStep& step);
    std::optional<ScheduledJobSet> MaterializeWorkflowStep(const WorkflowReadyStep& step);
    bool SendJobToWorker(size_t worker_idx, uint64_t job_id, const simcore::PSJob& job);
    size_t ActiveWorkerCount() const;
    void SetProgressCallback(ProgressCallback callback);
    void SetResultCallback(ResultCallback callback);
    void EnqueueProgressForTest(const simcore::PRProgress& progress);
    void EnqueueResultForTest(const simcore::PRResult& result);

    PRStatus SnapshotStatus() const;
    WorkflowCoordinatorTelemetry SnapshotTelemetry() const;
    std::vector<WorkerSnapshot> SnapshotWorkers() const;

private:
    struct WorkerSlot {
        size_t id = 0;
        std::unique_ptr<simcore::ProcessWorker> worker;
        std::atomic<bool> ready{ false };
        std::optional<uint64_t> in_flight_job_id;
        std::optional<std::string> loaded_savestate_affinity_key;
    };
    struct DispatchableWorkerInfo {
        size_t worker_idx = 0;
        std::optional<std::string> loaded_savestate_affinity_key;
    };
    struct DispatchedJobContext {
        WorkflowReadyStep step;
        std::int64_t job_set_id = 0;
    };

    void CoordinatorLoop();
    void DrainProgressLoop();
    void DrainResultsLoop();
    bool StartWorkerSlot(size_t worker_idx);
    void StopWorkerSlot(WorkerSlot& slot);
    std::vector<DispatchableWorkerInfo> CollectDispatchableWorkers();
    void ReleaseWorkerByResult(const simcore::PRResult& result);
    void PollReadyStepsFromDb();
    bool TryDequeueReadyStep(WorkflowReadyStep* step_out);
    std::string ReadyDedupKey(std::int64_t workflow_step_id) const;
    void EmitAdapterTraceEvent(
        const WorkflowReadyStep& step,
        const std::string& stage,
        const std::string& status,
        std::optional<std::int64_t> job_id,
        std::optional<std::int64_t> job_set_id,
        const std::optional<std::string>& message = std::nullopt) const;
    void MarkDeterministicFailure(
        const WorkflowReadyStep& step,
        std::optional<std::int64_t> job_id,
        std::optional<std::int64_t> job_set_id,
        const std::string& reason) const;
    void EmitWorkflowFailureEvents(
        const WorkflowReadyStep& step,
        const std::string& stage,
        const std::string& reason) const;
    void MaybeTerminalFailStepInStrictSmokeMode(const WorkflowReadyStep& step, const std::string& requested_by) const;

    void RegisterWorkerSlotTelemetry(const WorkerSlot& slot);
    void MarkWorkerError(const WorkerSlot& slot, const std::string& error);

    simcore::db::IExecutionDb* execution_db_ = nullptr;
    simcore::db::execution::workflow::IWorkflowModeProvider* mode_provider_ = nullptr;
    DBWorkflowWorkerCoordinatorConfig worker_cfg_{};
    CoordinatorIntegrationConfig integration_cfg_{};
    WorkflowSchedulerAdapter workflow_scheduler_adapter_;
    StepInputAggregationService input_aggregation_service_;
    ReadyStepPersistFn persist_materialization_fn_;
    WorkflowMaterializationService workflow_materialization_service_;
    WorkflowDispatchCoordinator workflow_dispatch_coordinator_;
    const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry_ = nullptr;
    std::unique_ptr<simcore::db::execution::workflow::StepCompletionGateService> owned_step_completion_gate_;
    simcore::db::execution::workflow::StepCompletionGateService* step_completion_gate_ = nullptr;
    std::unique_ptr<simcore::db::execution::workflow::AdapterChainOrchestrator> adapter_chain_orchestrator_;
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
    std::unordered_set<std::int64_t> seen_workflow_instance_ids_;
    mutable std::mutex workers_mtx_;
    std::vector<std::unique_ptr<WorkerSlot>> workers_;
    std::unordered_map<std::uint64_t, DispatchedJobContext> dispatched_job_context_by_id_;
    size_t rr_worker_cursor_ = 0;
    TSQueue<simcore::PRProgress> progress_q_;
    TSQueue<simcore::PRResult> results_q_;
    size_t materialized_count_ = 0;
    size_t terminal_published_count_ = 0;
    std::atomic<std::int64_t> ready_scan_count_{ 0 };
    std::atomic<std::int64_t> ready_steps_enqueued_{ 0 };
    std::atomic<std::int64_t> last_ready_scan_latency_ms_{ 0 };
    std::atomic<std::int64_t> max_ready_queue_depth_{ 0 };
    std::atomic<std::int64_t> input_complete_count_{ 0 };
    std::atomic<std::int64_t> input_timeout_count_{ 0 };
    std::atomic<std::int64_t> terminal_input_failure_count_{ 0 };
    std::atomic<std::int64_t> last_input_latency_ms_{ 0 };
    std::atomic<std::int64_t> materialization_count_{ 0 };
    std::atomic<std::int64_t> last_materialization_latency_ms_{ 0 };
    std::atomic<std::int64_t> max_materialization_latency_ms_{ 0 };
    std::atomic<std::int64_t> stale_claim_count_{ 0 };
    std::atomic<std::int64_t> dispatch_attempt_count_{ 0 };
    std::atomic<std::int64_t> dispatch_miss_count_{ 0 };
    std::atomic<std::int64_t> progress_batch_count_{ 0 };
    std::atomic<std::int64_t> max_progress_batch_size_{ 0 };
    std::atomic<std::int64_t> workflow_created_signal_count_{ 0 };
    std::atomic<std::int64_t> materialization_failure_count_{ 0 };
    std::atomic<std::int64_t> payload_materialization_failure_count_{ 0 };
    std::atomic<std::int64_t> adapter_input_complete_invocations_{ 0 };
    std::atomic<std::int64_t> adapter_job_claimed_invocations_{ 0 };
    std::atomic<std::int64_t> adapter_job_terminal_invocations_{ 0 };
    WorkerStatusRegistry worker_status_;
};

} // namespace simcore::runner::parallel::simcoredb
