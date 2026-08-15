#include "WorkflowCoordinatorService.h"

#include "WorkflowComposition.h"
#include "WorkflowGraphRoutingService.h"
#include "WorkflowTerminalAdvancementService.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <sstream>
#include <utility>

namespace savor::db::execution::workflow {
namespace {

void StoreMax(std::atomic<std::int64_t>& target, std::int64_t value) {
    auto current = target.load();
    while (value > current && !target.compare_exchange_weak(current, value)) {
    }
}

} // namespace

void CoordinatorItemCreditSource::Open(std::size_t total_credits) noexcept {
    open_.store(false, std::memory_order_release);
    reservations_.store(0, std::memory_order_relaxed);
    external_usage_.store(0, std::memory_order_relaxed);
    total_credits_.store(total_credits, std::memory_order_relaxed);
    open_.store(total_credits > 0, std::memory_order_release);
}

void CoordinatorItemCreditSource::SetCapacity(
    std::size_t total_credits) noexcept {
    total_credits_.store(total_credits, std::memory_order_release);
}

void CoordinatorItemCreditSource::SetExternalUsage(
    std::size_t used_credits) noexcept {
    external_usage_.store(used_credits, std::memory_order_release);
}

std::size_t CoordinatorItemCreditSource::AvailableCredits() const noexcept {
    if (!open_.load(std::memory_order_acquire)) {
        return 0;
    }
    const auto total = total_credits_.load(std::memory_order_acquire);
    const auto external = external_usage_.load(std::memory_order_acquire);
    const auto reserved = reservations_.load(std::memory_order_acquire);
    if (external >= total || reserved >= total - external) {
        return 0;
    }
    return total - external - reserved;
}

bool CoordinatorItemCreditSource::TryReserve(std::size_t credits) noexcept {
    if (credits == 0) {
        return true;
    }
    auto reserved = reservations_.load(std::memory_order_acquire);
    for (;;) {
        if (!open_.load(std::memory_order_acquire)) {
            return false;
        }
        const auto total = total_credits_.load(std::memory_order_acquire);
        const auto external = external_usage_.load(std::memory_order_acquire);
        if (external >= total
            || reserved >= total - external
            || credits > total - external - reserved) {
            return false;
        }
        if (reservations_.compare_exchange_weak(
                reserved,
                reserved + credits,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
}

void CoordinatorItemCreditSource::ReleaseReservations(
    std::size_t credits) noexcept {
    auto reserved = reservations_.load(std::memory_order_acquire);
    for (;;) {
        const auto release = std::min(reserved, credits);
        if (reservations_.compare_exchange_weak(
                reserved,
                reserved - release,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;
        }
    }
}

void CoordinatorItemCreditSource::Close() noexcept {
    open_.store(false, std::memory_order_release);
    total_credits_.store(0, std::memory_order_relaxed);
    external_usage_.store(0, std::memory_order_relaxed);
    reservations_.store(0, std::memory_order_relaxed);
}

bool CoordinatorItemCreditSource::IsOpen() const noexcept {
    return open_.load(std::memory_order_acquire);
}

WorkflowCoordinatorService::WorkflowCoordinatorService(
    savor::db::IExecutionDb* execution_db,
    const savor::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    WorkflowCoordinatorConfig config,
    EventLineCallback event_line_callback,
    StepCompletionGateService* step_completion_gate,
    savor::db::IAuthoringDb* authoring_db)
    : execution_db_(execution_db)
    , authoring_db_(authoring_db)
    , program_kind_registry_(program_kind_registry)
    , config_(config)
    , event_line_callback_(std::move(event_line_callback)) {
    if (step_completion_gate != nullptr) {
        step_completion_gate_ = step_completion_gate;
    } else {
        owned_step_completion_gate_ = std::make_unique<StepCompletionGateService>();
        step_completion_gate_ = owned_step_completion_gate_.get();
    }
}

WorkflowCoordinatorService::~WorkflowCoordinatorService() {
    Stop();
}

bool WorkflowCoordinatorService::Start(std::string* error_out) {
    if (running_.load()) {
        return true;
    }
    if (!config_.workflow_enabled) {
        if (error_out) {
            error_out->clear();
        }
        return true;
    }
    if (execution_db_ == nullptr
        || execution_db_->WorkflowQueryService() == nullptr
        || execution_db_->WorkflowCommandService() == nullptr) {
        if (error_out) {
            *error_out = "workflow coordinator requires workflow query and command services";
        }
        return false;
    }
    if (program_kind_registry_ == nullptr) {
        if (error_out) {
            *error_out = "workflow coordinator requires a program kind registry";
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(terminal_notification_mtx_);
        terminal_notifications_.clear();
    }
    next_terminal_repair_at_ = std::chrono::steady_clock::now();
    next_descriptor_repair_at_ = std::chrono::steady_clock::now();
    observed_registry_generation_ = program_kind_registry_->Generation();
    stop_.store(false);
    running_.store(true);
    worker_thread_ = std::thread([this]() { Loop(); });
    if (error_out) {
        error_out->clear();
    }
    return true;
}

void WorkflowCoordinatorService::Stop() {
    stop_.store(true);
    wait_cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    running_.store(false);
}

bool WorkflowCoordinatorService::IsRunning() const {
    return running_.load();
}

WorkflowCoordinatorTelemetry WorkflowCoordinatorService::SnapshotTelemetry() const {
    WorkflowCoordinatorTelemetry telemetry{};
    telemetry.ready_scan_count = ready_scan_count_.load();
    telemetry.ready_steps_seen = ready_steps_seen_.load();
    telemetry.workflow_created_seen_count = workflow_created_seen_count_.load();
    telemetry.last_ready_scan_latency_ms = last_ready_scan_latency_ms_.load();
    telemetry.active_materialized_workflow_count = active_materialized_workflow_count_.load();
    telemetry.materialization_throttle_count = materialization_throttle_count_.load();
    telemetry.materialization_count = materialization_count_.load();
    telemetry.materialization_failure_count = materialization_failure_count_.load();
    telemetry.last_materialization_latency_ms = last_materialization_latency_ms_.load();
    telemetry.max_materialization_latency_ms = max_materialization_latency_ms_.load();
    telemetry.continuation_count = continuation_count_.load();
    telemetry.continuation_added_work_count =
        continuation_added_work_count_.load();
    telemetry.continuation_failure_count =
        continuation_failure_count_.load();
    telemetry.terminal_scan_count = terminal_scan_count_.load();
    telemetry.terminal_step_count = terminal_step_count_.load();
    telemetry.terminal_empty_step_count = terminal_empty_step_count_.load();
    telemetry.terminal_failed_step_count = terminal_failed_step_count_.load();
    telemetry.terminal_completed_step_count = terminal_completed_step_count_.load();
    telemetry.transition_advanced_count = transition_advanced_count_.load();
    telemetry.workflow_completed_count = workflow_completed_count_.load();
    telemetry.workflow_failed_count = workflow_failed_count_.load();
    telemetry.targeted_terminal_notification_count =
        targeted_terminal_notification_count_.load();
    telemetry.targeted_terminal_advancement_count =
        targeted_terminal_advancement_count_.load();
    telemetry.descriptor_unavailable_count =
        descriptor_unavailable_count_.load();
    telemetry.descriptor_resumed_count = descriptor_resumed_count_.load();
    return telemetry;
}

bool WorkflowCoordinatorService::PublishTerminalCommit(
    const TerminalWorkflowStepNotification& notification) {
    if (notification.commit_sequence == 0
        || notification.workflow_step_id <= 0
        || notification.job_id <= 0) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(terminal_notification_mtx_);
        terminal_notifications_.try_emplace(
            std::make_tuple(
                notification.commit_sequence,
                notification.workflow_step_id,
                notification.job_id),
            notification);
    }
    ++targeted_terminal_notification_count_;
    terminal_notification_generation_.fetch_add(
        1,
        std::memory_order_release);
    wait_cv_.notify_all();
    return true;
}

void WorkflowCoordinatorService::Loop() {
    while (!stop_.load()) {
        const std::uint64_t observed_terminal_generation =
            terminal_notification_generation_.load(
                std::memory_order_acquire);
        const bool advanced = AdvanceAvailableWork();
        if (!advanced) {
            std::unique_lock<std::mutex> lock(wait_mtx_);
            wait_cv_.wait_for(
                lock,
                config_.poll_interval,
                [&]() {
                    return stop_.load() ||
                        terminal_notification_generation_.load(
                            std::memory_order_acquire) !=
                            observed_terminal_generation;
                });
        }
    }
}

bool WorkflowCoordinatorService::AdvanceAvailableWork() {
    bool advanced = false;
    advanced = ReconcileTargetedTerminalNotifications() || advanced;
    const auto now = std::chrono::steady_clock::now();
    const auto registry_generation = program_kind_registry_ != nullptr
        ? program_kind_registry_->Generation()
        : 0;
    if (now >= next_descriptor_repair_at_
        || registry_generation != observed_registry_generation_) {
        advanced = ReconcileDescriptorAvailability() || advanced;
        observed_registry_generation_ = registry_generation;
        next_descriptor_repair_at_ =
            now + std::max(
                config_.descriptor_repair_interval,
                std::chrono::milliseconds(1));
    }
    if (now >= next_terminal_repair_at_) {
        advanced = ReconcileTerminalWorkflowSteps() || advanced;
        const auto repair_interval =
            std::max(config_.terminal_repair_interval,
                     std::chrono::milliseconds(1));
        next_terminal_repair_at_ = now + repair_interval;
    }
    advanced = PollReadyStepsFromDb() || advanced;
    return advanced;
}

bool WorkflowCoordinatorService::PollReadyStepsFromDb() {
    const auto t0 = std::chrono::steady_clock::now();
    auto finish_scan = [this, t0]() {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        last_ready_scan_latency_ms_.store(static_cast<std::int64_t>(elapsed));
        ++ready_scan_count_;
    };
    auto* queries = execution_db_ != nullptr ? execution_db_->WorkflowQueryService() : nullptr;
    if (queries == nullptr) {
        finish_scan();
        return false;
    }

    const auto active_count = queries->CountActiveMaterializedWorkflows();
    active_materialized_workflow_count_.store(active_count);
    // Workflow decisions are durable control-plane work. Worker availability
    // is intentionally not a fanout gate; published worksets may accumulate
    // while the user-controlled execution stack is stopped.
    const auto ready_steps = queries->ListReadySteps(config_.ready_scan_limit);
    bool processed_any = false;
    for (const auto& step : ready_steps) {
        if (seen_workflow_instance_ids_.emplace(step.workflow_instance_id).second) {
            ++workflow_created_seen_count_;
        }
        ++ready_steps_seen_;
        processed_any = ProcessReadyWorkflowStep(step) || processed_any;
    }

    finish_scan();
    return processed_any;
}

bool WorkflowCoordinatorService::ReconcileDescriptorAvailability() {
    auto* queries = execution_db_ != nullptr
        ? execution_db_->WorkflowQueryService()
        : nullptr;
    auto* commands = execution_db_ != nullptr
        ? execution_db_->WorkflowCommandService()
        : nullptr;
    if (queries == nullptr || commands == nullptr
        || program_kind_registry_ == nullptr) {
        return false;
    }

    const auto running = queries->ListWorkflowInstances(
        WorkflowInstanceState::Running,
        0,
        (std::numeric_limits<std::int64_t>::max)());
    bool resumed_any = false;
    for (const auto& workflow : running) {
        for (const auto& step :
             queries->ListBlockedSteps(workflow.workflow_instance_id)) {
            if (!step.blocked_reason.has_value()
                || *step.blocked_reason != "DESCRIPTOR_UNAVAILABLE"
                || !program_kind_registry_->HasRequiredAdaptersForStepKind(
                    step.step_kind)) {
                continue;
            }

            std::string error;
            if (!commands->MarkStepBlocked(
                    {
                        .workflow_step_id = step.workflow_step_id,
                        .blocked_reason = std::nullopt,
                        .requested_by =
                            "workflow_coordinator_descriptor_available",
                    },
                    &error)) {
                EmitWorkflowFailureEvent(
                    WorkflowReadyStepRecord{
                        .workflow_instance_id = step.workflow_instance_id,
                        .workflow_step_id = step.workflow_step_id,
                        .step_key = step.step_key,
                        .step_kind = step.step_kind,
                    },
                    "ResumeDescriptorUnavailableStep",
                    error.empty() ? "failed clearing descriptor block" : error);
                continue;
            }
            (void)commands->AppendLifecycleEvent(
                {
                    .workflow_instance_id = step.workflow_instance_id,
                    .workflow_step_id = step.workflow_step_id,
                    .event_kind =
                        "Execution.WorkflowStepDescriptorAvailable.v1",
                    .message = step.step_kind,
                    .requested_by =
                        "workflow_coordinator_descriptor_available",
                },
                nullptr);
            ++descriptor_resumed_count_;
            resumed_any = true;
        }
    }
    return resumed_any;
}

bool WorkflowCoordinatorService::ReconcileTargetedTerminalNotifications() {
    std::vector<TerminalWorkflowStepNotification> notifications;
    {
        std::lock_guard<std::mutex> lock(terminal_notification_mtx_);
        const auto limit = std::max<std::size_t>(
            1,
            config_.terminal_scan_limit);
        notifications.reserve(std::min(limit, terminal_notifications_.size()));
        auto it = terminal_notifications_.begin();
        while (it != terminal_notifications_.end()
            && notifications.size() < limit) {
            notifications.push_back(it->second);
            it = terminal_notifications_.erase(it);
        }
    }
    if (notifications.empty()) {
        return false;
    }

    auto* queries = execution_db_ != nullptr
        ? execution_db_->WorkflowQueryService()
        : nullptr;
    if (queries == nullptr) {
        return false;
    }
    bool advanced_any = false;
    for (const auto& notification : notifications) {
        const auto snapshot =
            queries->GetStepTerminalSnapshotForJob(notification.job_id);
        if (!snapshot.has_value()) {
            continue;
        }
        if (snapshot->workflow_step_id != notification.workflow_step_id) {
            WorkflowReadyStepRecord step{};
            step.workflow_instance_id = snapshot->workflow_instance_id;
            step.workflow_step_id = snapshot->workflow_step_id;
            step.step_key = snapshot->step_key;
            step.step_kind = snapshot->step_kind;
            EmitWorkflowFailureEvent(
                step,
                "AdvanceTargetedTerminalStep",
                "terminal notification workflow_step_id mismatch");
            continue;
        }
        const bool advanced = AdvanceTerminalSnapshot(
            *snapshot,
            "AdvanceTargetedTerminalStep");
        if (advanced) {
            ++targeted_terminal_advancement_count_;
        }
        advanced_any = advanced || advanced_any;
    }
    return advanced_any;
}

bool WorkflowCoordinatorService::ReconcileTerminalWorkflowSteps() {
    auto* queries = execution_db_ != nullptr ? execution_db_->WorkflowQueryService() : nullptr;
    auto* commands = execution_db_ != nullptr ? execution_db_->WorkflowCommandService() : nullptr;
    if (queries == nullptr || commands == nullptr) {
        return false;
    }

    const auto snapshots = queries->ListTerminalReadyStepSnapshots(config_.terminal_scan_limit);
    ++terminal_scan_count_;
    if (snapshots.empty()) {
        return false;
    }

    bool advanced_any = false;
    for (const auto& snapshot : snapshots) {
        advanced_any = AdvanceTerminalSnapshot(
            snapshot,
            "AdvanceTerminalStep") || advanced_any;
    }
    return advanced_any;
}

bool WorkflowCoordinatorService::AdvanceTerminalSnapshot(
    const WorkflowStepTerminalSnapshot& snapshot,
    const char* failure_stage) {
    auto* queries = execution_db_ != nullptr
        ? execution_db_->WorkflowQueryService()
        : nullptr;
    auto* commands = execution_db_ != nullptr
        ? execution_db_->WorkflowCommandService()
        : nullptr;
    if (queries == nullptr || commands == nullptr) {
        return false;
    }

    const auto* descriptor = program_kind_registry_ != nullptr
        ? program_kind_registry_->FindForStepKind(snapshot.step_kind)
        : nullptr;
    if (descriptor == nullptr || descriptor->job_materializer == nullptr) {
        WorkflowReadyStepRecord step{};
        step.workflow_instance_id = snapshot.workflow_instance_id;
        step.workflow_step_id = snapshot.workflow_step_id;
        step.step_key = snapshot.step_key;
        step.step_kind = snapshot.step_kind;
        EmitWorkflowFailureEvent(
            step,
            "ContinueTerminalStep",
            "program job materializer is unavailable");
        return false;
    }

    WorkflowReadyStepRecord continuation_step{};
    continuation_step.workflow_instance_id = snapshot.workflow_instance_id;
    continuation_step.workflow_step_id = snapshot.workflow_step_id;
    continuation_step.workflow_unit_activation_id =
        snapshot.workflow_unit_activation_id;
    continuation_step.step_key = snapshot.step_key;
    continuation_step.graph_node_key = snapshot.graph_node_key;
    continuation_step.step_kind = snapshot.step_kind;
    continuation_step.priority = snapshot.priority;
    continuation_step.input_ref_kind = snapshot.input_ref_kind;
    continuation_step.input_ref_id = snapshot.input_ref_id;

    std::string error;
    auto materialization_context =
        BuildMaterializationContext(continuation_step, &error);
    if (!materialization_context.has_value()) {
        EmitWorkflowFailureEvent(
            continuation_step,
            "BuildContinuationContext",
            error.empty() ? "continuation context is unavailable" : error);
        return false;
    }

    programdb::ProgramJobContinuationResult continuation{};
    try {
        if (!descriptor->job_materializer->Continue(
                programdb::ProgramJobContinuationContext{
                    .materialization = std::move(*materialization_context),
                    .root_job_set_id = snapshot.job_set_id,
                    .expected_total = snapshot.expected_total,
                    .discovered_total = snapshot.discovered_total,
                    .terminal_total = snapshot.terminal_total,
                    .failed_total = snapshot.failed_total,
                },
                &continuation,
                &error)) {
            EmitWorkflowFailureEvent(
                continuation_step,
                "ContinueTerminalStep",
                error.empty()
                    ? "program continuation failed"
                    : error);
            return false;
        }
    } catch (const std::exception& exception) {
        EmitWorkflowFailureEvent(
            continuation_step,
            "ContinueTerminalStep",
            std::string("program continuation threw: ")
                + exception.what());
        return false;
    } catch (...) {
        EmitWorkflowFailureEvent(
            continuation_step,
            "ContinueTerminalStep",
            "program continuation threw an unknown exception");
        return false;
    }
    ++continuation_count_;
    for (const auto& line : continuation.event_lines) {
        EmitEventLine(line);
    }

    if (continuation.disposition
        == programdb::ProgramJobContinuationDisposition::AddedWork) {
        ++continuation_added_work_count_;
        return true;
    }

    if (continuation.disposition
        == programdb::ProgramJobContinuationDisposition::Failed) {
        const auto failure_code =
            continuation.failure_code.value_or(
                "PROGRAM_CONTINUATION_FAILED");
        const auto failure_text =
            continuation.failure_text.value_or(
                "program continuation rejected the completed work");
        if (!commands->TerminalFailWorkflowInstance(
                {
                    .workflow_instance_id =
                        snapshot.workflow_instance_id,
                    .workflow_step_id = snapshot.workflow_step_id,
                    .failure_code = failure_code,
                    .failure_message = failure_text,
                    .requested_by =
                        "workflow_coordinator_program_continuation",
                },
                &error)) {
            EmitWorkflowFailureEvent(
                continuation_step,
                "FailTerminalContinuation",
                error.empty()
                    ? "failed committing program continuation failure"
                    : error);
            return false;
        }
        ++continuation_failure_count_;
        ++terminal_step_count_;
        ++terminal_failed_step_count_;
        ++workflow_failed_count_;
        return true;
    }

    std::optional<programdb::ProgramJobContinuationOutput> transition_output;
    if (continuation.output.has_value()) {
        const auto& output = *continuation.output;
        if (output.output_key.empty()
            || output.data_kind.empty()
            || output.ref_kind.empty()
            || output.ref_id <= 0) {
            EmitWorkflowFailureEvent(
                continuation_step,
                "RecordContinuationOutput",
                "program continuation returned an invalid output");
            return false;
        }
        if (!commands->RecordStepOutput(
                {
                    .workflow_step_id = snapshot.workflow_step_id,
                    .output_key = output.output_key,
                    .output_data_kind = output.data_kind,
                    .output_ref_kind = output.ref_kind,
                    .output_ref_id = output.ref_id,
                    .requested_by =
                        "workflow_coordinator_program_continuation",
                },
                &error)) {
            EmitWorkflowFailureEvent(
                continuation_step,
                "RecordContinuationOutput",
                error.empty()
                    ? "failed recording program continuation output"
                    : error);
            return false;
        }
        transition_output = output;
    }

    WorkflowGraphRoutingService graph_routing(
        execution_db_,
        authoring_db_,
        queries,
        commands,
        config_.successor_step_priority_boost);
    WorkflowTerminalAdvancementService terminal_advancement(
        program_kind_registry_,
        step_completion_gate_,
        execution_db_,
        queries,
        commands,
        authoring_db_ != nullptr ? &graph_routing : nullptr,
        config_.successor_step_priority_boost);
    WorkflowTerminalAdvancementResult advancement{};
    if (!terminal_advancement.AdvanceSnapshot(
            snapshot,
            &advancement,
            &error,
            std::move(transition_output))) {
        WorkflowReadyStepRecord step{};
        step.workflow_instance_id = snapshot.workflow_instance_id;
        step.workflow_step_id = snapshot.workflow_step_id;
        step.step_key = snapshot.step_key;
        step.step_kind = snapshot.step_kind;
        EmitWorkflowFailureEvent(
            step,
            failure_stage != nullptr
                ? failure_stage
                : "AdvanceTerminalStep",
            error.empty() ? "unknown error" : error);
        return false;
    }

    ++terminal_step_count_;
    if (snapshot.discovered_total == 0) {
        ++terminal_empty_step_count_;
    } else if (snapshot.failed_total > 0) {
        ++terminal_failed_step_count_;
    } else {
        ++terminal_completed_step_count_;
    }
    if (advancement.advanced_next_step
        || advancement.spawned_step_count > 0) {
        ++transition_advanced_count_;
    }
    if (advancement.workflow_completed) {
        ++workflow_completed_count_;
    }
    if (advancement.workflow_failed) {
        ++workflow_failed_count_;
    }
    return true;
}

bool WorkflowCoordinatorService::ProcessReadyWorkflowStep(const WorkflowReadyStepRecord& step) {
    if (program_kind_registry_ == nullptr
        || !program_kind_registry_->HasRequiredAdaptersForStepKind(
            step.step_kind)) {
        auto* commands = execution_db_ != nullptr
            ? execution_db_->WorkflowCommandService()
            : nullptr;
        if (commands == nullptr) {
            return false;
        }
        std::string error;
        if (!commands->MarkStepBlocked(
                {
                    .workflow_step_id = step.workflow_step_id,
                    .blocked_reason = "DESCRIPTOR_UNAVAILABLE",
                    .requested_by =
                        "workflow_coordinator_descriptor_unavailable",
                },
                &error)) {
            EmitWorkflowFailureEvent(
                step,
                "BlockDescriptorUnavailableStep",
                error.empty() ? "failed marking descriptor unavailable"
                              : error);
            return false;
        }
        ++descriptor_unavailable_count_;
        return true;
    }

    return MaterializeWorkflowStep(step);
}

std::optional<programdb::ProgramJobMaterializationContext>
WorkflowCoordinatorService::BuildMaterializationContext(
    const WorkflowReadyStepRecord& step,
    std::string* error_out) const {
    if (execution_db_ == nullptr) {
        if (error_out != nullptr) {
            *error_out =
                "workflow materialization dependencies are unavailable";
        }
        return std::nullopt;
    }

    if (execution_db_->WorkflowQueryService() == nullptr) {
        if (error_out != nullptr) {
            *error_out = "workflow graph query service is unavailable";
        }
        return std::nullopt;
    }

    const auto graph = execution_db_->WorkflowQueryService()->GetWorkflowGraph(step.workflow_instance_id);
    if (!graph.has_value()) {
        if (error_out != nullptr) {
            *error_out = "workflow graph snapshot is unavailable";
        }
        return std::nullopt;
    }

    programdb::WorkflowGraphStepScheduleContext context{};
    context.workflow_instance_id = step.workflow_instance_id;
    context.workflow_step_id = step.workflow_step_id;
    context.workflow_graph_revision_id = graph->instance.workflow_graph_revision_id;
    context.step_key = step.step_key;
    context.step_kind = step.step_kind;
    context.workflow_unit_activation_id = step.workflow_unit_activation_id;
    context.step_priority = step.priority;
    const WorkflowUnitActivationRecord* owning_activation = nullptr;
    if (step.workflow_unit_activation_id.has_value()) {
        for (const auto& activation : graph->unit_activations) {
            if (activation.workflow_unit_activation_id == *step.workflow_unit_activation_id) {
                owning_activation = &activation;
                break;
            }
        }
    }
    if (owning_activation == nullptr) {
        for (const auto& activation : graph->unit_activations) {
            if (activation.graph_node_key == step.step_key || activation.activation_key == step.step_key) {
                owning_activation = &activation;
                break;
            }
        }
    }
    if (owning_activation != nullptr) {
        context.activation_key = owning_activation->activation_key;
        context.activation_graph_node_key = owning_activation->graph_node_key;
        context.unit_kind = owning_activation->unit_kind;
        context.activation_params_json = owning_activation->activation_params_json;
        context.authored_ref_kind = owning_activation->authored_ref_kind;
        context.authored_ref_id = owning_activation->authored_ref_id;
        const auto units = BuildDefaultWorkflowUnitRegistry();
        const auto* unit = units.Find(owning_activation->unit_kind);
        if (unit != nullptr) {
            context.unit_variant = unit->unit_variant;
            context.breakpoint_profile_key = unit->breakpoint_profile_key;
        }
    }
    const auto expected_node_key = context.activation_graph_node_key.empty()
        ? step.step_key
        : context.activation_graph_node_key;
    for (const auto& binding : graph->input_bindings) {
        if (binding.node_key != expected_node_key) {
            continue;
        }
        context.input_bindings.push_back(
            programdb::WorkflowGraphInputBinding{
                .node_key = binding.node_key,
                .input_key = binding.input_key,
                .data_kind = binding.data_kind,
                .ref_kind = binding.ref_kind,
                .ref_id = binding.ref_id,
                .source_kind = binding.source_kind,
            });
    }
    for (const auto& argument : graph->arguments) {
        if (!argument.node_key.empty() && argument.node_key != expected_node_key) {
            continue;
        }
        context.arguments.push_back(
            programdb::WorkflowGraphArgument{
                .node_key = argument.node_key,
                .argument_key = argument.argument_key,
                .value_type = argument.value_type,
                .integer_value = argument.integer_value,
                .text_value = argument.text_value,
                .source_kind = argument.source_kind,
            });
    }
    if (error_out != nullptr) {
        error_out->clear();
    }
    return programdb::ProgramJobMaterializationContext{
        .step =
            programdb::WorkflowStepScheduleContext{
                .workflow_instance_id = step.workflow_instance_id,
                .workflow_step_id = step.workflow_step_id,
                .step_key = step.step_key,
                .step_kind = step.step_kind,
                .domain_ref_kind = step.input_ref_kind,
                .domain_ref_id = step.input_ref_id.value_or(0),
                .step_priority = step.priority,
            },
        .graph = std::move(context),
    };
}

std::optional<programdb::WorkflowStepScheduleResult>
WorkflowCoordinatorService::ScheduleReadyStep(
    const WorkflowReadyStepRecord& step,
    std::string* error_out) const {
    if (program_kind_registry_ == nullptr) {
        if (error_out != nullptr) {
            *error_out =
                "workflow materialization dependencies are unavailable";
        }
        return std::nullopt;
    }
    const auto* descriptor =
        program_kind_registry_->FindForStepKind(step.step_kind);
    if (descriptor == nullptr || descriptor->job_materializer == nullptr) {
        if (error_out != nullptr) {
            *error_out =
                "program job materializer is unavailable for step kind "
                + step.step_kind;
        }
        return std::nullopt;
    }

    auto context = BuildMaterializationContext(step, error_out);
    if (!context.has_value()) {
        return std::nullopt;
    }
    programdb::WorkflowStepScheduleResult result{};
    try {
        if (!descriptor->job_materializer->Materialize(
                *context,
                &result,
                error_out)) {
            return std::nullopt;
        }
    } catch (const std::exception& exception) {
        if (error_out != nullptr) {
            *error_out =
                std::string("program job materializer threw: ")
                + exception.what();
        }
        return std::nullopt;
    } catch (...) {
        if (error_out != nullptr) {
            *error_out =
                "program job materializer threw an unknown exception";
        }
        return std::nullopt;
    }
    if (error_out != nullptr) {
        error_out->clear();
    }
    return result;
}

bool WorkflowCoordinatorService::MaterializeWorkflowStep(const WorkflowReadyStepRecord& step) {
    const auto started = std::chrono::steady_clock::now();
    std::string schedule_error;
    const auto scheduled = ScheduleReadyStep(step, &schedule_error);
    if (!scheduled.has_value() || scheduled->root_job_set_id <= 0) {
        ++materialization_failure_count_;
        if (scheduled.has_value()) {
            for (const auto& line : scheduled->event_lines) {
                EmitEventLine(line);
            }
        }
        const std::string failure_reason = !schedule_error.empty()
            ? std::move(schedule_error)
            : scheduled.has_value()
            ? "schedule result did not include a job set"
            : "no schedule result";
        EmitWorkflowFailureEvent(step, "MaterializeWorkflowStep", failure_reason);
        MaybeTerminalFailStepInStrictSmokeMode(
            step,
            "workflow_coordinator_materialize_strict_smoke",
            failure_reason);
        return false;
    }

    auto* commands = execution_db_ != nullptr ? execution_db_->WorkflowCommandService() : nullptr;
    if (commands == nullptr) {
        return false;
    }

    std::string error;
    if (!commands->MarkStepMaterialized(
        {
            .workflow_step_id = step.workflow_step_id,
            .job_set_id = scheduled->root_job_set_id,
            .input_ref_kind = scheduled->persistence.program_ref_kind.empty()
                ? std::nullopt
                : std::optional<std::string>(scheduled->persistence.program_ref_kind),
            .input_ref_id = scheduled->persistence.program_ref_id > 0
                ? std::optional<std::int64_t>(scheduled->persistence.program_ref_id)
                : std::nullopt,
            .requested_by = "workflow_coordinator_materialize",
        },
        &error)) {
        ++materialization_failure_count_;
        const std::string failure_reason = error.empty() ? "unknown error" : error;
        EmitWorkflowFailureEvent(step, "MarkStepMaterialized", failure_reason);
        MaybeTerminalFailStepInStrictSmokeMode(
            step,
            "workflow_coordinator_materialize_strict_smoke",
            "failed to mark materialized: " + failure_reason);
        return false;
    }

    for (const auto& line : scheduled->event_lines) {
        EmitEventLine(line);
    }

    if (execution_db_ != nullptr) {
        const auto details = execution_db_->GetJobSetProgress(scheduled->root_job_set_id);
        if (details.has_value()) {
            std::ostringstream line;
            line << "[workflow-materialization-counts]"
                 << " step=" << step.step_key
                 << " kind=" << step.step_kind
                 << " workflow_step_id=" << step.workflow_step_id
                 << " job_set=" << scheduled->root_job_set_id
                 << " total=" << details->total_jobs
                 << " done=" << details->completed_jobs;
            if (details->expected_total.has_value()) {
                line << " expected_total=" << *details->expected_total;
            }
            EmitEventLine(line.str());
        }
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    last_materialization_latency_ms_.store(static_cast<std::int64_t>(elapsed));
    StoreMax(max_materialization_latency_ms_, static_cast<std::int64_t>(elapsed));
    ++materialization_count_;
    return true;
}

void WorkflowCoordinatorService::EmitWorkflowFailureEvent(
    const WorkflowReadyStepRecord& step,
    const std::string& stage,
    const std::string& reason) const {
    auto* commands = execution_db_ != nullptr ? execution_db_->WorkflowCommandService() : nullptr;
    if (commands == nullptr) {
        return;
    }
    std::ostringstream detail;
    detail << "stage=" << stage << ";reason=" << reason;
    std::string error;
    (void)commands->AppendLifecycleEvent(
        {
            .workflow_instance_id = step.workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .event_kind = "Execution.WorkflowStepCoordinatorFailure.v1",
            .message = detail.str(),
            .requested_by = "workflow_coordinator",
        },
        &error);
}

void WorkflowCoordinatorService::MaybeTerminalFailStepInStrictSmokeMode(
    const WorkflowReadyStepRecord& step,
    const std::string& requested_by,
    const std::string& failure_reason) const {
    if (!config_.strict_smoke_terminal_on_failure) {
        return;
    }
    auto* commands = execution_db_ != nullptr ? execution_db_->WorkflowCommandService() : nullptr;
    if (commands == nullptr) {
        return;
    }
    std::string failure_message = "workflow step '" + step.step_key + "' materialization failed";
    if (!failure_reason.empty()) {
        failure_message += ": " + failure_reason;
    }
    std::string error;
    if (!commands->TerminalFailWorkflowInstance(
        {
            .workflow_instance_id = step.workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .failure_code = "WORKFLOW_STEP_MATERIALIZATION_FAILED",
            .failure_message = failure_message,
            .requested_by = requested_by,
        },
        &error)) {
        EmitWorkflowFailureEvent(
            step,
            "StrictSmokeTerminalFailWorkflowInstance",
            error.empty() ? "unknown error" : error);
    }
}

void WorkflowCoordinatorService::EmitEventLine(const std::string& line) const {
    if (event_line_callback_) {
        event_line_callback_(line);
    }
}

} // namespace savor::db::execution::workflow
