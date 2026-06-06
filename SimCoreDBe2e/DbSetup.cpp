#include "DbSetup.h"

#include "Common/Types/UtcTimestamp.h"
#include "Execution/Workflow/SeedProbeWorkflowDefinition.h"
#include "Execution/Workflow/WorkflowInstanceBuilder.h"
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
            .samples_per_axis = kSeedProbeSamplesPerAxis,
            .min_value = 47,
            .max_value = 207,
            .cap_trigger_top = true,
            .ignore_trigger_minmax = true,
            .combo_attempts_per_target = 40,
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

bool SeedExecutionWorkflow(
    simcore::db::IAnalysisDb* analysis_db,
    simcore::db::IExecutionDb* execution_db,
    std::int64_t savestate_id,
    std::int64_t seed_probe_spec_id,
    std::int64_t* workflow_instance_id_out,
    std::int64_t* probe_run_id_out,
    std::string* error_out) {
    if (analysis_db == nullptr || execution_db == nullptr) {
        if (error_out) *error_out = "analysis/execution db unavailable";
        return false;
    }

    std::int64_t probe_set_id = 0;
    if (!analysis_db->CreateSeedProbeSet(
            {
                .name = "SimCoreDBe2e probe set",
                .probe_flavor = "BATTLE_PRE",
                .breakpoint_policy_name = "default",
                .segment_source_kind = "manual",
                .created_at_utc = UtcNow(),
                .event_id = "simcoredbe2e.analysis.probe_set",
                .correlation_id = "simcoredbe2e.seedprobe",
                .causation_id = "simcoredbe2e.seed",
            },
            &probe_set_id,
            error_out)) {
        return false;
    }

    std::int64_t probe_run_id = 0;
    if (!analysis_db->RequestSeedProbeRun(
            {
                .probe_set_id = probe_set_id,
                .entry_savestate_id = savestate_id,
                .seed_probe_spec_id = seed_probe_spec_id,
                .codec_version = 1,
                .status = "queued",
                .requested_at_utc = UtcNow(),
                .event_id = "simcoredbe2e.analysis.probe_run",
                .correlation_id = "simcoredbe2e.seedprobe",
                .causation_id = "simcoredbe2e.seed",
            },
            &probe_run_id,
            error_out)) {
        return false;
    }
    if (probe_run_id_out != nullptr) {
        *probe_run_id_out = probe_run_id;
    }

    simcore::db::execution::workflow::WorkflowDefinitionRegistry registry;
    if (!registry.RegisterSeedProbeDefaults(error_out)) {
        return false;
    }
    simcore::db::execution::workflow::WorkflowInstanceValidator validator;
    simcore::db::execution::workflow::WorkflowInstanceBuilder builder(&registry, &validator);
    simcore::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    if (!builder.BuildCreateCommand(
            {
                .workflow_kind = "SEED_PROBE_CHAIN",
                .root_scope_kind = "run",
                .root_scope_id = probe_run_id,
                .input_ref_kind = std::string("sp_probe_run"),
                .input_ref_id = probe_run_id,
                .created_by = "simcoredbe2e",
                .created_at_utc = UtcNow().time_since_epoch().count(),
                .available_inputs = { "sp_probe_run.probe_run_id" },
            },
            &command,
            error_out)) {
        return false;
    }

    return execution_db->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
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
                        .unit_kind = "seed_probe_chain",
                        .display_name = "Seed Probe Chain",
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

    return execution_db->CreateWorkflowInstance(
        {
            .workflow_kind = "workflow_graph",
            .root_scope_kind = "manual",
            .workflow_graph_revision_id = saved.workflow_graph_revision_id,
            .created_by = "simcoredbe2e",
            .created_at_utc = UtcNow().time_since_epoch().count(),
            .steps = {
                {
                    .step_key = "probe_1",
                    .step_kind = "seed_probe_chain",
                    .priority = 1,
                    .max_attempts = 1,
                },
            },
            .input_bindings = {
                {
                    .node_key = "probe_1",
                    .input_key = "entry_savestate",
                    .data_kind = "state.savestate_id",
                    .ref_kind = "state.savestate",
                    .ref_id = savestate_id,
                    .source_kind = "external",
                },
            },
        },
        workflow_instance_id_out,
        error_out);
}

bool SeedTasMovieWorkflow(
    simcore::db::IExecutionDb* execution_db,
    std::int64_t dtm_artifact_id,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (execution_db == nullptr || dtm_artifact_id <= 0) {
        if (error_out) *error_out = "execution db/dtm artifact unavailable";
        return false;
    }

    simcore::db::execution::workflow::WorkflowDefinitionRegistry registry;
    if (!registry.RegisterTasMovieDefaults(error_out)) {
        return false;
    }
    simcore::db::execution::workflow::WorkflowInstanceValidator validator;
    simcore::db::execution::workflow::WorkflowInstanceBuilder builder(&registry, &validator);
    simcore::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    if (!builder.BuildCreateCommand(
            {
                .workflow_kind = "TAS_MOVIE_CHAIN",
                .root_scope_kind = "manual",
                .root_scope_id = dtm_artifact_id,
                .input_ref_kind = std::string("state_artifact"),
                .input_ref_id = dtm_artifact_id,
                .created_by = "simcoredbe2e",
                .created_at_utc = UtcNow().time_since_epoch().count(),
                .available_inputs = { "state_artifact.artifact_id" },
            },
            &command,
            error_out)) {
        return false;
    }
    return execution_db->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
}

bool SeedTasMovieSeedProbeWorkflow(
    simcore::db::IAnalysisDb* analysis_db,
    simcore::db::IExecutionDb* execution_db,
    std::int64_t placeholder_savestate_id,
    std::int64_t seed_probe_spec_id,
    std::int64_t* workflow_instance_id_out,
    std::int64_t* probe_run_id_out,
    std::string* error_out) {
    if (analysis_db == nullptr || execution_db == nullptr) {
        if (error_out) *error_out = "analysis/execution db unavailable";
        return false;
    }

    std::int64_t probe_set_id = 0;
    if (!analysis_db->CreateSeedProbeSet(
            {
                .name = "SimCoreDBe2e TasMovie probe set",
                .probe_flavor = "BATTLE_PRE",
                .breakpoint_policy_name = "default",
                .segment_source_kind = "tasmovie",
                .created_at_utc = UtcNow(),
                .event_id = "simcoredbe2e.tasmovie.analysis.probe_set",
                .correlation_id = "simcoredbe2e.tasmovie_seedprobe",
                .causation_id = "simcoredbe2e.seed",
            },
            &probe_set_id,
            error_out)) {
        return false;
    }

    std::int64_t probe_run_id = 0;
    if (!analysis_db->RequestSeedProbeRun(
            {
                .probe_set_id = probe_set_id,
                .entry_savestate_id = placeholder_savestate_id,
                .seed_probe_spec_id = seed_probe_spec_id,
                .codec_version = 1,
                .status = "queued",
                .requested_at_utc = UtcNow(),
                .event_id = "simcoredbe2e.tasmovie.analysis.probe_run",
                .correlation_id = "simcoredbe2e.tasmovie_seedprobe",
                .causation_id = "simcoredbe2e.seed",
            },
            &probe_run_id,
            error_out)) {
        return false;
    }
    if (probe_run_id_out != nullptr) {
        *probe_run_id_out = probe_run_id;
    }

    simcore::db::execution::workflow::WorkflowDefinitionRegistry registry;
    if (!registry.RegisterSeedProbeDefaults(error_out) || !registry.RegisterTasMovieDefaults(error_out)) {
        return false;
    }
    simcore::db::execution::workflow::WorkflowInstanceValidator validator;
    simcore::db::execution::workflow::WorkflowInstanceBuilder builder(&registry, &validator);
    simcore::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    if (!builder.BuildCreateCommand(
            {
                .workflow_kind = "TAS_MOVIE_SEED_PROBE_CHAIN",
                .root_scope_kind = "run",
                .root_scope_id = probe_run_id,
                .input_ref_kind = std::string("sp_probe_run"),
                .input_ref_id = probe_run_id,
                .created_by = "simcoredbe2e",
                .created_at_utc = UtcNow().time_since_epoch().count(),
                .available_inputs = { "sp_probe_run.probe_run_id" },
            },
            &command,
            error_out)) {
        return false;
    }
    return execution_db->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
}

} // namespace simcore::e2e
