#include "DbSetup.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "Common/Types/UtcTimestamp.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Execution/Workflow/WorkflowUnitActivationFactory.h"
#include "Tas/DtmFile.h"

namespace simcore::e2e {

using simcore::db::types::UtcNow;

simcore::db::DbConfigPaths BuildDbPaths(const CliOptions& options) {
    const auto root = options.workspace_root.value_or(std::filesystem::temp_directory_path() / "simcoredbe2e-default");

    std::error_code ec;
    if (std::filesystem::exists(root)) {
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

    return simcore::db::DbConfigPaths{
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

ScopedWorkflowCoordinatorService::~ScopedWorkflowCoordinatorService() {
    Stop();
}

bool ScopedWorkflowCoordinatorService::Start(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IAuthoringDb* authoring_db,
    const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    const CliOptions& options,
    std::string* error_out,
    EventLineCallback event_line_callback) {
    if (service_ != nullptr && service_->IsRunning()) {
        return true;
    }
    if (execution_db == nullptr || authoring_db == nullptr || program_kind_registry == nullptr) {
        if (error_out != nullptr) {
            *error_out = "workflow coordinator requires execution db, authoring db, and program registry";
        }
        return false;
    }

    simcore::db::execution::workflow::WorkflowCoordinatorConfig config{};
    config.workflow_enabled = true;
    config.strict_smoke_terminal_on_failure = false;
    config.poll_interval = std::chrono::milliseconds(std::max<std::int64_t>(1, options.poll_ms));

    auto service = std::make_unique<simcore::db::execution::workflow::WorkflowCoordinatorService>(
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

simcore::db::execution::workflow::WorkflowCoordinatorTelemetry
ScopedWorkflowCoordinatorService::SnapshotTelemetry() const {
    return service_ != nullptr
        ? service_->SnapshotTelemetry()
        : simcore::db::execution::workflow::WorkflowCoordinatorTelemetry{};
}

bool SeedStateSavestate(
    simcore::db::IStateDb* state_db,
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
                .sha256 = "simcoredbe2e-" + savestate_file.filename().string(),
                .size_bytes = static_cast<std::int64_t>(std::filesystem::file_size(savestate_file)),
                .compression_kind = 0,
                .filename = std::filesystem::absolute(savestate_file).string(),
                .file_ext = savestate_file.extension().string(),
                .artifact_kind = "SAV",
                .created_at_utc = UtcNow(),
                .event_id = "simcoredbe2e.state.artifact",
                .correlation_id = "simcoredbe2e.seedprobe",
                .causation_id = "simcoredbe2e.seed",
            },
            &artifact_id,
            error_out)) {
        return false;
    }

    if (!state_db->CreateSavestate(
            {
                .artifact_id = artifact_id,
                .savestate_type = "TRANSITION",
                .note = "SimCoreDBe2e start state",
                .is_complete = true,
                .created_at_utc = UtcNow(),
                .event_id = "simcoredbe2e.state.savestate",
                .correlation_id = "simcoredbe2e.seedprobe",
                .causation_id = "simcoredbe2e.seed",
            },
            savestate_id_out,
            error_out)) {
        return false;
    }

    return true;
}

bool SeedStateDtmArtifact(
    simcore::db::IStateDb* state_db,
    const std::filesystem::path& dtm_file,
    std::int64_t* artifact_id_out,
    std::string* error_out) {
    if (state_db == nullptr || artifact_id_out == nullptr) {
        if (error_out) *error_out = "state db unavailable";
        return false;
    }

    simcore::tas::DtmFile dtm;
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
            .event_id = "simcoredbe2e.state.dtm_artifact",
            .correlation_id = "simcoredbe2e.tasmovie",
            .causation_id = "simcoredbe2e.seed",
        },
        artifact_id_out,
        error_out);
}

bool SeedAuthoringSpec(
    simcore::db::IAuthoringDb* authoring_db,
    const CliOptions& options,
    std::int64_t* seed_probe_spec_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || seed_probe_spec_id_out == nullptr) {
        if (error_out) *error_out = "authoring db unavailable";
        return false;
    }

    return authoring_db->SaveSeedProbeSpec(
        {
            .name = "SimCoreDBe2e seedprobe",
            .priority = 1,
            .run_ms = 10000,
            .vi_stall_ms = 2000,
            .samples_per_axis = options.seedprobe_samples_per_axis.value_or(kSeedProbeSamplesPerAxis),
            .min_value = 47,
            .max_value = 207,
            .cap_trigger_top = true,
            .ignore_trigger_minmax = true,
            .combo_attempts_per_target = options.seedprobe_combo_attempts_per_target.value_or(20),
            .combo_sampler_tries = 4,
            .auto_schedule_battle_run = false,
            .created_at_utc = UtcNow(),
            .event_id = "simcoredbe2e.authoring.seedprobe",
            .correlation_id = "simcoredbe2e.seedprobe",
            .causation_id = "simcoredbe2e.seed",
        },
        seed_probe_spec_id_out,
        error_out);
}

bool SeedWorkflowGraphExecution(
    simcore::db::IAuthoringDb* authoring_db,
    simcore::db::IExecutionDb* execution_db,
    std::int64_t savestate_id,
    std::int64_t seed_probe_spec_id,
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

    simcore::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = "SimCoreDBe2e workflow graph seedprobe",
                .description = "Graph-style seed probe chain scenario",
                .graph_version = 1,
                .graph_hash = "simcoredbe2e.workflow_graph.seedprobe",
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
                .event_id = "simcoredbe2e.authoring.workflow_graph.seedprobe",
                .correlation_id = "simcoredbe2e.workflow_graph.seedprobe",
                .causation_id = "simcoredbe2e.seed",
            },
            &saved,
            error_out)) {
        return false;
    }

    simcore::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "simcoredbe2e";
    command.created_at_utc = UtcNow().time_since_epoch().count();
    const auto registry = simcore::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto probe_activation = simcore::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
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
    return execution_db->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
}

bool SeedTasMovieWorkflow(
    simcore::db::IAuthoringDb* authoring_db,
    simcore::db::IExecutionDb* execution_db,
    std::int64_t dtm_artifact_id,
    const CliOptions& options,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || execution_db == nullptr || dtm_artifact_id <= 0) {
        if (error_out) *error_out = "authoring/execution db/dtm artifact unavailable";
        return false;
    }

    simcore::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = "SimCoreDBe2e workflow graph TasMovie",
                .description = "Graph-style TAS movie scenario",
                .graph_version = 1,
                .graph_hash = "simcoredbe2e.workflow_graph.tasmovie",
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
                .event_id = "simcoredbe2e.authoring.workflow_graph.tasmovie",
                .correlation_id = "simcoredbe2e.workflow_graph.tasmovie",
                .causation_id = "simcoredbe2e.seed",
            },
            &saved,
            error_out)) {
        return false;
    }

    simcore::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.root_scope_id = dtm_artifact_id;
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "simcoredbe2e";
    command.created_at_utc = UtcNow().time_since_epoch().count();
    const auto registry = simcore::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto tas_activation = simcore::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
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
    command.arguments.push_back({
        .node_key = "tas_1",
        .argument_key = "rtc",
        .value_type = "integer",
        .integer_value = options.tasmovie_rtc.value_or(0),
        .source_kind = "scenario",
    });
    return execution_db->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
}

bool SeedTasMovieSeedProbeWorkflow(
    simcore::db::IAuthoringDb* authoring_db,
    simcore::db::IExecutionDb* execution_db,
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

    simcore::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = "SimCoreDBe2e workflow graph TasMovie SeedProbe",
                .description = "Graph-style TAS movie into seed probe scenario",
                .graph_version = 1,
                .graph_hash = "simcoredbe2e.workflow_graph.tasmovie_seedprobe",
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
                .event_id = "simcoredbe2e.authoring.workflow_graph.tasmovie_seedprobe",
                .correlation_id = "simcoredbe2e.workflow_graph.tasmovie_seedprobe",
                .causation_id = "simcoredbe2e.seed",
            },
            &saved,
            error_out)) {
        return false;
    }

    simcore::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.root_scope_id = dtm_artifact_id;
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "simcoredbe2e";
    command.created_at_utc = UtcNow().time_since_epoch().count();
    const auto registry = simcore::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto tas_activation = simcore::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
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
    auto probe_activation = simcore::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
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
    command.arguments.push_back({
        .node_key = "tas_1",
        .argument_key = "rtc",
        .value_type = "integer",
        .integer_value = options.tasmovie_rtc.value_or(0),
        .source_kind = "scenario",
    });
    return execution_db->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
}

} // namespace simcore::e2e
