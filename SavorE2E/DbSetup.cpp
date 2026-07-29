#include "DbSetup.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <sstream>
#include <utility>

#include "Common/Types/UtcTimestamp.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Execution/Workflow/WorkflowUnitActivationFactory.h"
#include "Tas/DtmFile.h"

namespace savor::e2e {

using savor::db::types::UtcNow;

namespace {

constexpr std::size_t kWorkflowBoundaryDiagnosticLimit = 256;

const char* WorkflowInstanceStateName(
    savor::db::execution::workflow::WorkflowInstanceState state) {
    using savor::db::execution::workflow::WorkflowInstanceState;
    switch (state) {
    case WorkflowInstanceState::Pending: return "PENDING";
    case WorkflowInstanceState::Running: return "RUNNING";
    case WorkflowInstanceState::Completed: return "COMPLETED";
    case WorkflowInstanceState::Failed: return "FAILED";
    case WorkflowInstanceState::Canceled: return "CANCELED";
    }
    return "UNKNOWN";
}

const char* WorkflowStepStateName(
    savor::db::execution::workflow::WorkflowStepState state) {
    using savor::db::execution::workflow::WorkflowStepState;
    switch (state) {
    case WorkflowStepState::Waiting: return "WAITING";
    case WorkflowStepState::Ready: return "READY";
    case WorkflowStepState::Materialized: return "MATERIALIZED";
    case WorkflowStepState::Running: return "RUNNING";
    case WorkflowStepState::Completed: return "COMPLETED";
    case WorkflowStepState::Failed: return "FAILED";
    case WorkflowStepState::Skipped: return "SKIPPED";
    }
    return "UNKNOWN";
}

} // namespace

void AppendTasMovieRtcArgumentIfSingle(
    savor::db::execution::workflow::WorkflowCreateInstanceCommand* command,
    const CliOptions& options,
    int default_value) {
    if (command == nullptr) {
        return;
    }
    command->arguments.push_back({
        .node_key = "tas_1",
        .argument_key = "headroom",
        .value_type = "integer",
        .integer_value = options.tasmovie_headroom_x10.value_or(15),
        .source_kind = "scenario",
    });
    const auto range = ResolveTasMovieRtcRange(options, default_value);
    if (range.low != range.high) {
        return;
    }
    command->arguments.push_back({
        .node_key = "tas_1",
        .argument_key = "rtc",
        .value_type = "integer",
        .integer_value = range.low,
        .source_kind = "scenario",
    });
}

savor::db::DbConfigPaths BuildDbPaths(const CliOptions& options) {
    const auto root = options.workspace_root.value_or(std::filesystem::temp_directory_path() / "savor-e2e-default");
    const bool reuse_existing_database = std::find(
        options.scenarios.begin(),
        options.scenarios.end(),
        "battle_end") != options.scenarios.end()
        || std::find(
            options.scenarios.begin(),
            options.scenarios.end(),
            "battle_end_results") != options.scenarios.end()
        || std::find(
            options.scenarios.begin(),
            options.scenarios.end(),
            "navigation_context") != options.scenarios.end();

    std::error_code ec;
    if (!reuse_existing_database && std::filesystem::exists(root)) {
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (ec) {
                break;
            }
            if (entry.path().filename() == "log") {
                continue;
            }
            std::filesystem::remove_all(entry.path(), ec);
            if (ec) {
                break;
            }
        }
    }

    std::filesystem::create_directories(root, ec);
    std::filesystem::create_directories(root / "log", ec);
    std::filesystem::create_directories(root / "object_store", ec);
    std::filesystem::create_directories(root / "archive_store", ec);

    return savor::db::DbConfigPaths{
        .execution_db_path = root / "execution.db",
        .state_db_path = root / "state.db",
        .analysis_db_path = root / "analysis.db",
        .authoring_db_path = root / "authoring.db",
        .ui_read_db_path = root / "ui_read.db",
        .archive_db_path = root / "archive.db",
        .object_store_root = root / "object_store",
        .archive_store_root = root / "archive_store",
    };
}

bool CheckWorkflowQuiescence(
    savor::db::IExecutionDb* execution_db,
    std::string* diagnostics_out) {
    if (diagnostics_out != nullptr) {
        diagnostics_out->clear();
    }
    if (execution_db == nullptr) {
        if (diagnostics_out != nullptr) {
            *diagnostics_out = "execution DB is unavailable";
        }
        return false;
    }

    auto* queries = execution_db->WorkflowQueryService();
    if (queries == nullptr) {
        if (diagnostics_out != nullptr) {
            *diagnostics_out = "workflow query service is unavailable";
        }
        return false;
    }

    using savor::db::execution::workflow::WorkflowInstanceState;
    using savor::db::execution::workflow::WorkflowStepState;
    const auto oldest_timestamp = std::numeric_limits<std::int64_t>::lowest();
    const auto newest_timestamp = std::numeric_limits<std::int64_t>::max();
    const auto pending_instances = queries->ListWorkflowInstances(
        WorkflowInstanceState::Pending,
        oldest_timestamp,
        newest_timestamp);
    const auto running_instances = queries->ListWorkflowInstances(
        WorkflowInstanceState::Running,
        oldest_timestamp,
        newest_timestamp);
    const auto ready_steps = queries->ListReadySteps(kWorkflowBoundaryDiagnosticLimit);
    const auto active_materialized_workflows = queries->CountActiveMaterializedWorkflows();
    const auto terminal_ready_steps =
        queries->ListTerminalReadyStepSnapshots(kWorkflowBoundaryDiagnosticLimit);

    if (pending_instances.empty()
        && running_instances.empty()
        && ready_steps.empty()
        && active_materialized_workflows == 0
        && terminal_ready_steps.empty()) {
        return true;
    }

    std::ostringstream diagnostics;
    diagnostics << "workflow DB is not quiescent";

    const auto append_instances =
        [&diagnostics](const auto& instances) {
            for (const auto& instance : instances) {
                diagnostics
                    << "\n  workflow_instance_id=" << instance.workflow_instance_id
                    << " state=" << WorkflowInstanceStateName(instance.state)
                    << " workflow_kind='" << instance.workflow_kind << "'";
            }
        };
    append_instances(pending_instances);
    append_instances(running_instances);

    for (const auto& step : ready_steps) {
        diagnostics
            << "\n  workflow_instance_id=" << step.workflow_instance_id
            << " workflow_step_id=" << step.workflow_step_id
            << " state=READY"
            << " step_kind='" << step.step_kind << "'"
            << " step_key='" << step.step_key << "'";
    }
    if (ready_steps.size() == kWorkflowBoundaryDiagnosticLimit) {
        diagnostics << "\n  additional READY steps may exist";
    }

    if (active_materialized_workflows != 0) {
        diagnostics
            << "\n  active_materialized_workflow_count="
            << active_materialized_workflows;

        std::size_t detailed_step_count = 0;
        const auto append_materialized_steps =
            [&](const auto& instances) {
                for (const auto& instance : instances) {
                    const auto graph = queries->GetWorkflowGraph(instance.workflow_instance_id);
                    if (!graph.has_value()) {
                        continue;
                    }
                    for (const auto& step : graph->steps) {
                        if (step.state != WorkflowStepState::Materialized
                            && step.state != WorkflowStepState::Running) {
                            continue;
                        }
                        if (detailed_step_count >= kWorkflowBoundaryDiagnosticLimit) {
                            return;
                        }
                        diagnostics
                            << "\n  workflow_instance_id=" << step.workflow_instance_id
                            << " workflow_step_id=" << step.workflow_step_id
                            << " state=" << WorkflowStepStateName(step.state)
                            << " step_kind='" << step.step_kind << "'"
                            << " step_key='" << step.step_key << "'";
                        ++detailed_step_count;
                    }
                }
            };
        append_materialized_steps(pending_instances);
        append_materialized_steps(running_instances);
        if (detailed_step_count == kWorkflowBoundaryDiagnosticLimit) {
            diagnostics << "\n  additional MATERIALIZED/RUNNING steps may exist";
        }
    }

    for (const auto& snapshot : terminal_ready_steps) {
        diagnostics
            << "\n  workflow_instance_id=" << snapshot.workflow_instance_id
            << " workflow_step_id=" << snapshot.workflow_step_id
            << " state=TERMINAL_READY"
            << " workflow_kind='" << snapshot.workflow_kind << "'"
            << " step_kind='" << snapshot.step_kind << "'"
            << " job_set_id=" << snapshot.job_set_id;
    }
    if (terminal_ready_steps.size() == kWorkflowBoundaryDiagnosticLimit) {
        diagnostics << "\n  additional TERMINAL_READY steps may exist";
    }

    if (diagnostics_out != nullptr) {
        *diagnostics_out = diagnostics.str();
    }
    return false;
}

ScopedWorkflowCoordinatorService::~ScopedWorkflowCoordinatorService() {
    Stop();
}

bool ScopedWorkflowCoordinatorService::Start(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAuthoringDb* authoring_db,
    const savor::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    const CliOptions& options,
    std::string* error_out,
    EventLineCallback event_line_callback,
    bool strict_smoke_terminal_on_failure,
    std::shared_ptr<
        savor::db::execution::workflow::CoordinatorItemCreditSource>
        item_credit_source) {
    if (service_ != nullptr && service_->IsRunning()) {
        return true;
    }
    if (execution_db == nullptr || authoring_db == nullptr || program_kind_registry == nullptr) {
        if (error_out != nullptr) {
            *error_out = "workflow coordinator requires execution db, authoring db, and program registry";
        }
        return false;
    }

    savor::db::execution::workflow::WorkflowCoordinatorConfig config{};
    config.workflow_enabled = true;
    config.strict_smoke_terminal_on_failure = strict_smoke_terminal_on_failure;
    config.poll_interval = std::chrono::milliseconds(std::max<std::int64_t>(1, options.poll_ms));
    config.item_credit_source = std::move(item_credit_source);

    auto service = std::make_unique<savor::db::execution::workflow::WorkflowCoordinatorService>(
        execution_db,
        program_kind_registry,
        config,
        std::move(event_line_callback),
        nullptr,
        authoring_db);
    std::string err;
    if (!service->Start(&err)) {
        if (error_out != nullptr) {
            *error_out = "workflow coordinator startup failed: " + err;
        }
        return false;
    }
    service_ = std::move(service);
    return true;
}

void ScopedWorkflowCoordinatorService::Stop() {
    if (service_ != nullptr) {
        service_->Stop();
        service_.reset();
    }
}

bool ScopedWorkflowCoordinatorService::IsRunning() const {
    return service_ != nullptr && service_->IsRunning();
}

savor::db::execution::workflow::WorkflowCoordinatorTelemetry
ScopedWorkflowCoordinatorService::SnapshotTelemetry() const {
    return service_ != nullptr
        ? service_->SnapshotTelemetry()
        : savor::db::execution::workflow::WorkflowCoordinatorTelemetry{};
}

bool SeedStateSavestate(
    savor::db::IStateDb* state_db,
    const std::filesystem::path& savestate_file,
    std::int64_t* savestate_id_out,
    std::string* error_out) {
    if (state_db == nullptr || savestate_id_out == nullptr) {
        if (error_out) *error_out = "state db unavailable";
        return false;
    }

    std::int64_t artifact_id = 0;
    if (!state_db->StoreArtifact(
            {
                .sha256 = "savor-e2e-" + savestate_file.filename().string(),
                .size_bytes = static_cast<std::int64_t>(std::filesystem::file_size(savestate_file)),
                .compression_kind = 0,
                .filename = std::filesystem::absolute(savestate_file).string(),
                .file_ext = savestate_file.extension().string(),
                .artifact_kind = "SAV",
                .created_at_utc = UtcNow(),
                .correlation_id = "savor-e2e.seedprobe",
                .causation_id = "savor-e2e.seed",
            },
            &artifact_id,
            error_out)) {
        return false;
    }

    if (!state_db->CreateSavestate(
            {
                .artifact_id = artifact_id,
                .savestate_type = "TRANSITION",
                .note = "SavorE2E start state",
                .is_complete = true,
                .created_at_utc = UtcNow(),
                .correlation_id = "savor-e2e.seedprobe",
                .causation_id = "savor-e2e.seed",
            },
            savestate_id_out,
            error_out)) {
        return false;
    }

    return true;
}

bool SeedStateDtmArtifact(
    savor::db::IStateDb* state_db,
    const std::filesystem::path& dtm_file,
    std::int64_t* artifact_id_out,
    std::string* error_out) {
    if (state_db == nullptr || artifact_id_out == nullptr) {
        if (error_out) *error_out = "state db unavailable";
        return false;
    }

    savor::tas::DtmFile dtm;
    if (!dtm.load(dtm_file.string())) {
        if (error_out) *error_out = "failed loading DTM: " + dtm_file.string();
        return false;
    }

    return state_db->StoreArtifact(
        {
            .sha256 = dtm.compute_sha256(),
            .size_bytes = static_cast<std::int64_t>(std::filesystem::file_size(dtm_file)),
            .compression_kind = 0,
            .filename = std::filesystem::absolute(dtm_file).string(),
            .file_ext = dtm_file.extension().string(),
            .artifact_kind = "DTM",
            .created_at_utc = UtcNow(),
            .correlation_id = "savor-e2e.tasmovie",
            .causation_id = "savor-e2e.seed",
        },
        artifact_id_out,
        error_out);
}

bool SeedAuthoringSpec(
    savor::db::IAuthoringDb* authoring_db,
    const CliOptions& options,
    std::int64_t* seed_probe_spec_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || seed_probe_spec_id_out == nullptr) {
        if (error_out) *error_out = "authoring db unavailable";
        return false;
    }

    return authoring_db->SaveSeedProbeSpec(
        {
            .name = "SavorE2E seedprobe",
            .priority = 1,
            .run_ms = 10000,
            .vi_stall_ms = 2000,
            .min_value = 47,
            .max_value = 207,
            .cap_trigger_top = true,
            .ignore_trigger_minmax = true,
            .combo_attempts_per_target = options.seedprobe_combo_attempts_per_target.value_or(20),
            .combo_sampler_tries = 4,
            .auto_schedule_battle_run = false,
            .created_at_utc = UtcNow(),
            .correlation_id = "savor-e2e.seedprobe",
            .causation_id = "savor-e2e.seed",
        },
        seed_probe_spec_id_out,
        error_out);
}

bool SeedWorkflowGraphExecution(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    std::int64_t savestate_id,
    std::int64_t seed_probe_spec_id,
    const CliOptions& options,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || execution_db == nullptr) {
        if (error_out) *error_out = "authoring/execution db unavailable";
        return false;
    }
    if (savestate_id <= 0 || seed_probe_spec_id <= 0) {
        if (error_out) *error_out = "savestate and seed probe spec ids must be > 0";
        return false;
    }

    savor::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = "SavorE2E workflow graph seedprobe",
                .description = "Graph-style seed probe chain scenario",
                .graph_version = 1,
                .graph_hash = "savor-e2e.workflow_graph.seedprobe",
                .nodes = {
                    {
                        .node_key = "probe_1",
                        .unit_kind = "battle_seed_probe",
                        .display_name = "Battle Seed Probe",
                        .authored_ref_kind = std::string("seed_probe_spec"),
                        .authored_ref_id = seed_probe_spec_id,
                        .inputs = {
                            { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .display_name = "Entry savestate" },
                        },
                        .possible_outputs = {
                            { .output_key = "unique_input_frames", .data_kind = "analysis.input_frame_set_id", .display_name = "Unique input frames" },
                        },
                    },
                },
                .created_at_utc = UtcNow(),
                .correlation_id = "savor-e2e.workflow_graph.seedprobe",
                .causation_id = "savor-e2e.seed",
            },
            &saved,
            error_out)) {
        return false;
    }

    savor::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "savor-e2e";
    command.created_at_utc = UtcNow().time_since_epoch().count();
    const auto registry = savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto probe_activation = savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
        registry,
        "probe_1",
        "probe_1",
        "battle_seed_probe",
        "Battle Seed Probe",
        std::optional<std::string>("seed_probe_spec"),
        seed_probe_spec_id,
        {},
        &activation_error);
    if (!probe_activation.has_value()) {
        if (error_out) *error_out = activation_error;
        return false;
    }
    command.unit_activations.push_back(std::move(*probe_activation));
    command.input_bindings.push_back({
        .node_key = "probe_1",
        .input_key = "entry_savestate",
        .data_kind = "state.savestate_id",
        .ref_kind = "state.savestate",
        .ref_id = savestate_id,
        .source_kind = "external",
    });
    command.arguments.push_back({
        .node_key = "probe_1",
        .argument_key = "samples_per_axis",
        .value_type = "integer",
        .integer_value = options.seedprobe_samples_per_axis.value_or(kSeedProbeSamplesPerAxis),
        .source_kind = "scenario",
    });
    return execution_db->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
}

bool SeedTasMovieWorkflow(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    std::int64_t dtm_artifact_id,
    const CliOptions& options,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || execution_db == nullptr || dtm_artifact_id <= 0) {
        if (error_out) *error_out = "authoring/execution db/dtm artifact unavailable";
        return false;
    }

    savor::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = "SavorE2E workflow graph TasMovie",
                .description = "Graph-style TAS movie scenario",
                .graph_version = 1,
                .graph_hash = "savor-e2e.workflow_graph.tasmovie",
                .nodes = {
                    {
                        .node_key = "tas_1",
                        .unit_kind = "tas_movie",
                        .display_name = "TAS Movie",
                        .inputs = {
                            { .input_key = "dtm_artifact", .data_kind = "state_artifact.dtm_artifact_id", .display_name = "DTM artifact" },
                        },
                        .possible_outputs = {
                            { .output_key = "savestate", .data_kind = "state.savestate_id", .display_name = "Output savestate" },
                        },
                    },
                },
                .created_at_utc = UtcNow(),
                .correlation_id = "savor-e2e.workflow_graph.tasmovie",
                .causation_id = "savor-e2e.seed",
            },
            &saved,
            error_out)) {
        return false;
    }

    savor::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.root_scope_id = dtm_artifact_id;
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "savor-e2e";
    command.created_at_utc = UtcNow().time_since_epoch().count();
    const auto registry = savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto tas_activation = savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
        registry,
        "tas_1",
        "tas_1",
        "tas_movie",
        "TAS Movie",
        std::nullopt,
        std::nullopt,
        {},
        &activation_error);
    if (!tas_activation.has_value()) {
        if (error_out) *error_out = activation_error;
        return false;
    }
    command.unit_activations.push_back(std::move(*tas_activation));
    command.input_bindings.push_back({
        .node_key = "tas_1",
        .input_key = "dtm_artifact",
        .data_kind = "state_artifact.dtm_artifact_id",
        .ref_kind = "state_artifact",
        .ref_id = dtm_artifact_id,
        .source_kind = "external",
    });
    AppendTasMovieRtcArgumentIfSingle(&command, options, 0);
    return execution_db->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
}

bool SeedTasMovieSeedProbeWorkflow(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    std::int64_t dtm_artifact_id,
    std::int64_t seed_probe_spec_id,
    const CliOptions& options,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || execution_db == nullptr) {
        if (error_out) *error_out = "authoring/execution db unavailable";
        return false;
    }
    if (dtm_artifact_id <= 0 || seed_probe_spec_id <= 0) {
        if (error_out) *error_out = "dtm artifact and seed probe spec ids must be > 0";
        return false;
    }

    savor::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = "SavorE2E workflow graph TasMovie SeedProbe",
                .description = "Graph-style TAS movie into seed probe scenario",
                .graph_version = 1,
                .graph_hash = "savor-e2e.workflow_graph.tasmovie_seedprobe",
                .nodes = {
                    {
                        .node_key = "tas_1",
                        .unit_kind = "tas_movie",
                        .display_name = "TAS Movie",
                        .inputs = {
                            { .input_key = "dtm_artifact", .data_kind = "state_artifact.dtm_artifact_id", .display_name = "DTM artifact" },
                        },
                        .possible_outputs = {
                            { .output_key = "savestate", .data_kind = "state.savestate_id", .display_name = "Output savestate" },
                        },
                    },
                    {
                        .node_key = "probe_1",
                        .unit_kind = "battle_seed_probe",
                        .display_name = "Battle Seed Probe",
                        .authored_ref_kind = std::string("seed_probe_spec"),
                        .authored_ref_id = seed_probe_spec_id,
                        .inputs = {
                            { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .display_name = "Entry savestate" },
                        },
                        .possible_outputs = {
                            { .output_key = "unique_input_frames", .data_kind = "analysis.input_frame_set_id", .display_name = "Unique input frames" },
                        },
                    },
                },
                .edges = {
                    { .from_node_key = "tas_1", .output_key = "savestate", .to_node_key = "probe_1", .input_key = "entry_savestate" },
                },
                .created_at_utc = UtcNow(),
                .correlation_id = "savor-e2e.workflow_graph.tasmovie_seedprobe",
                .causation_id = "savor-e2e.seed",
            },
            &saved,
            error_out)) {
        return false;
    }

    savor::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.root_scope_id = dtm_artifact_id;
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "savor-e2e";
    command.created_at_utc = UtcNow().time_since_epoch().count();
    const auto registry = savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto tas_activation = savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
        registry,
        "tas_1",
        "tas_1",
        "tas_movie",
        "TAS Movie",
        std::nullopt,
        std::nullopt,
        {},
        &activation_error);
    if (!tas_activation.has_value()) {
        if (error_out) *error_out = activation_error;
        return false;
    }
    command.unit_activations.push_back(std::move(*tas_activation));
    auto probe_activation = savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
        registry,
        "probe_1",
        "probe_1",
        "battle_seed_probe",
        "Battle Seed Probe",
        std::optional<std::string>("seed_probe_spec"),
        seed_probe_spec_id,
        { "tas_1" },
        &activation_error);
    if (!probe_activation.has_value()) {
        if (error_out) *error_out = activation_error;
        return false;
    }
    command.unit_activations.push_back(std::move(*probe_activation));
    command.input_bindings.push_back({
        .node_key = "tas_1",
        .input_key = "dtm_artifact",
        .data_kind = "state_artifact.dtm_artifact_id",
        .ref_kind = "state_artifact",
        .ref_id = dtm_artifact_id,
        .source_kind = "external",
    });
    AppendTasMovieRtcArgumentIfSingle(&command, options, 0);
    command.arguments.push_back({
        .node_key = "probe_1",
        .argument_key = "samples_per_axis",
        .value_type = "integer",
        .integer_value = options.seedprobe_samples_per_axis.value_or(kSeedProbeSamplesPerAxis),
        .source_kind = "scenario",
    });
    return execution_db->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
}

} // namespace savor::e2e
