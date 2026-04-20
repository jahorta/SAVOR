#include "DBWorkflowWorkerCoordinator.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>

#include "../../../../SimCoreDB/Execution/Jobs/JobEventOrchestration.h"
#include "../../../../SimCoreDB/Execution/Workflow/WorkflowModeProvider.h"

namespace simcore::runner::parallel::simcoredb {

DBWorkflowWorkerCoordinator::DBWorkflowWorkerCoordinator(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::execution::workflow::IWorkflowModeProvider* mode_provider,
    DBWorkflowWorkerCoordinatorConfig worker_cfg,
    CoordinatorIntegrationConfig integration_cfg,
    WorkflowSchedulerAdapter::ScheduleFn workflow_schedule_fn,
    ClaimJobsFn claim_jobs_fn,
    BuildJobPayloadFn build_job_payload_fn,
    ReadyStepPersistFn persist_materialization_fn,
    const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    simcore::db::execution::workflow::StepCompletionGateService* step_completion_gate)
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
    , persist_materialization_fn_(std::move(persist_materialization_fn))
    , workflow_materialization_service_(
        &workflow_scheduler_adapter_,
        persist_materialization_fn_,
        [this](const WorkflowReadyStep& step, const ScheduledJobSet& scheduled) {
            if (execution_db_ && execution_db_->WorkflowCommandService()) {
                std::string error;
                const bool marked = execution_db_->WorkflowCommandService()->MarkStepMaterialized(
                    {
                        .workflow_step_id = step.workflow_step_id,
                        .job_set_id = scheduled.job_set_id,
                        .requested_by = "workflow_materialize",
                    },
                    &error);
                if (!marked) {
                    ++materialization_failure_count_;
                    EmitWorkflowFailureEvents(
                        step,
                        "MarkStepMaterialized",
                        error.empty() ? "unknown error" : error);
                    MaybeTerminalFailStepInStrictSmokeMode(step, "workflow_materialize_strict_smoke");
                }
            }
            workflow_bridge_.NotifyMaterialized(step.workflow_step_id, scheduled.job_set_id);
        })
    , job_materialization_service_(
        std::move(claim_jobs_fn),
        std::move(build_job_payload_fn))
    , workflow_dispatch_coordinator_(
        &job_materialization_service_,
        [this](size_t worker_idx, const ClaimedJobRecord& claimed_job) {
            const auto job_id = claimed_job.job_id;
            if (adapter_chain_orchestrator_) {
                simcore::db::execution::workflow::AdapterChainTrace trace{};
                (void)adapter_chain_orchestrator_->OnJobClaimed(claimed_job.step.step_kind, job_id, &trace);
                ++adapter_job_claimed_invocations_;
                EmitAdapterTraceEvent(claimed_job.step, "OnJobClaimed", "invoked", job_id, claimed_job.job_set_id);
            }
            if (!claimed_job.payload.has_value()) {
                return false;
            }
            if (!SendJobToWorker(worker_idx, static_cast<std::uint64_t>(job_id), *claimed_job.payload)) {
                return false;
            }
            std::lock_guard<std::mutex> worker_lock(workers_mtx_);
            if (worker_idx < workers_.size()) {
                workers_[worker_idx]->loaded_savestate_affinity_key = claimed_job.affinity.savestate_affinity_key;
            }
            dispatched_job_context_by_id_[static_cast<std::uint64_t>(job_id)] = DispatchedJobContext{
                .step = claimed_job.step,
                .job_set_id = claimed_job.job_set_id,
            };
            return true;
        })
    , program_kind_registry_(program_kind_registry) {
    if (step_completion_gate != nullptr) {
        step_completion_gate_ = step_completion_gate;
    } else {
        owned_step_completion_gate_ = std::make_unique<simcore::db::execution::workflow::StepCompletionGateService>();
        step_completion_gate_ = owned_step_completion_gate_.get();
    }
    if (program_kind_registry_ != nullptr) {
        adapter_chain_orchestrator_ = std::make_unique<simcore::db::execution::workflow::AdapterChainOrchestrator>(
            program_kind_registry_,
            step_completion_gate_);
    }
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
        std::lock_guard<std::mutex> lock(queue_mtx_);
        seen_workflow_instance_ids_.clear();
    }

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

void DBWorkflowWorkerCoordinator::SetWorkflowCreatedCallback(WorkflowCoordinatorBridge::WorkflowCreatedCallback callback) {
    workflow_bridge_.SetWorkflowCreatedCallback(std::move(callback));
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

bool DBWorkflowWorkerCoordinator::PublishWorkflowCreated(const WorkflowCreatedSignal& signal) {
    if (!integration_cfg_.workflow_enabled) {
        return false;
    }

    const bool published = workflow_bridge_.NotifyWorkflowCreated(signal);
    if (published) {
        ++workflow_created_signal_count_;
        queue_cv_.notify_one();
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
    if (!step.input_ref_id.has_value()) {
        EmitWorkflowFailureEvents(step, "OnInputComplete", "missing input_ref_id");
        MaybeTerminalFailStepInStrictSmokeMode(step, "workflow_materialize_missing_input_ref");
        return std::nullopt;
    }

    if (adapter_chain_orchestrator_) {
        simcore::db::execution::workflow::AdapterChainTrace trace{};
        (void)adapter_chain_orchestrator_->OnInputComplete(step.step_kind, *step.input_ref_id, &trace);
        ++adapter_input_complete_invocations_;
        EmitAdapterTraceEvent(step, "OnInputComplete", "invoked", std::nullopt, std::nullopt);
    }

    const auto scheduled = workflow_materialization_service_.MaterializeWorkflowStep(step);
    if (!scheduled.has_value()) {
        return std::nullopt;
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
        MarkWorkerError(slot, "send_job failed");
        return false;
    }

    slot.in_flight_job_id = job_id;
    worker_status_.SetCurrentJob(static_cast<std::int64_t>(slot.id), static_cast<std::int64_t>(job_id), std::nullopt);
    worker_status_.UpdateState(static_cast<std::int64_t>(slot.id), WorkerStateKind::Running);
    worker_status_.RecordHeartbeat(static_cast<std::int64_t>(slot.id));
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

void DBWorkflowWorkerCoordinator::EnqueueProgressForTest(const simcore::PRProgress& progress) {
    progress_q_.push(progress);
}

void DBWorkflowWorkerCoordinator::EnqueueResultForTest(const simcore::PRResult& result) {
    results_q_.push(result);
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
    telemetry.materialization_count = materialization_count_.load();
    telemetry.last_materialization_latency_ms = last_materialization_latency_ms_.load();
    telemetry.max_materialization_latency_ms = max_materialization_latency_ms_.load();
    telemetry.stale_claim_count = stale_claim_count_.load();
    telemetry.dispatch_attempt_count = dispatch_attempt_count_.load();
    telemetry.dispatch_miss_count = dispatch_miss_count_.load();
    const auto attempts = telemetry.dispatch_attempt_count;
    telemetry.dispatch_miss_rate_basis_points = attempts <= 0
        ? 0
        : static_cast<std::int64_t>((telemetry.dispatch_miss_count * 10000) / attempts);
    telemetry.progress_batch_count = progress_batch_count_.load();
    telemetry.max_progress_batch_size = max_progress_batch_size_.load();
    telemetry.workflow_created_signal_count = workflow_created_signal_count_.load();
    telemetry.materialization_failure_count = materialization_failure_count_.load();
    telemetry.payload_materialization_failure_count = payload_materialization_failure_count_.load();
    return telemetry;
}

std::vector<WorkerSnapshot> DBWorkflowWorkerCoordinator::SnapshotWorkers() const {
    std::lock_guard<std::mutex> lock(workers_mtx_);
    return worker_status_.GetClusterSnapshot();
}

void DBWorkflowWorkerCoordinator::CoordinatorLoop() {
    while (!stop_.load()) {
        if (paused_.load()) {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(50));
            continue;
        }

        PollReadyStepsFromDb();
        const auto worker_target = ActiveWorkerCount();
        const auto buffered = job_materialization_service_.CountBufferedJobs();
        if (buffered < worker_target) {
            const auto claim_budget = worker_target - buffered;
            (void)job_materialization_service_.ClaimJobs(claim_budget, std::chrono::steady_clock::now());
        }
        (void)job_materialization_service_.MaterializeClaimedJobPayload(std::chrono::steady_clock::now());
        for (const auto& failed_payload : job_materialization_service_.ListByState(ClaimedJobLifecycleState::PayloadMaterialized)) {
            ++payload_materialization_failure_count_;
            std::ostringstream message;
            message << "job_id=" << failed_payload.job_id << ";job_set_id=" << failed_payload.job_set_id
                    << ";reason=build_payload returned empty";
            EmitWorkflowFailureEvents(failed_payload.step, "BuildClaimedPayload", message.str());
            MaybeTerminalFailStepInStrictSmokeMode(failed_payload.step, "workflow_payload_materialize_strict_smoke");
            (void)job_materialization_service_.AbandonClaim(failed_payload.job_id);
        }
        {
            const auto dispatchable_workers = CollectDispatchableWorkers();
            for (const auto& worker : dispatchable_workers) {
                ++dispatch_attempt_count_;
                const bool dispatched = workflow_dispatch_coordinator_.DispatchNextEligibleForWorker(
                    worker.worker_idx,
                    worker.loaded_savestate_affinity_key,
                    std::chrono::steady_clock::now());
                if (!dispatched) {
                    ++dispatch_miss_count_;
                }
            }
        }
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

        if (adapter_chain_orchestrator_) {
            if (!step.input_ref_id.has_value()) {
                EmitWorkflowFailureEvents(step, "OnInputComplete", "missing input_ref_id");
                MaybeTerminalFailStepInStrictSmokeMode(step, "workflow_materialize_missing_input_ref");
                continue;
            }
            simcore::db::execution::workflow::AdapterChainTrace trace{};
            const auto persisted = adapter_chain_orchestrator_->OnInputComplete(step.step_kind, *step.input_ref_id, &trace);
            ++adapter_input_complete_invocations_;
            EmitAdapterTraceEvent(
                step,
                "OnInputComplete",
                "invoked",
                std::nullopt,
                std::nullopt,
                persisted.has_value() ? std::optional<std::string>("program_ref_id=" + std::to_string(persisted->persistence.program_ref_id)) : std::nullopt);
        }

        const auto materialize_started = std::chrono::steady_clock::now();
        (void)workflow_materialization_service_.MaterializeWorkflowStep(step);
        ++materialization_count_;
        const auto materialization_latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - materialize_started).count();
        last_materialization_latency_ms_.store(static_cast<std::int64_t>(materialization_latency_ms));
        {
            const auto prev_max = max_materialization_latency_ms_.load();
            if (materialization_latency_ms > prev_max) {
                max_materialization_latency_ms_.store(static_cast<std::int64_t>(materialization_latency_ms));
            }
        }
        (void)job_materialization_service_.MaterializeClaimedJobPayload(std::chrono::steady_clock::now());
        for (const auto& failed_payload : job_materialization_service_.ListByState(ClaimedJobLifecycleState::PayloadMaterialized)) {
            ++payload_materialization_failure_count_;
            std::ostringstream message;
            message << "job_id=" << failed_payload.job_id << ";job_set_id=" << failed_payload.job_set_id
                    << ";reason=build_payload returned empty";
            EmitWorkflowFailureEvents(failed_payload.step, "BuildClaimedPayload", message.str());
            MaybeTerminalFailStepInStrictSmokeMode(failed_payload.step, "workflow_payload_materialize_strict_smoke");
            (void)job_materialization_service_.AbandonClaim(failed_payload.job_id);
        }
        {
            const auto dispatchable_workers = CollectDispatchableWorkers();
            for (const auto& worker : dispatchable_workers) {
                ++dispatch_attempt_count_;
                const bool dispatched = workflow_dispatch_coordinator_.DispatchNextEligibleForWorker(
                    worker.worker_idx,
                    worker.loaded_savestate_affinity_key,
                    std::chrono::steady_clock::now());
                if (!dispatched) {
                    ++dispatch_miss_count_;
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
    constexpr std::size_t kMaxBatchSize = 64;
    simcore::PRProgress progress;
    while (progress_q_.pop_wait(progress)) {
        std::vector<simcore::PRProgress> batch;
        batch.reserve(kMaxBatchSize);
        batch.push_back(progress);
        simcore::PRProgress next;
        while (batch.size() < kMaxBatchSize && progress_q_.try_pop(next)) {
            batch.push_back(std::move(next));
        }
        ++progress_batch_count_;
        const auto batch_size = static_cast<std::int64_t>(batch.size());
        const auto prev_max = max_progress_batch_size_.load();
        if (batch_size > prev_max) {
            max_progress_batch_size_.store(batch_size);
        }
        if (progress_callback_) {
            for (const auto& item : batch) {
                progress_callback_(item);
            }
        }
    }
}

void DBWorkflowWorkerCoordinator::DrainResultsLoop() {
    simcore::PRResult result;
    while (results_q_.pop_wait(result)) {
        std::optional<DispatchedJobContext> context;
        {
            std::lock_guard<std::mutex> lock(workers_mtx_);
            const auto it = dispatched_job_context_by_id_.find(result.job_id);
            if (it != dispatched_job_context_by_id_.end()) {
                context = it->second;
                dispatched_job_context_by_id_.erase(it);
            }
        }

        if (context.has_value() && adapter_chain_orchestrator_) {
            simcore::db::execution::workflow::AdapterChainTrace trace{};
            std::string adapter_error;
            const auto mapped = adapter_chain_orchestrator_->OnJobTerminal(
                context->step.step_kind,
                static_cast<std::int64_t>(result.job_id),
                result,
                &trace,
                &adapter_error);
            ++adapter_job_terminal_invocations_;
            EmitAdapterTraceEvent(
                context->step,
                "OnJobTerminal",
                mapped.has_value() ? "invoked" : "failed",
                static_cast<std::int64_t>(result.job_id),
                context->job_set_id,
                adapter_error.empty() ? std::nullopt : std::optional<std::string>(adapter_error));
            if (!mapped.has_value() && !adapter_error.empty()) {
                MarkDeterministicFailure(
                    context->step,
                    static_cast<std::int64_t>(result.job_id),
                    context->job_set_id,
                    "ADAPTER_RESULT_PERSIST_FAILED:" + adapter_error);
            }
        }

        ReleaseWorkerByResult(result);
        (void)job_materialization_service_.CleanupDispatchedOrExpired(static_cast<std::int64_t>(result.job_id));
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
        RegisterWorkerSlotTelemetry(slot);
        MarkWorkerError(slot, "ProcessWorker.start failed");
        return false;
    }

    slot.ready.store(slot.worker->wait_ready(0));
    RegisterWorkerSlotTelemetry(slot);
    if (!slot.ready.load()) {
        std::ostringstream error;
        error << "wait_ready failed err=" << slot.worker->ready_error();
        MarkWorkerError(slot, error.str());
    }
    return slot.ready.load();
}

void DBWorkflowWorkerCoordinator::StopWorkerSlot(WorkerSlot& slot) {
    worker_status_.UpdateState(static_cast<std::int64_t>(slot.id), WorkerStateKind::Stopping);
    worker_status_.SetCurrentJob(static_cast<std::int64_t>(slot.id), std::nullopt, std::nullopt);
    slot.ready.store(false);
    slot.in_flight_job_id.reset();
    if (slot.worker) {
        slot.worker->stop();
    }
    worker_status_.UpdateState(static_cast<std::int64_t>(slot.id), WorkerStateKind::Dead);
    worker_status_.UnregisterWorker(static_cast<std::int64_t>(slot.id));
}

std::vector<DBWorkflowWorkerCoordinator::DispatchableWorkerInfo> DBWorkflowWorkerCoordinator::CollectDispatchableWorkers() {
    std::vector<DispatchableWorkerInfo> dispatchable;
    std::lock_guard<std::mutex> lock(workers_mtx_);
    if (workers_.empty()) {
        return dispatchable;
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
        dispatchable.push_back(DispatchableWorkerInfo{
            .worker_idx = idx,
            .loaded_savestate_affinity_key = slot.loaded_savestate_affinity_key,
        });
    }

    if (!dispatchable.empty()) {
        rr_worker_cursor_ = (dispatchable.back().worker_idx + 1) % workers_.size();
    }
    return dispatchable;
}

void DBWorkflowWorkerCoordinator::ReleaseWorkerByResult(const simcore::PRResult& result) {
    std::lock_guard<std::mutex> lock(workers_mtx_);
    if (result.worker_id >= workers_.size()) {
        return;
    }

    auto& slot = *workers_[result.worker_id];
    slot.in_flight_job_id.reset();
    worker_status_.SetCurrentJob(static_cast<std::int64_t>(slot.id), std::nullopt, std::nullopt);
    if (slot.worker) {
        slot.worker->release_slot();
        if (slot.worker->is_ready()) {
            worker_status_.UpdateState(static_cast<std::int64_t>(slot.id), WorkerStateKind::Idle);
        } else {
            MarkWorkerError(slot, "worker became unavailable after result");
        }
    }
}


void DBWorkflowWorkerCoordinator::EmitAdapterTraceEvent(
    const WorkflowReadyStep& step,
    const std::string& stage,
    const std::string& status,
    std::optional<std::int64_t> job_id,
    std::optional<std::int64_t> job_set_id,
    const std::optional<std::string>& message) const {
    if (execution_db_ == nullptr || execution_db_->WorkflowCommandService() == nullptr) {
        return;
    }
    std::ostringstream detail;
    detail << "stage=" << stage << ";status=" << status;
    if (job_id.has_value()) detail << ";job_id=" << *job_id;
    if (job_set_id.has_value()) detail << ";job_set_id=" << *job_set_id;
    if (message.has_value()) detail << ";message=" << *message;

    std::string error;
    (void)execution_db_->WorkflowCommandService()->AppendStepInputEvent(
        {
            .workflow_instance_id = step.workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .event_kind = "Execution.AdapterChainStage.v1",
            .source_key = stage,
            .request_id = std::nullopt,
            .message = detail.str(),
            .requested_by = "adapter_chain_orchestrator",
        },
        &error);
}

void DBWorkflowWorkerCoordinator::MarkDeterministicFailure(
    const WorkflowReadyStep& step,
    std::optional<std::int64_t> job_id,
    std::optional<std::int64_t> job_set_id,
    const std::string& reason) const {
    if (execution_db_ == nullptr || execution_db_->WorkflowCommandService() == nullptr) {
        return;
    }
    std::string error;
    (void)execution_db_->WorkflowCommandService()->AppendStepInputEvent(
        {
            .workflow_instance_id = step.workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .event_kind = "Execution.AdapterChainFailure.v1",
            .source_key = std::nullopt,
            .request_id = std::nullopt,
            .message = reason,
            .requested_by = "adapter_chain_orchestrator",
        },
        &error);
    (void)execution_db_->WorkflowCommandService()->MarkStepTerminal(
        {
            .workflow_step_id = step.workflow_step_id,
            .terminal_state = "FAILED",
            .requested_by = "adapter_chain_orchestrator",
        },
        &error);
    if (execution_db_->JobCommandService() != nullptr && job_id.has_value() && job_set_id.has_value()) {
        (void)execution_db_->JobCommandService()->AppendLifecycleEvent(
            {
                .kind = simcore::db::execution::jobs::JobLifecycleEventKind::JobProgressed,
                .job_set_id = *job_set_id,
                .job_id = *job_id,
                .message = reason,
                .requested_by = "adapter_chain_orchestrator",
            },
            &error);
    }
}

void DBWorkflowWorkerCoordinator::EmitWorkflowFailureEvents(
    const WorkflowReadyStep& step,
    const std::string& stage,
    const std::string& reason) const {
    if (execution_db_ == nullptr || execution_db_->WorkflowCommandService() == nullptr) {
        return;
    }
    std::ostringstream detail;
    detail << "stage=" << stage << ";reason=" << reason;
    std::string error;
    (void)execution_db_->WorkflowCommandService()->AppendStepInputEvent(
        {
            .workflow_instance_id = step.workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .event_kind = "Execution.WorkflowStepCoordinatorFailure.v1",
            .source_key = stage,
            .request_id = std::nullopt,
            .message = detail.str(),
            .requested_by = "workflow_coordinator",
        },
        &error);
    (void)execution_db_->WorkflowCommandService()->AppendLifecycleEvent(
        {
            .workflow_instance_id = step.workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .event_kind = "Execution.WorkflowStepCoordinatorFailure.v1",
            .message = detail.str(),
            .requested_by = "workflow_coordinator",
        },
        &error);
}

void DBWorkflowWorkerCoordinator::MaybeTerminalFailStepInStrictSmokeMode(
    const WorkflowReadyStep& step,
    const std::string& requested_by) const {
    if (!integration_cfg_.strict_smoke_terminal_on_failure) {
        return;
    }
    if (execution_db_ == nullptr || execution_db_->WorkflowCommandService() == nullptr) {
        return;
    }
    std::string error;
    (void)execution_db_->WorkflowCommandService()->MarkStepTerminal(
        {
            .workflow_step_id = step.workflow_step_id,
            .terminal_state = "FAILED",
            .requested_by = requested_by,
        },
        &error);
}

void DBWorkflowWorkerCoordinator::RegisterWorkerSlotTelemetry(const WorkerSlot& slot) {
    int pid = 0;
    WorkerStateKind state = WorkerStateKind::Dead;
    if (slot.worker) {
        pid = static_cast<int>(slot.worker->GetPid());
        state = slot.worker->is_ready() ? WorkerStateKind::Idle : WorkerStateKind::Dead;
    }
    worker_status_.RegisterWorker(static_cast<std::int64_t>(slot.id), "localhost", pid, "workflow");
    worker_status_.UpdateState(static_cast<std::int64_t>(slot.id), state);
    worker_status_.SetCurrentJob(static_cast<std::int64_t>(slot.id), std::nullopt, std::nullopt);
    worker_status_.RecordHeartbeat(static_cast<std::int64_t>(slot.id));
}

void DBWorkflowWorkerCoordinator::MarkWorkerError(const WorkerSlot& slot, const std::string& error) {
    worker_status_.UpdateState(static_cast<std::int64_t>(slot.id), WorkerStateKind::Dead);
    worker_status_.RecordError(static_cast<std::int64_t>(slot.id), error);
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
        bool announce_created = false;
        {
            std::lock_guard<std::mutex> lock(queue_mtx_);
            announce_created = seen_workflow_instance_ids_.emplace(step.workflow_instance_id).second;
        }
        if (announce_created) {
            (void)PublishWorkflowCreated(WorkflowCreatedSignal{
                .workflow_instance_id = step.workflow_instance_id,
            });
        }
        EnqueueReadyStep(WorkflowReadyStep{
            .workflow_instance_id = step.workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .step_key = step.step_key,
            .step_kind = step.step_kind,
            .priority = step.priority,
            .input_ref_id = step.input_ref_id,
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
