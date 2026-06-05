#include "DBWorkflowWorkerCoordinator.h"

#include "../../../../SimCoreDB/Execution/Workflow/WorkflowTerminalAdvancementService.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <utility>

#include "../../../../SimCoreDB/Execution/Jobs/JobEventOrchestration.h"

namespace simcore::runner::parallel::simcoredb {
namespace {

WorkflowSchedulerAdapter::ScheduleFn ResolveWorkflowScheduleFn(
    simcore::db::IExecutionDb* execution_db,
    const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    WorkflowSchedulerAdapter::ScheduleFn workflow_schedule_fn) {
    if (workflow_schedule_fn) {
        return workflow_schedule_fn;
    }
    auto adapter_chain_orchestrator = std::make_shared<simcore::db::execution::workflow::AdapterChainOrchestrator>(
        program_kind_registry,
        nullptr);
    return [execution_db, program_kind_registry, adapter_chain_orchestrator](const WorkflowReadyStep& step) -> ScheduledJobSet {
        if (execution_db == nullptr || program_kind_registry == nullptr) {
            return {};
        }
        const auto* descriptor = program_kind_registry->FindForStepKind(step.step_kind);
        if (descriptor == nullptr || descriptor->job_persistence == nullptr || adapter_chain_orchestrator == nullptr) {
            return {};
        }
        if (!step.input_ref_id.has_value()) {
            return {};
        }
        const auto persisted = adapter_chain_orchestrator->OnInputComplete(step.step_kind, *step.input_ref_id);
        if (!persisted.has_value() || persisted->root_job_set_id <= 0) {
            return {};
        }
        return ScheduledJobSet{
            .job_set_id = persisted->root_job_set_id,
            .workflow_step_id = step.workflow_step_id,
            .event_lines = persisted->event_lines,
        };
    };
}

bool IsNoWorkWorkflowStep(const WorkflowReadyStep& step) {
    return step.step_kind == "seedprobe.done";
}

} // namespace

DBWorkflowWorkerCoordinator::DBWorkflowWorkerCoordinator(
    simcore::db::IExecutionDb* execution_db,
    DBWorkflowWorkerCoordinatorConfig worker_cfg,
    CoordinatorIntegrationConfig integration_cfg,
    const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    ReadyStepPersistFn persist_materialization_fn,
    simcore::db::execution::workflow::StepCompletionGateService* step_completion_gate,
    simcore::db::IStateDb* state_db)
    : DBWorkflowWorkerCoordinator(
        execution_db,
        nullptr,
        std::move(worker_cfg),
        integration_cfg,
        {},
        std::move(persist_materialization_fn),
        program_kind_registry,
        step_completion_gate,
        state_db) {
}

DBWorkflowWorkerCoordinator::DBWorkflowWorkerCoordinator(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::execution::workflow::IWorkflowModeProvider* mode_provider,
    DBWorkflowWorkerCoordinatorConfig worker_cfg,
    CoordinatorIntegrationConfig integration_cfg,
    WorkflowSchedulerAdapter::ScheduleFn workflow_schedule_fn,
    ReadyStepPersistFn persist_materialization_fn,
    const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    simcore::db::execution::workflow::StepCompletionGateService* step_completion_gate,
    simcore::db::IStateDb* state_db)
    : execution_db_(execution_db)
    , state_db_(state_db)
    , worker_cfg_(std::move(worker_cfg))
    , integration_cfg_(integration_cfg)
    , schedule_ready_step_fn_(ResolveWorkflowScheduleFn(
        execution_db,
        program_kind_registry,
        std::move(workflow_schedule_fn)))
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
    , job_materialization_service_(execution_db, program_kind_registry)
    , program_kind_registry_(program_kind_registry) {
    (void)mode_provider;
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
    if (workflow_step_thread_.joinable() || worker_job_thread_.joinable()) {
        return;
    }

    stop_.store(false);
    job_materialization_service_.ResetForStart();
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
            RegisterWorkerSlotTelemetry(*slot);
            workers_.push_back(std::move(slot));
        }
    }

    progress_drainer_thread_ = std::thread([this]() { DrainProgressLoop(); });
    results_drainer_thread_ = std::thread([this]() { DrainResultsLoop(); });
    job_materializer_thread_ = std::thread([this]() { job_materialization_service_.MaterializeClaimedJobPayloadLoop(stop_); });
    workflow_step_thread_ = std::thread([this]() { WorkflowStepCoordinatorLoop(); });
    worker_job_thread_ = std::thread([this]() { WorkerJobCoordinatorLoop(); });
}

void DBWorkflowWorkerCoordinator::Stop() {
    stop_.store(true);
    queue_cv_.notify_all();
    job_materialization_service_.StopMaterializationLoop();
    progress_q_.close();
    results_q_.close();

    if (workflow_step_thread_.joinable()) {
        workflow_step_thread_.join();
    }
    if (worker_job_thread_.joinable()) {
        worker_job_thread_.join();
    }
    if (job_materializer_thread_.joinable()) {
        job_materializer_thread_.join();
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

    const auto scheduled = MaterializeWorkflowStepInternal(step);
    if (!scheduled.has_value()) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(queue_mtx_);
    ++materialized_count_;
    ++epoch_;
    return scheduled;
}

std::optional<ScheduledJobSet> DBWorkflowWorkerCoordinator::MaterializeWorkflowStepInternal(const WorkflowReadyStep& step) {
    if (!schedule_ready_step_fn_) {
        return std::nullopt;
    }

    const auto scheduled = schedule_ready_step_fn_(step);
    if (scheduled.job_set_id <= 0) {
        return std::nullopt;
    }

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
            return std::nullopt;
        }
    }

    if (persist_materialization_fn_) {
        persist_materialization_fn_(step, scheduled);
    }
    for (const auto& line : scheduled.event_lines) {
        EmitDurableEventLine(line);
    }
    if (execution_db_ != nullptr) {
        const auto details = execution_db_->GetJobSetProgress(scheduled.job_set_id);
        if (details.has_value()) {
            std::ostringstream line;
            line << "[seedprobe-materialization-counts]"
                 << " step=" << step.step_key
                 << " kind=" << step.step_kind
                 << " workflow_step_id=" << step.workflow_step_id
                 << " job_set=" << scheduled.job_set_id
                 << " total=" << details->total_jobs
                 << " done=" << details->completed_jobs;
            if (details->expected_total.has_value()) {
                line << " expected_total=" << *details->expected_total;
            }
            const auto child_rows = execution_db_->GetChildJobSetProgress(scheduled.job_set_id);
            if (!child_rows.empty()) {
                std::int64_t child_total = 0;
                std::int64_t child_expected = 0;
                for (const auto& child : child_rows) {
                    child_total += child.total_jobs;
                    child_expected += child.expected_total.value_or(0);
                }
                line << " child_job_sets=" << child_rows.size()
                     << " child_total=" << child_total
                     << " child_expected_total=" << child_expected;
            }
            EmitDurableEventLine(line.str());
        }
    }
    workflow_bridge_.NotifyMaterialized(step.workflow_step_id, scheduled.job_set_id);
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

bool DBWorkflowWorkerCoordinator::DispatchClaimedJobToWorker(size_t worker_idx, const ClaimedJobRecord& claimed_job) {
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
    if (!EnsureWorkerProgramForJob(worker_idx, claimed_job)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> worker_lock(workers_mtx_);
        dispatched_job_context_by_id_[static_cast<std::uint64_t>(job_id)] = DispatchedJobContext{
            .step = claimed_job.step,
            .job_set_id = claimed_job.job_set_id,
        };
    }
    if (!SendJobToWorker(worker_idx, static_cast<std::uint64_t>(job_id), *claimed_job.payload)) {
        std::lock_guard<std::mutex> worker_lock(workers_mtx_);
        dispatched_job_context_by_id_.erase(static_cast<std::uint64_t>(job_id));
        return false;
    }
    std::lock_guard<std::mutex> worker_lock(workers_mtx_);
    if (worker_idx < workers_.size()) {
        workers_[worker_idx]->loaded_program_kind = claimed_job.program_kind;
        workers_[worker_idx]->loaded_program_runtime_affinity_key = claimed_job.affinity.program_runtime_affinity_key;
        workers_[worker_idx]->loaded_savestate_affinity_key = claimed_job.affinity.savestate_affinity_key;
    }
    return true;
}

bool DBWorkflowWorkerCoordinator::DispatchNextEligibleForWorker(
    size_t worker_idx,
    const MaterializedJobSelectionAffinity& worker_affinity,
    std::chrono::steady_clock::time_point now) {
    ClaimedJobRecord candidate{};
    if (!job_materialization_service_.TrySelectMaterializedJobForWorker(worker_affinity, &candidate)) {
        return false;
    }

    if (!candidate.payload.has_value()) {
        std::ostringstream line;
        line << "[seedprobe-dispatch-invalid] job=" << candidate.job_id
             << " worker=" << worker_idx
             << " step=" << candidate.step.step_key
             << " kind=" << candidate.step.step_kind
             << " workflow_step_id=" << candidate.step.workflow_step_id
             << " job_set=" << candidate.job_set_id
             << " reason=missing_payload";
        EmitDurableEventLine(line.str());
        return false;
    }
    const bool dispatched = DispatchClaimedJobToWorker(worker_idx, candidate);
    if (!dispatched) {
        const bool requeued = job_materialization_service_.RequeueMaterializedJob(candidate.job_id);
        std::ostringstream line;
        line << "[seedprobe-dispatch-requeue] job=" << candidate.job_id
             << " worker=" << worker_idx
             << " step=" << candidate.step.step_key
             << " kind=" << candidate.step.step_kind
             << " workflow_step_id=" << candidate.step.workflow_step_id
             << " job_set=" << candidate.job_set_id
             << " reason=send_failed"
             << " requeued=" << (requeued ? "true" : "false");
        EmitDurableEventLine(line.str());
        return false;
    }
    const bool marked_dispatched = job_materialization_service_.MarkDispatched(candidate.job_id, now);
    std::ostringstream line;
    line << "[seedprobe-dispatch] job=" << candidate.job_id
         << " worker=" << worker_idx
         << " step=" << candidate.step.step_key
         << " kind=" << candidate.step.step_kind
         << " workflow_step_id=" << candidate.step.workflow_step_id
         << " job_set=" << candidate.job_set_id
         << " program_kind=" << candidate.program_kind
         << " claim_sequence=" << candidate.claim_sequence
         << " mark_dispatched=" << (marked_dispatched ? "true" : "false");
    if (candidate.affinity.savestate_affinity_key.has_value()) {
        line << " savestate_affinity=" << *candidate.affinity.savestate_affinity_key;
    }
    if (candidate.affinity.program_runtime_affinity_key.has_value()) {
        line << " runtime_affinity=" << *candidate.affinity.program_runtime_affinity_key;
    }
    EmitDurableEventLine(line.str());
    return true;
}

void DBWorkflowWorkerCoordinator::HandlePayloadMaterializationFailures() {
    for (const auto& failed_payload : job_materialization_service_.ListByState(ClaimedJobLifecycleState::MaterializationFailed)) {
        ++payload_materialization_failure_count_;
        std::ostringstream message;
        message << "job_id=" << failed_payload.job_id << ";job_set_id=" << failed_payload.job_set_id
                << ";reason=build_payload returned empty";
        EmitWorkflowFailureEvents(failed_payload.step, "BuildClaimedPayload", message.str());
        MaybeTerminalFailStepInStrictSmokeMode(failed_payload.step, "workflow_payload_materialize_strict_smoke");
        (void)job_materialization_service_.AbandonClaim(failed_payload.job_id);
    }
}

bool DBWorkflowWorkerCoordinator::EnsureWorkerProgramForJob(size_t worker_idx, const ClaimedJobRecord& claimed_job) {
    if (claimed_job.program_kind <= 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(workers_mtx_);
    if (worker_idx >= workers_.size()) {
        return false;
    }

    auto& slot = *workers_[worker_idx];
    if (!slot.ready.load() || slot.worker == nullptr) {
        return false;
    }

    const auto runtime_key = claimed_job.affinity.program_runtime_affinity_key;
    const auto savestate_key = claimed_job.affinity.savestate_affinity_key;
    const bool needs_program = !slot.loaded_program_kind.has_value()
        || slot.loaded_program_kind.value() != claimed_job.program_kind;
    const bool needs_runtime = slot.loaded_program_runtime_affinity_key != runtime_key;
    const bool needs_savestate = slot.loaded_savestate_affinity_key != savestate_key;
    if (!needs_program && !needs_runtime && !needs_savestate) {
        return true;
    }

    simcore::PSInit init{};
    init.default_timeout_ms = claimed_job.runtime_init.default_timeout_ms > 0
        ? static_cast<uint32_t>(claimed_job.runtime_init.default_timeout_ms)
        : 10000;
    init.derived_buffer_type = claimed_job.runtime_init.derived_buffer_type;
    if (claimed_job.runtime_init.savestate_ref_id > 0) {
        const auto savestate_path = PrepareWorkerSavestatePathForJob(worker_idx, claimed_job, needs_savestate);
        if (!savestate_path.has_value()) {
            MarkWorkerError(slot, "savestate materialization failed");
            return false;
        }
        init.savestate_path = *savestate_path;
    }
    if (!slot.worker->ctl_set_program(
        static_cast<uint8_t>(claimed_job.program_kind),
        static_cast<uint8_t>(claimed_job.program_kind),
        init)) {
        MarkWorkerError(slot, "ctl_set_program failed");
        return false;
    }
    if (!slot.worker->ctl_run_init_once()) {
        MarkWorkerError(slot, "ctl_run_init_once failed");
        return false;
    }
    if (!slot.worker->ctl_activate_main()) {
        MarkWorkerError(slot, "ctl_activate_main failed");
        return false;
    }

    slot.loaded_program_kind = claimed_job.program_kind;
    slot.loaded_program_runtime_affinity_key = runtime_key;
    slot.loaded_savestate_affinity_key = savestate_key;
    worker_status_.SetCurrentJob(static_cast<std::int64_t>(slot.id), std::nullopt, claimed_job.program_kind);
    worker_status_.UpdateState(static_cast<std::int64_t>(slot.id), WorkerStateKind::Idle);
    worker_status_.RecordHeartbeat(static_cast<std::int64_t>(slot.id));
    return true;
}

std::optional<std::string> DBWorkflowWorkerCoordinator::PrepareWorkerSavestatePathForJob(
    size_t worker_idx,
    const ClaimedJobRecord& claimed_job,
    bool force_rematerialize) {
    if (claimed_job.runtime_init.savestate_ref_id <= 0) {
        return std::string{};
    }
    if (state_db_ == nullptr) {
        return std::nullopt;
    }

    const auto worker_root = std::filesystem::path(worker_cfg_.worker_dir_root)
        / ".worker"
        / ("worker-" + std::to_string(worker_idx));
    const auto savestate_dir = worker_root / "savestate";
    const auto savestate_path = savestate_dir / "current.sav";

    std::error_code ec;
    if (!force_rematerialize && std::filesystem::exists(savestate_path, ec) && !ec) {
        return savestate_path.string();
    }

    std::filesystem::remove_all(savestate_dir, ec);
    if (ec) {
        return std::nullopt;
    }
    std::filesystem::create_directories(savestate_dir, ec);
    if (ec) {
        return std::nullopt;
    }

    std::string error;
    const auto materialized = state_db_->MaterializeSavestateToPath(
        claimed_job.runtime_init.savestate_ref_id,
        savestate_path.string(),
        &error);
    if (!materialized.has_value() || materialized->empty()) {
        return std::nullopt;
    }
    return materialized;
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

void DBWorkflowWorkerCoordinator::SetResultMapEventCallback(ResultMapEventCallback callback) {
    result_map_event_callback_ = std::move(callback);
    job_materialization_service_.SetEventCallback([this](const std::string& line) {
        EmitDurableEventLine(line);
    });
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
    telemetry.dispatch_success_count = dispatch_success_count_.load();
    telemetry.dispatch_miss_count = dispatch_miss_count_.load();
    const auto attempts = telemetry.dispatch_attempt_count;
    telemetry.dispatch_miss_rate_basis_points = attempts <= 0
        ? 0
        : static_cast<std::int64_t>((telemetry.dispatch_miss_count * 10000) / attempts);
    telemetry.progress_batch_count = progress_batch_count_.load();
    telemetry.max_progress_batch_size = max_progress_batch_size_.load();
    telemetry.results_received_count = results_received_count_.load();
    telemetry.workflow_created_signal_count = workflow_created_signal_count_.load();
    telemetry.materialization_failure_count = materialization_failure_count_.load();
    telemetry.payload_materialization_failure_count = payload_materialization_failure_count_.load();
    return telemetry;
}

std::vector<WorkerSnapshot> DBWorkflowWorkerCoordinator::SnapshotWorkers() const {
    std::lock_guard<std::mutex> lock(workers_mtx_);
    return worker_status_.GetClusterSnapshot();
}

void DBWorkflowWorkerCoordinator::WorkflowStepCoordinatorLoop() {
    ReconcileTerminalWorkflowSteps();
    PollReadyStepsFromDb();

    while (!stop_.load()) {
        if (paused_.load()) {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(50));
            continue;
        }

        ReconcileTerminalWorkflowSteps();
        PollReadyStepsFromDb();
        WorkflowReadyStep step;
        if (!TryDequeueReadyStep(&step)) {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(50));
            continue;
        }

        ProcessReadyWorkflowStep(step);
    }
}

void DBWorkflowWorkerCoordinator::ReconcileTerminalWorkflowSteps() {
    if (!integration_cfg_.workflow_enabled
        || execution_db_ == nullptr
        || execution_db_->WorkflowQueryService() == nullptr
        || execution_db_->WorkflowCommandService() == nullptr
        || adapter_chain_orchestrator_ == nullptr) {
        return;
    }

    const auto snapshots = execution_db_->WorkflowQueryService()->ListTerminalReadyStepSnapshots(8);
    if (snapshots.empty()) {
        return;
    }

    simcore::db::execution::workflow::WorkflowTerminalAdvancementService terminal_advancement(
        adapter_chain_orchestrator_.get(),
        execution_db_->WorkflowQueryService(),
        execution_db_->WorkflowCommandService());

    for (const auto& snapshot : snapshots) {
        simcore::db::execution::workflow::WorkflowTerminalAdvancementResult advancement{};
        std::string advancement_error;
        const WorkflowReadyStep step{
            .workflow_instance_id = snapshot.workflow_instance_id,
            .workflow_step_id = snapshot.workflow_step_id,
            .step_key = snapshot.step_key,
            .step_kind = snapshot.step_kind,
            .priority = 0,
        };
        if (!terminal_advancement.AdvanceSnapshot(snapshot, &advancement, &advancement_error)) {
            EmitAdapterTraceEvent(
                step,
                "OnStepTerminalSweep",
                "failed",
                std::nullopt,
                snapshot.job_set_id,
                advancement_error.empty() ? std::nullopt : std::optional<std::string>(advancement_error));
            continue;
        }

        EmitAdapterTraceEvent(
            step,
            "OnStepTerminalSweep",
            advancement.advanced_next_step
                ? "advanced"
                : advancement.workflow_completed ? "workflow_completed" : advancement.gate_can_transition ? "terminal" : "blocked",
            std::nullopt,
            snapshot.job_set_id,
            advancement.blocked_reason);
        if (advancement.advanced_next_step || advancement.workflow_completed) {
            queue_cv_.notify_all();
        }
    }
}

void DBWorkflowWorkerCoordinator::WorkerJobCoordinatorLoop() {
    while (!stop_.load()) {
        if (paused_.load()) {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(50));
            continue;
        }

        ReconcileWorkerPool();

        const auto now = std::chrono::steady_clock::now();
        const auto worker_target = ActiveWorkerCount();
        const auto buffered = job_materialization_service_.CountBufferedJobs();
        std::size_t claimed_count = 0;
        if (buffered < worker_target) {
            const auto claim_budget = worker_target - buffered;
            claimed_count = job_materialization_service_.ClaimJobs(claim_budget, now);
            if (claimed_count > 0) {
                std::ostringstream line;
                line << "[seedprobe-claim-batch] claimed=" << claimed_count
                     << " budget=" << claim_budget
                     << " buffered_before=" << buffered
                     << " worker_target=" << worker_target
                     << " buffered_after=" << job_materialization_service_.CountBufferedJobs();
                EmitDurableEventLine(line.str());
            }
        }

        HandlePayloadMaterializationFailures();

        bool dispatched_any = false;
        const auto dispatchable_workers = CollectDispatchableWorkers();
        for (const auto& worker : dispatchable_workers) {
            ++dispatch_attempt_count_;
            const bool dispatched = DispatchNextEligibleForWorker(
                worker.worker_idx,
                MaterializedJobSelectionAffinity{
                    .savestate_affinity_key = worker.loaded_savestate_affinity_key,
                    .program_kind = worker.loaded_program_kind,
                    .program_runtime_affinity_key = worker.loaded_program_runtime_affinity_key,
                },
                std::chrono::steady_clock::now());
            if (dispatched) {
                ++dispatch_success_count_;
                dispatched_any = true;
            } else {
                ++dispatch_miss_count_;
            }
        }

        if (claimed_count == 0 && !dispatched_any) {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(50));
        }
    }
}

void DBWorkflowWorkerCoordinator::ProcessReadyWorkflowStep(const WorkflowReadyStep& step) {
    if (!integration_cfg_.workflow_enabled) {
        return;
    }

    if (IsNoWorkWorkflowStep(step)) {
        if (CompleteNoWorkWorkflowStep(step)) {
            queue_cv_.notify_all();
        }
        return;
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
            return;
        }
        EnqueueReadyStep(step);
        return;
    }

    ++input_complete_count_;
    if (aggregation.input_latency_ms.has_value()) {
        last_input_latency_ms_.store(*aggregation.input_latency_ms);
    }

    const auto materialize_started = std::chrono::steady_clock::now();
    (void)MaterializeWorkflowStepInternal(step);
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
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        ++materialized_count_;
        ++epoch_;
    }
    queue_cv_.notify_one();
}

bool DBWorkflowWorkerCoordinator::CompleteNoWorkWorkflowStep(const WorkflowReadyStep& step) const {
    if (execution_db_ == nullptr || execution_db_->WorkflowCommandService() == nullptr) {
        return false;
    }

    auto* commands = execution_db_->WorkflowCommandService();
    std::string error;
    (void)commands->AppendStepInputEvent(
        {
            .workflow_instance_id = step.workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .event_kind = "Execution.WorkflowStepInputComplete.v1",
            .source_key = std::nullopt,
            .request_id = std::nullopt,
            .message = std::optional<std::string>("no-work-step"),
            .requested_by = "workflow_no_work_step",
        },
        &error);

    if (!commands->MarkStepTerminal(
        {
            .workflow_step_id = step.workflow_step_id,
            .terminal_state = "COMPLETED",
            .requested_by = "workflow_no_work_step",
        },
        &error)) {
        EmitWorkflowFailureEvents(
            step,
            "CompleteNoWorkStep",
            error.empty() ? "mark terminal failed" : error);
        return false;
    }

    if (!commands->AppendLifecycleEvent(
        {
            .workflow_instance_id = step.workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .event_kind = "Execution.WorkflowTransitionEvaluated.v1",
            .message = std::optional<std::string>("transition_evaluated"),
            .requested_by = "workflow_no_work_step",
        },
        &error)) {
        EmitWorkflowFailureEvents(
            step,
            "CompleteNoWorkStep",
            error.empty() ? "append transition evaluation failed" : error);
        return false;
    }

    if (!commands->AppendLifecycleEvent(
        {
            .workflow_instance_id = step.workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .event_kind = "Execution.WorkflowTransitionAdvanced.v1",
            .message = std::optional<std::string>("transition_advanced"),
            .requested_by = "workflow_no_work_step",
        },
        &error)) {
        EmitWorkflowFailureEvents(
            step,
            "CompleteNoWorkStep",
            error.empty() ? "append transition advanced failed" : error);
        return false;
    }

    if (!commands->CompleteWorkflowInstance(
        {
            .workflow_instance_id = step.workflow_instance_id,
            .requested_by = "workflow_no_work_step",
        },
        &error)) {
        EmitWorkflowFailureEvents(
            step,
            "CompleteNoWorkStep",
            error.empty() ? "complete workflow failed" : error);
        return false;
    }

    std::ostringstream line;
    line << "[seedprobe-terminal-advance]"
         << " step=" << step.step_key
         << " kind=" << step.step_kind
         << " workflow_step_id=" << step.workflow_step_id
         << " status=workflow_completed"
         << " step_terminal=true"
         << " gate=true"
         << " next_step=false"
         << " workflow_completed=true"
         << " no_work=true";
    EmitDurableEventLine(line.str());
    return true;
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
        for (const auto& item : batch) {
            if (execution_db_ != nullptr && execution_db_->JobCommandService() != nullptr && item.job_id > 0) {
                std::ostringstream message;
                message << "worker=" << item.worker_id << " progress=" << item.text;
                std::string error;
                if (!execution_db_->JobCommandService()->AppendLifecycleEvent(
                        {
                            .kind = simcore::db::execution::jobs::JobLifecycleEventKind::JobProgressed,
                            .job_id = static_cast<std::int64_t>(item.job_id),
                            .message = message.str(),
                            .requested_by = "workflow_progress_drainer",
                        },
                        &error)) {
                    std::ostringstream line;
                    line << "[workflow-progress-persist-failed] job=" << item.job_id
                         << " worker=" << item.worker_id;
                    if (!error.empty()) {
                        line << " error=" << error;
                    }
                    EmitDurableEventLine(line.str());
                }
            }
            if (progress_callback_) {
                progress_callback_(item);
            }
        }
    }
}

void DBWorkflowWorkerCoordinator::DrainResultsLoop() {
    simcore::PRResult result;
    while (results_q_.pop_wait(result)) {
        ++results_received_count_;
        std::optional<DispatchedJobContext> context;
        bool worker_known = false;
        std::optional<std::uint64_t> worker_in_flight_job_id;
        {
            std::lock_guard<std::mutex> lock(workers_mtx_);
            const auto it = dispatched_job_context_by_id_.find(result.job_id);
            if (it != dispatched_job_context_by_id_.end()) {
                context = it->second;
                dispatched_job_context_by_id_.erase(it);
            }
            if (result.worker_id < workers_.size()) {
                worker_known = true;
                worker_in_flight_job_id = workers_[result.worker_id]->in_flight_job_id;
            }
        }

        if (!context.has_value()) {
            std::ostringstream line;
            line << "[seedprobe-result-no-context] job=" << result.job_id
                 << " worker=" << result.worker_id;
            if (worker_known) {
                line << " worker_in_flight=";
                if (worker_in_flight_job_id.has_value()) {
                    line << *worker_in_flight_job_id;
                } else {
                    line << "none";
                }
            } else {
                line << " worker_in_flight=unknown_worker";
            }
            EmitDurableEventLine(line.str());
        }
        if (worker_known && (!worker_in_flight_job_id.has_value() || *worker_in_flight_job_id != result.job_id)) {
            std::ostringstream line;
            line << "[seedprobe-result-worker-mismatch] job=" << result.job_id
                 << " worker=" << result.worker_id
                 << " worker_in_flight=";
            if (worker_in_flight_job_id.has_value()) {
                line << *worker_in_flight_job_id;
            } else {
                line << "none";
            }
            EmitDurableEventLine(line.str());
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
            if (mapped.has_value() && result_map_event_callback_) {
                for (const auto& line : mapped->event_lines) {
                    result_map_event_callback_(line);
                }
            }
            if (!mapped.has_value() && !adapter_error.empty()) {
                std::ostringstream line;
                line << "[seedprobe-result-map-failed] job=" << result.job_id
                     << " worker=" << result.worker_id
                     << " step=" << context->step.step_key
                     << " kind=" << context->step.step_kind
                     << " workflow_step_id=" << context->step.workflow_step_id
                     << " job_set=" << context->job_set_id
                     << " error=" << adapter_error;
                EmitDurableEventLine(line.str());
                MarkDeterministicFailure(
                    context->step,
                    static_cast<std::int64_t>(result.job_id),
                    context->job_set_id,
                    "ADAPTER_RESULT_PERSIST_FAILED:" + adapter_error);
            } else if (execution_db_ != nullptr
                && execution_db_->WorkflowQueryService() != nullptr
                && execution_db_->WorkflowCommandService() != nullptr) {
                simcore::db::execution::workflow::WorkflowTerminalAdvancementService terminal_advancement(
                    adapter_chain_orchestrator_.get(),
                    execution_db_->WorkflowQueryService(),
                    execution_db_->WorkflowCommandService());
                simcore::db::execution::workflow::WorkflowTerminalAdvancementResult advancement{};
                std::string advancement_error;
                if (!terminal_advancement.AdvanceForTerminalJob(
                        static_cast<std::int64_t>(result.job_id),
                        &advancement,
                        &advancement_error)) {
                    EmitAdapterTraceEvent(
                        context->step,
                        "OnStepTerminal",
                        "failed",
                        static_cast<std::int64_t>(result.job_id),
                        context->job_set_id,
                        advancement_error.empty() ? std::nullopt : std::optional<std::string>(advancement_error));
                    std::ostringstream line;
                    line << "[seedprobe-terminal-advance] job=" << result.job_id
                         << " step=" << context->step.step_key
                         << " kind=" << context->step.step_kind
                         << " workflow_step_id=" << context->step.workflow_step_id
                         << " job_set=" << context->job_set_id
                         << " status=failed";
                    if (!advancement_error.empty()) {
                        line << " error=" << advancement_error;
                    }
                    EmitDurableEventLine(line.str());
                } else if (advancement.snapshot_found) {
                    const char* status = advancement.advanced_next_step
                        ? "advanced"
                        : advancement.workflow_completed ? "workflow_completed"
                        : advancement.gate_can_transition ? "terminal"
                        : "blocked";
                    EmitAdapterTraceEvent(
                        context->step,
                        "OnStepTerminal",
                        status,
                        static_cast<std::int64_t>(result.job_id),
                        context->job_set_id,
                        advancement.blocked_reason);
                    std::ostringstream line;
                    line << "[seedprobe-terminal-advance] job=" << result.job_id
                         << " step=" << context->step.step_key
                         << " kind=" << context->step.step_kind
                         << " workflow_step_id=" << context->step.workflow_step_id
                         << " job_set=" << context->job_set_id
                         << " status=" << status
                         << " step_terminal=" << (advancement.step_marked_terminal ? "true" : "false")
                         << " gate=" << (advancement.gate_can_transition ? "true" : "false")
                         << " next_step=" << (advancement.advanced_next_step ? "true" : "false")
                         << " workflow_completed=" << (advancement.workflow_completed ? "true" : "false");
                    if (advancement.blocked_reason.has_value()) {
                        line << " blocked_reason=" << *advancement.blocked_reason;
                    }
                    EmitDurableEventLine(line.str());
                    if (advancement.advanced_next_step || advancement.workflow_completed) {
                        queue_cv_.notify_all();
                    }
                } else {
                    std::ostringstream line;
                    line << "[seedprobe-terminal-advance] job=" << result.job_id
                         << " step=" << context->step.step_key
                         << " kind=" << context->step.step_kind
                         << " workflow_step_id=" << context->step.workflow_step_id
                         << " job_set=" << context->job_set_id
                         << " status=snapshot_missing";
                    EmitDurableEventLine(line.str());
                }
            }
        }

        ReleaseWorkerByResult(result);
        (void)job_materialization_service_.CleanupDispatchedOrExpired(static_cast<std::int64_t>(result.job_id));
        if (result_callback_) {
            result_callback_(result);
        }
    }
}

void DBWorkflowWorkerCoordinator::ReconcileWorkerPool() {
    std::lock_guard<std::mutex> lock(workers_mtx_);
    while (workers_.size() < worker_cfg_.desired_workers) {
        const auto worker_idx = workers_.size();
        auto slot = std::make_unique<WorkerSlot>();
        slot->id = worker_idx;
        slot->worker = std::make_unique<simcore::ProcessWorker>();
        slot->worker->set_progress_queue(&progress_q_);
        workers_.push_back(std::move(slot));
    }

    for (size_t worker_idx = 0; worker_idx < workers_.size(); ++worker_idx) {
        auto& slot = *workers_[worker_idx];
        if (slot.start_attempted || slot.ready.load()) {
            continue;
        }
        (void)StartWorkerSlot(worker_idx);
    }

    while (workers_.size() > worker_cfg_.desired_workers) {
        auto& slot = workers_.back();
        if (slot->in_flight_job_id.has_value()) {
            break;
        }
        StopWorkerSlot(*slot);
        workers_.pop_back();
    }
}

bool DBWorkflowWorkerCoordinator::StartWorkerSlot(size_t worker_idx) {
    if (worker_idx >= workers_.size()) {
        return false;
    }

    auto& slot = *workers_[worker_idx];
    slot.start_attempted = true;
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
    slot.start_attempted = false;
    slot.in_flight_job_id.reset();
    slot.loaded_program_kind.reset();
    slot.loaded_program_runtime_affinity_key.reset();
    slot.loaded_savestate_affinity_key.reset();
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
            .loaded_program_kind = slot.loaded_program_kind,
            .loaded_program_runtime_affinity_key = slot.loaded_program_runtime_affinity_key,
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

void DBWorkflowWorkerCoordinator::EmitDurableEventLine(const std::string& line) const {
    if (result_map_event_callback_) {
        result_map_event_callback_(line);
    }
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
