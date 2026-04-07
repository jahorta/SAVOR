#include "DBWorkflowWorkerCoordinator.h"

#include <chrono>
#include <sstream>
#include <utility>

#include "../../../../SimCoreDB/Execution/Workflow/WorkflowModeProvider.h"

namespace simcore::runner::parallel::simcoredb {

DBWorkflowWorkerCoordinator::DBWorkflowWorkerCoordinator(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::execution::workflow::IWorkflowModeProvider* mode_provider,
    DBWorkflowWorkerCoordinatorConfig worker_cfg,
    CoordinatorIntegrationConfig integration_cfg,
    WorkflowSchedulerAdapter::ScheduleFn workflow_schedule_fn,
    BuildJobPayloadFn build_job_payload_fn,
    ReadyStepPersistFn persist_materialization_fn)
    : execution_db_(execution_db)
    , mode_provider_(mode_provider)
    , worker_cfg_(std::move(worker_cfg))
    , integration_cfg_(integration_cfg)
    , workflow_scheduler_adapter_(std::move(workflow_schedule_fn))
    , input_aggregation_service_(
        StepInputAggregationConfig{},
        [this](
            const WorkflowReadyStep& step,
            const std::string& event_kind,
            const std::optional<std::string>& source_key,
            const std::optional<std::string>& request_id,
            const std::optional<std::string>& message) {
            if (execution_db_ == nullptr || execution_db_->WorkflowCommandService() == nullptr) {
                return;
            }
            std::string error;
            (void)execution_db_->WorkflowCommandService()->AppendStepInputEvent(
                {
                    .workflow_instance_id = step.workflow_instance_id,
                    .workflow_step_id = step.workflow_step_id,
                    .event_kind = event_kind,
                    .source_key = source_key,
                    .request_id = request_id,
                    .message = message,
                    .requested_by = "step_input_aggregation",
                },
                &error);
        })
    , build_job_payload_fn_(std::move(build_job_payload_fn))
    , persist_materialization_fn_(std::move(persist_materialization_fn)) {
}

DBWorkflowWorkerCoordinator::~DBWorkflowWorkerCoordinator() {
    Stop();
}

void DBWorkflowWorkerCoordinator::Start() {
    if (coordinator_thread_.joinable()) {
        return;
    }

    stop_.store(false);

    {
        std::lock_guard<std::mutex> lock(workers_mtx_);
        workers_.clear();
        workers_.reserve(worker_cfg_.desired_workers);
        for (size_t i = 0; i < worker_cfg_.desired_workers; ++i) {
            auto slot = std::make_unique<WorkerSlot>();
            slot->id = i;
            slot->worker = std::make_unique<simcore::ProcessWorker>();
            slot->worker->set_progress_queue(&progress_q_);
            workers_.push_back(std::move(slot));
            StartWorkerSlot(i);
        }
    }

    progress_drainer_thread_ = std::thread([this]() { DrainProgressLoop(); });
    results_drainer_thread_ = std::thread([this]() { DrainResultsLoop(); });
    coordinator_thread_ = std::thread([this]() { CoordinatorLoop(); });
}

void DBWorkflowWorkerCoordinator::Stop() {
    stop_.store(true);
    queue_cv_.notify_all();
    progress_q_.close();
    results_q_.close();

    if (coordinator_thread_.joinable()) {
        coordinator_thread_.join();
    }
    if (progress_drainer_thread_.joinable()) {
        progress_drainer_thread_.join();
    }
    if (results_drainer_thread_.joinable()) {
        results_drainer_thread_.join();
    }

    std::lock_guard<std::mutex> lock(workers_mtx_);
    for (auto& slot : workers_) {
        StopWorkerSlot(*slot);
    }
    workers_.clear();
}

void DBWorkflowWorkerCoordinator::SetPaused(bool paused) {
    paused_.store(paused);
    if (!paused) {
        queue_cv_.notify_all();
    }
}

bool DBWorkflowWorkerCoordinator::IsPaused() const {
    return paused_.load();
}

void DBWorkflowWorkerCoordinator::SetWorkflowMaterializationCallback(WorkflowCoordinatorBridge::MaterializationCallback callback) {
    workflow_bridge_.SetMaterializationCallback(std::move(callback));
}

void DBWorkflowWorkerCoordinator::SetWorkflowTerminalCallback(WorkflowCoordinatorBridge::TerminalCallback callback) {
    workflow_bridge_.SetTerminalCallback(std::move(callback));
}

bool DBWorkflowWorkerCoordinator::PublishTerminalJobSet(const TerminalJobSetSignal& signal) {
    if (!integration_cfg_.workflow_enabled) {
        return false;
    }

    const bool published = workflow_bridge_.NotifyTerminal(signal);
    if (published) {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        ++terminal_published_count_;
    }
    return published;
}

void DBWorkflowWorkerCoordinator::EnqueueReadyStep(const WorkflowReadyStep& step) {
    std::lock_guard<std::mutex> lock(queue_mtx_);
    const auto key = ReadyDedupKey(step.workflow_step_id);
    if (!seen_ready_step_ids_.emplace(key).second) {
        return;
    }
    ready_queue_.push_back(step);
    queue_cv_.notify_one();
}

std::optional<ScheduledJobSet> DBWorkflowWorkerCoordinator::MaterializeWorkflowStep(const WorkflowReadyStep& step) {
    if (!integration_cfg_.workflow_enabled) {
        return std::nullopt;
    }

    const auto scheduled = workflow_scheduler_adapter_.MaterializeReadyStep(step);
    if (execution_db_ && execution_db_->WorkflowCommandService()) {
        std::string error;
        (void)execution_db_->WorkflowCommandService()->MarkStepMaterialized(
            {
                .workflow_step_id = step.workflow_step_id,
                .job_set_id = scheduled.job_set_id,
                .requested_by = "workflow_materialize",
            },
            &error);
    }
    workflow_bridge_.NotifyMaterialized(step.workflow_step_id, scheduled.job_set_id);
    if (persist_materialization_fn_) {
        persist_materialization_fn_(step, scheduled);
    }

    std::lock_guard<std::mutex> lock(queue_mtx_);
    ++materialized_count_;
    ++epoch_;
    return scheduled;
}

bool DBWorkflowWorkerCoordinator::SendJobToWorker(size_t worker_idx, uint64_t job_id, const simcore::PSJob& job) {
    std::lock_guard<std::mutex> lock(workers_mtx_);
    if (worker_idx >= workers_.size()) {
        return false;
    }

    auto& slot = *workers_[worker_idx];
    if (!slot.ready.load() || !slot.worker->try_acquire_slot()) {
        return false;
    }

    if (!slot.worker->send_job(job_id, epoch_.load(), job)) {
        slot.worker->release_slot();
        return false;
    }

    slot.in_flight_job_id = job_id;
    return true;
}

size_t DBWorkflowWorkerCoordinator::ActiveWorkerCount() const {
    std::lock_guard<std::mutex> lock(workers_mtx_);
    return workers_.size();
}

void DBWorkflowWorkerCoordinator::SetProgressCallback(ProgressCallback callback) {
    progress_callback_ = std::move(callback);
}

void DBWorkflowWorkerCoordinator::SetResultCallback(ResultCallback callback) {
    result_callback_ = std::move(callback);
}

PRStatus DBWorkflowWorkerCoordinator::SnapshotStatus() const {
    PRStatus status{};
    status.epoch = epoch_.load();
    status.workers = ActiveWorkerCount();

    std::lock_guard<std::mutex> lock(queue_mtx_);
    status.queued_jobs = ready_queue_.size();
    status.running_workers = materialized_count_;
    status.pending_start_workers = terminal_published_count_;
    status.ready_workers = status.workers;
    return status;
}

WorkflowCoordinatorTelemetry DBWorkflowWorkerCoordinator::SnapshotTelemetry() const {
    WorkflowCoordinatorTelemetry telemetry{};
    telemetry.ready_scan_count = ready_scan_count_.load();
    telemetry.ready_steps_enqueued = ready_steps_enqueued_.load();
    telemetry.last_ready_scan_latency_ms = last_ready_scan_latency_ms_.load();
    telemetry.max_ready_queue_depth = max_ready_queue_depth_.load();
    telemetry.input_complete_count = input_complete_count_.load();
    telemetry.input_timeout_count = input_timeout_count_.load();
    telemetry.terminal_input_failure_count = terminal_input_failure_count_.load();
    telemetry.last_input_latency_ms = last_input_latency_ms_.load();
    return telemetry;
}

void DBWorkflowWorkerCoordinator::CoordinatorLoop() {
    while (!stop_.load()) {
        if (paused_.load()) {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(50));
            continue;
        }

        PollReadyStepsFromDb();

        WorkflowReadyStep step;
        if (!TryDequeueReadyStep(&step)) {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(50));
            continue;
        }

        if (!integration_cfg_.workflow_enabled) {
            continue;
        }

        const auto aggregation = input_aggregation_service_.Evaluate(
            step,
            std::chrono::steady_clock::now(),
            true);
        if (!aggregation.input_complete) {
            if (aggregation.timed_out) {
                ++input_timeout_count_;
            }
            if (aggregation.terminal_failure_ready) {
                ++terminal_input_failure_count_;
                if (execution_db_ && execution_db_->WorkflowCommandService()) {
                    std::string error;
                    (void)execution_db_->WorkflowCommandService()->MarkStepTerminal(
                        {
                            .workflow_step_id = step.workflow_step_id,
                            .terminal_state = "FAILED",
                            .requested_by = "step_input_aggregation_timeout",
                        },
                        &error);
                }
                continue;
            }
            EnqueueReadyStep(step);
            continue;
        }

        ++input_complete_count_;
        if (aggregation.input_latency_ms.has_value()) {
            last_input_latency_ms_.store(*aggregation.input_latency_ms);
        }

        const auto scheduled = workflow_scheduler_adapter_.MaterializeReadyStep(step);
        if (execution_db_ && execution_db_->WorkflowCommandService()) {
            std::string error;
            (void)execution_db_->WorkflowCommandService()->MarkStepMaterialized(
                {
                    .workflow_step_id = step.workflow_step_id,
                    .job_set_id = scheduled.job_set_id,
                    .requested_by = "workflow_materialize",
                },
                &error);
        }
        workflow_bridge_.NotifyMaterialized(step.workflow_step_id, scheduled.job_set_id);
        if (persist_materialization_fn_) {
            persist_materialization_fn_(step, scheduled);
        }

        if (build_job_payload_fn_) {
            const auto payload = build_job_payload_fn_(step);
            if (payload.has_value()) {
                const auto worker_idx = AcquireAvailableWorker();
                if (worker_idx.has_value()) {
                    (void)SendJobToWorker(*worker_idx, static_cast<uint64_t>(step.workflow_step_id), *payload);
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(queue_mtx_);
            ++materialized_count_;
            ++epoch_;
        }
    }
}

void DBWorkflowWorkerCoordinator::DrainProgressLoop() {
    simcore::PRProgress progress;
    while (progress_q_.pop_wait(progress)) {
        if (progress_callback_) {
            progress_callback_(progress);
        }
    }
}

void DBWorkflowWorkerCoordinator::DrainResultsLoop() {
    simcore::PRResult result;
    while (results_q_.pop_wait(result)) {
        ReleaseWorkerByResult(result);
        if (result_callback_) {
            result_callback_(result);
        }
    }
}

bool DBWorkflowWorkerCoordinator::StartWorkerSlot(size_t worker_idx) {
    if (worker_idx >= workers_.size()) {
        return false;
    }

    auto& slot = *workers_[worker_idx];
    simcore::ProcStartParams ps{};
    ps.worker_id = worker_idx;
    ps.exe_path = worker_cfg_.worker_exe_path;
    ps.iso_path = worker_cfg_.iso_path;
    ps.dolphin_base_dir = worker_cfg_.dolphin_base_dir;

    std::ostringstream user_dir;
    user_dir << worker_cfg_.worker_dir_root << "\\workflow-worker-" << worker_idx << "\\User";
    ps.user_dir = user_dir.str();
    ps.vm_control = true;

    if (!slot.worker->start(ps, &results_q_)) {
        slot.ready.store(false);
        return false;
    }

    slot.ready.store(slot.worker->wait_ready(worker_cfg_.child_launch_timeout_ms));
    return slot.ready.load();
}

void DBWorkflowWorkerCoordinator::StopWorkerSlot(WorkerSlot& slot) {
    slot.ready.store(false);
    slot.in_flight_job_id.reset();
    if (slot.worker) {
        slot.worker->stop();
    }
}

std::optional<size_t> DBWorkflowWorkerCoordinator::AcquireAvailableWorker() {
    std::lock_guard<std::mutex> lock(workers_mtx_);
    if (workers_.empty()) {
        return std::nullopt;
    }

    for (size_t i = 0; i < workers_.size(); ++i) {
        const size_t idx = (rr_worker_cursor_ + i) % workers_.size();
        auto& slot = *workers_[idx];
        if (!slot.ready.load()) {
            continue;
        }
        if (slot.in_flight_job_id.has_value()) {
            continue;
        }
        rr_worker_cursor_ = (idx + 1) % workers_.size();
        return idx;
    }

    return std::nullopt;
}

void DBWorkflowWorkerCoordinator::ReleaseWorkerByResult(const simcore::PRResult& result) {
    std::lock_guard<std::mutex> lock(workers_mtx_);
    if (result.worker_id >= workers_.size()) {
        return;
    }

    auto& slot = *workers_[result.worker_id];
    slot.in_flight_job_id.reset();
    if (slot.worker) {
        slot.worker->release_slot();
    }
}

void DBWorkflowWorkerCoordinator::PollReadyStepsFromDb() {
    const auto t0 = std::chrono::steady_clock::now();
    if (execution_db_ == nullptr || execution_db_->WorkflowQueryService() == nullptr) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        last_ready_scan_latency_ms_.store(static_cast<std::int64_t>(elapsed));
        ++ready_scan_count_;
        return;
    }

    if (!integration_cfg_.workflow_enabled) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        last_ready_scan_latency_ms_.store(static_cast<std::int64_t>(elapsed));
        ++ready_scan_count_;
        return;
    }

    constexpr std::size_t kReadyStepScanLimit = 2048;
    const auto ready_steps = execution_db_->WorkflowQueryService()->ListReadySteps(kReadyStepScanLimit);
    for (const auto& step : ready_steps) {
        EnqueueReadyStep(WorkflowReadyStep{
            .workflow_instance_id = step.workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .step_key = step.step_key,
            .step_kind = step.step_kind,
            .priority = step.priority,
        });
        ++ready_steps_enqueued_;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        const auto depth = static_cast<std::int64_t>(ready_queue_.size());
        const auto prev = max_ready_queue_depth_.load();
        if (depth > prev) {
            max_ready_queue_depth_.store(depth);
        }
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    last_ready_scan_latency_ms_.store(static_cast<std::int64_t>(elapsed));
    ++ready_scan_count_;
}

bool DBWorkflowWorkerCoordinator::TryDequeueReadyStep(WorkflowReadyStep* step_out) {
    std::lock_guard<std::mutex> lock(queue_mtx_);
    if (ready_queue_.empty()) {
        return false;
    }

    *step_out = ready_queue_.front();
    ready_queue_.pop_front();
    seen_ready_step_ids_.erase(ReadyDedupKey(step_out->workflow_step_id));
    return true;
}

std::string DBWorkflowWorkerCoordinator::ReadyDedupKey(std::int64_t workflow_step_id) const {
    return std::to_string(workflow_step_id);
}

} // namespace simcore::runner::parallel::simcoredb
