#include "WorkflowCoordinatorService.h"

#include "WorkflowComposition.h"
#include "WorkflowGraphRoutingService.h"
#include "WorkflowTerminalAdvancementService.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>

namespace simcore::db::execution::workflow {
namespace {

bool IsSeedProbeKind(const std::string& step_kind) {
    constexpr const char* kPrefix = "seedprobe.";
    return step_kind.rfind(kPrefix, 0) == 0;
}

bool IsNoWorkWorkflowStep(const WorkflowReadyStepRecord& step) {
    return step.step_kind == "seedprobe.done";
}

void StoreMax(std::atomic<std::int64_t>& target, std::int64_t value) {
    auto current = target.load();
    while (value > current && !target.compare_exchange_weak(current, value)) {
    }
}

} // namespace

WorkflowCoordinatorService::WorkflowCoordinatorService(
    simcore::db::IExecutionDb* execution_db,
    const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    WorkflowCoordinatorConfig config,
    EventLineCallback event_line_callback,
    StepCompletionGateService* step_completion_gate,
    simcore::db::IAuthoringDb* authoring_db)
    : execution_db_(execution_db)
    , authoring_db_(authoring_db)
    , program_kind_registry_(program_kind_registry)
    , config_(config)
    , event_line_callback_(std::move(event_line_callback))
    , adapter_chain_orchestrator_(program_kind_registry, nullptr) {
    if (step_completion_gate != nullptr) {
        step_completion_gate_ = step_completion_gate;
    } else {
        owned_step_completion_gate_ = std::make_unique<StepCompletionGateService>();
        step_completion_gate_ = owned_step_completion_gate_.get();
    }
    adapter_chain_orchestrator_ = AdapterChainOrchestrator(program_kind_registry_, step_completion_gate_);
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
    telemetry.input_complete_count = input_complete_count_.load();
    telemetry.input_timeout_count = input_timeout_count_.load();
    telemetry.terminal_input_failure_count = terminal_input_failure_count_.load();
    telemetry.last_input_latency_ms = last_input_latency_ms_.load();
    telemetry.materialization_count = materialization_count_.load();
    telemetry.materialization_failure_count = materialization_failure_count_.load();
    telemetry.last_materialization_latency_ms = last_materialization_latency_ms_.load();
    telemetry.max_materialization_latency_ms = max_materialization_latency_ms_.load();
    telemetry.terminal_scan_count = terminal_scan_count_.load();
    telemetry.terminal_step_count = terminal_step_count_.load();
    telemetry.terminal_empty_step_count = terminal_empty_step_count_.load();
    telemetry.terminal_failed_step_count = terminal_failed_step_count_.load();
    telemetry.terminal_completed_step_count = terminal_completed_step_count_.load();
    telemetry.transition_advanced_count = transition_advanced_count_.load();
    telemetry.workflow_completed_count = workflow_completed_count_.load();
    telemetry.workflow_failed_count = workflow_failed_count_.load();
    return telemetry;
}

void WorkflowCoordinatorService::Loop() {
    while (!stop_.load()) {
        const bool advanced = AdvanceAvailableWork();
        if (!advanced) {
            std::unique_lock<std::mutex> lock(wait_mtx_);
            wait_cv_.wait_for(lock, config_.poll_interval);
        }
    }
}

bool WorkflowCoordinatorService::AdvanceAvailableWork() {
    bool advanced = false;
    advanced = ReconcileTerminalWorkflowSteps() || advanced;
    advanced = PollReadyStepsFromDb() || advanced;
    return advanced;
}

bool WorkflowCoordinatorService::PollReadyStepsFromDb() {
    const auto t0 = std::chrono::steady_clock::now();
    auto* queries = execution_db_ != nullptr ? execution_db_->WorkflowQueryService() : nullptr;
    if (queries == nullptr) {
        ++ready_scan_count_;
        return false;
    }

    const auto ready_steps = queries->ListReadySteps(config_.ready_scan_limit);
    bool processed_any = false;
    for (const auto& step : ready_steps) {
        if (seen_workflow_instance_ids_.emplace(step.workflow_instance_id).second) {
            ++workflow_created_seen_count_;
        }
        ++ready_steps_seen_;
        processed_any = ProcessReadyWorkflowStep(step) || processed_any;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    last_ready_scan_latency_ms_.store(static_cast<std::int64_t>(elapsed));
    ++ready_scan_count_;
    return processed_any;
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

    WorkflowGraphRoutingService graph_routing(
        execution_db_,
        authoring_db_,
        queries,
        commands);
    WorkflowTerminalAdvancementService terminal_advancement(
        &adapter_chain_orchestrator_,
        execution_db_,
        queries,
        commands,
        authoring_db_ != nullptr ? &graph_routing : nullptr);

    bool advanced_any = false;
    for (const auto& snapshot : snapshots) {
        WorkflowTerminalAdvancementResult advancement{};
        std::string error;
        if (!terminal_advancement.AdvanceSnapshot(snapshot, &advancement, &error)) {
            WorkflowReadyStepRecord step{};
            step.workflow_instance_id = snapshot.workflow_instance_id;
            step.workflow_step_id = snapshot.workflow_step_id;
            step.step_key = snapshot.step_key;
            step.step_kind = snapshot.step_kind;
            EmitWorkflowFailureEvent(
                step,
                "AdvanceTerminalStep",
                error.empty() ? "unknown error" : error);
            continue;
        }

        ++terminal_step_count_;
        if (snapshot.discovered_total == 0) {
            ++terminal_empty_step_count_;
        } else if (snapshot.failed_total > 0) {
            ++terminal_failed_step_count_;
        } else {
            ++terminal_completed_step_count_;
        }
        if (advancement.advanced_next_step || advancement.spawned_step_count > 0) {
            ++transition_advanced_count_;
        }
        if (advancement.workflow_completed) {
            ++workflow_completed_count_;
        }
        if (advancement.workflow_failed) {
            ++workflow_failed_count_;
        }
        advanced_any = true;
    }
    return advanced_any;
}

bool WorkflowCoordinatorService::ProcessReadyWorkflowStep(const WorkflowReadyStepRecord& step) {
    if (IsNoWorkWorkflowStep(step)) {
        return CompleteNoWorkWorkflowStep(step);
    }

    const auto aggregation = EvaluateInputAggregation(step, std::chrono::steady_clock::now());
    if (!aggregation.input_complete) {
        if (aggregation.timed_out) {
            ++input_timeout_count_;
        }
        if (aggregation.terminal_failure_ready) {
            ++terminal_input_failure_count_;
            auto* commands = execution_db_ != nullptr ? execution_db_->WorkflowCommandService() : nullptr;
            if (commands != nullptr) {
                std::string error;
                (void)commands->MarkStepTerminal(
                    {
                        .workflow_step_id = step.workflow_step_id,
                        .terminal_state = "FAILED",
                        .requested_by = "workflow_input_aggregation_timeout",
                    },
                    &error);
            }
            return true;
        }
        return false;
    }

    ++input_complete_count_;
    if (aggregation.input_latency_ms.has_value()) {
        last_input_latency_ms_.store(*aggregation.input_latency_ms);
    }
    return MaterializeWorkflowStep(step);
}

bool WorkflowCoordinatorService::CompleteNoWorkWorkflowStep(const WorkflowReadyStepRecord& step) {
    auto* commands = execution_db_ != nullptr ? execution_db_->WorkflowCommandService() : nullptr;
    if (commands == nullptr) {
        return false;
    }

    std::string error;
    if (!commands->MarkStepTerminal(
        {
            .workflow_step_id = step.workflow_step_id,
            .terminal_state = "COMPLETED",
            .requested_by = "workflow_no_work_step",
        },
        &error)) {
        EmitWorkflowFailureEvent(step, "CompleteNoWorkStep", error.empty() ? "mark terminal failed" : error);
        return false;
    }

    if (!commands->AppendLifecycleEvent(
        {
            .workflow_instance_id = step.workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .event_kind = "Execution.WorkflowTransitionEvaluated.v1",
            .message = std::optional<std::string>("no_work_step"),
            .requested_by = "workflow_no_work_step",
        },
        &error)) {
        EmitWorkflowFailureEvent(step, "CompleteNoWorkStep", error.empty() ? "transition event failed" : error);
        return false;
    }

    if (!commands->CompleteWorkflowInstance(
        {
            .workflow_instance_id = step.workflow_instance_id,
            .requested_by = "workflow_no_work_step",
        },
        &error)) {
        EmitWorkflowFailureEvent(step, "CompleteNoWorkStep", error.empty() ? "complete workflow failed" : error);
        return false;
    }
    ++workflow_completed_count_;
    return true;
}

std::optional<programdb::WorkflowStepScheduleResult> WorkflowCoordinatorService::ScheduleReadyStep(
    const WorkflowReadyStepRecord& step) const {
    if (program_kind_registry_ == nullptr || execution_db_ == nullptr) {
        return std::nullopt;
    }
    const auto* descriptor = program_kind_registry_->FindForStepKind(step.step_kind);
    if (descriptor == nullptr) {
        return std::nullopt;
    }

    if (step.input_ref_id.has_value()
        && descriptor->job_persistence != nullptr
        && step.step_kind != "battle_chain") {
        return adapter_chain_orchestrator_.OnInputComplete(step.step_kind, *step.input_ref_id);
    }

    if (descriptor->graph_job_persistence == nullptr || execution_db_->WorkflowQueryService() == nullptr) {
        return std::nullopt;
    }

    const auto graph = execution_db_->WorkflowQueryService()->GetWorkflowGraph(step.workflow_instance_id);
    if (!graph.has_value()) {
        return std::nullopt;
    }

    programdb::WorkflowGraphStepScheduleContext context{};
    context.workflow_instance_id = step.workflow_instance_id;
    context.workflow_step_id = step.workflow_step_id;
    context.workflow_graph_revision_id = graph->instance.workflow_graph_revision_id;
    context.step_key = step.step_key;
    context.step_kind = step.step_kind;
    context.workflow_unit_activation_id = step.workflow_unit_activation_id;
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
    if (step.input_ref_id.has_value() && *step.input_ref_id > 0) {
        if (step.step_kind == "seed_probe_chain" && step.input_ref_kind == "state.savestate") {
            context.input_bindings.push_back(
                programdb::WorkflowGraphInputBinding{
                    .node_key = expected_node_key,
                    .input_key = "entry_savestate",
                    .data_kind = "state.savestate_id",
                    .ref_kind = *step.input_ref_kind,
                    .ref_id = *step.input_ref_id,
                    .source_kind = "upstream",
                });
        } else if (step.step_kind == "battle_chain"
            && (*step.input_ref_kind == "an.input_set" || *step.input_ref_kind == "au.input_set")) {
            context.input_bindings.push_back(
                programdb::WorkflowGraphInputBinding{
                    .node_key = expected_node_key,
                    .input_key = "initial_input_frames",
                    .data_kind = "analysis.input_frame_set_id",
                    .ref_kind = *step.input_ref_kind,
                    .ref_id = *step.input_ref_id,
                    .source_kind = "upstream",
                });
        }
    }
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
    return adapter_chain_orchestrator_.OnGraphInputComplete(step.step_kind, context);
}

bool WorkflowCoordinatorService::MaterializeWorkflowStep(const WorkflowReadyStepRecord& step) {
    const auto started = std::chrono::steady_clock::now();
    const auto scheduled = ScheduleReadyStep(step);
    if (!scheduled.has_value() || scheduled->root_job_set_id <= 0) {
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
        EmitWorkflowFailureEvent(step, "MarkStepMaterialized", error.empty() ? "unknown error" : error);
        MaybeTerminalFailStepInStrictSmokeMode(step, "workflow_coordinator_materialize_strict_smoke");
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

WorkflowCoordinatorService::StepInputAggregationStatus WorkflowCoordinatorService::EvaluateInputAggregation(
    const WorkflowReadyStepRecord& step,
    std::chrono::steady_clock::time_point now) {
    StepInputAggregationStatus status{};
    if (!IsSeedProbePilotStep(step)) {
        status.input_complete = true;
        status.input_latency_ms = 0;
        return status;
    }

    const auto key = InputContextKey(step);
    auto [it, inserted] = input_contexts_.try_emplace(key);
    auto& ctx = it->second;
    if (inserted) {
        ctx.workflow_instance_id = step.workflow_instance_id;
        ctx.step_key = step.step_key;
        ctx.required_inputs = RequiredInputsFor(step);
        ctx.started_at = now;
        ctx.deadline = now + config_.input_timeout;
        for (const auto& source : ctx.required_inputs) {
            const auto request_id = key + ":" + source + ":retry" + std::to_string(ctx.timeout_retries);
            ctx.pending_request_ids_by_source[source] = request_id;
            (void)EmitInputEvent(
                step,
                "Execution.WorkflowStepInputRequested.v1",
                source,
                request_id,
                std::optional<std::string>("all-inputs-required"));
        }

        const auto sync_it = ctx.pending_request_ids_by_source.find("sync");
        if (sync_it != ctx.pending_request_ids_by_source.end()) {
            (void)SubmitInputFragment(step, "sync", sync_it->second, now);
        }
    }

    if (!ctx.async_fragment_simulated) {
        const auto async_it = ctx.pending_request_ids_by_source.find("async");
        if (async_it != ctx.pending_request_ids_by_source.end()) {
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - ctx.started_at);
            if (age >= std::chrono::milliseconds(5)) {
                (void)SubmitInputFragment(step, "async", async_it->second, now);
                ctx.async_fragment_simulated = true;
            }
        }
    }

    const bool all_ready = std::all_of(
        ctx.required_inputs.begin(),
        ctx.required_inputs.end(),
        [&](const std::string& source) {
            return ctx.ready_sources.find(source) != ctx.ready_sources.end();
        });
    if (all_ready) {
        status.input_complete = true;
        if (!ctx.input_complete_emitted) {
            (void)EmitInputEvent(
                step,
                "Execution.WorkflowStepInputComplete.v1",
                std::nullopt,
                std::nullopt,
                std::optional<std::string>("all-required-inputs-ready"));
            ctx.input_complete_emitted = true;
        }
        status.input_latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - ctx.started_at).count();
        input_contexts_.erase(key);
        return status;
    }

    if (now >= ctx.deadline) {
        status.timed_out = true;
        if (ctx.timeout_retries < config_.input_timeout_retries) {
            ++ctx.timeout_retries;
            ctx.deadline = now + config_.input_timeout;
            ctx.async_fragment_simulated = false;
            for (const auto& source : ctx.required_inputs) {
                const auto request_id = key + ":" + source + ":retry" + std::to_string(ctx.timeout_retries);
                ctx.pending_request_ids_by_source[source] = request_id;
                (void)EmitInputEvent(
                    step,
                    "Execution.WorkflowStepInputRequested.v1",
                    source,
                    request_id,
                    std::optional<std::string>("retry-on-timeout"));
            }
        } else {
            status.terminal_failure_ready = true;
            status.input_latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - ctx.started_at).count();
            input_contexts_.erase(key);
        }
    }

    return status;
}

bool WorkflowCoordinatorService::SubmitInputFragment(
    const WorkflowReadyStepRecord& step,
    const std::string& source_key,
    const std::optional<std::string>& request_id,
    std::chrono::steady_clock::time_point now) {
    const auto key = InputContextKey(step);
    const auto ctx_it = input_contexts_.find(key);
    if (ctx_it == input_contexts_.end()) {
        return false;
    }

    auto& ctx = ctx_it->second;
    if (ctx.ready_sources.find(source_key) != ctx.ready_sources.end()) {
        return true;
    }

    const auto pending_it = ctx.pending_request_ids_by_source.find(source_key);
    if (pending_it == ctx.pending_request_ids_by_source.end()) {
        return false;
    }
    if (request_id.has_value() && pending_it->second != *request_id) {
        return false;
    }

    ctx.ready_sources.insert(source_key);
    (void)EmitInputEvent(
        step,
        "Execution.WorkflowStepInputFragmentReady.v1",
        source_key,
        pending_it->second,
        std::optional<std::string>("fragment-ready"));
    (void)now;
    return true;
}

std::string WorkflowCoordinatorService::InputContextKey(const WorkflowReadyStepRecord& step) const {
    return std::to_string(step.workflow_instance_id) + ":" + step.step_key;
}

bool WorkflowCoordinatorService::IsSeedProbePilotStep(const WorkflowReadyStepRecord& step) const {
    return IsSeedProbeKind(step.step_kind);
}

std::vector<std::string> WorkflowCoordinatorService::RequiredInputsFor(const WorkflowReadyStepRecord& step) const {
    if (IsSeedProbePilotStep(step)) {
        return { "sync", "async" };
    }
    return {};
}

bool WorkflowCoordinatorService::EmitInputEvent(
    const WorkflowReadyStepRecord& step,
    const std::string& event_kind,
    const std::optional<std::string>& source_key,
    const std::optional<std::string>& request_id,
    const std::optional<std::string>& message) {
    auto* commands = execution_db_ != nullptr ? execution_db_->WorkflowCommandService() : nullptr;
    if (commands == nullptr) {
        return false;
    }

    std::ostringstream detail;
    if (source_key.has_value()) {
        detail << "source=" << *source_key;
    }
    if (request_id.has_value()) {
        if (detail.tellp() > 0) {
            detail << ";";
        }
        detail << "request_id=" << *request_id;
    }
    if (message.has_value()) {
        if (detail.tellp() > 0) {
            detail << ";";
        }
        detail << "message=" << *message;
    }

    std::string error;
    return commands->AppendLifecycleEvent(
        {
            .workflow_instance_id = step.workflow_instance_id,
            .workflow_step_id = step.workflow_step_id,
            .event_kind = event_kind,
            .message = detail.str().empty() ? std::nullopt : std::optional<std::string>(detail.str()),
            .requested_by = "workflow_input_aggregation",
        },
        &error);
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
    const std::string& requested_by) const {
    if (!config_.strict_smoke_terminal_on_failure) {
        return;
    }
    auto* commands = execution_db_ != nullptr ? execution_db_->WorkflowCommandService() : nullptr;
    if (commands == nullptr) {
        return;
    }
    std::string error;
    (void)commands->MarkStepTerminal(
        {
            .workflow_step_id = step.workflow_step_id,
            .terminal_state = "FAILED",
            .requested_by = requested_by,
        },
        &error);
}

void WorkflowCoordinatorService::EmitEventLine(const std::string& line) const {
    if (event_line_callback_) {
        event_line_callback_(line);
    }
}

} // namespace simcore::db::execution::workflow
