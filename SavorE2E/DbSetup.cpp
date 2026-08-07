#include "DbSetup.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <limits>
#include <sstream>
#include <utility>

#include "Common/Types/UtcTimestamp.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Execution/Workflow/WorkflowUnitActivationFactory.h"
#include "Tas/DtmFile.h"
#include "Utils/Hash.h"

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

savor::db::DbConfigPaths BuildDbPaths(const CliOptions& options) {
    const auto root = options.workspace_root.value_or(std::filesystem::temp_directory_path() / "savor-e2e-default");
    const bool preserve_prepared_workspace = std::ranges::any_of(
        options.scenarios,
        [](const std::string& scenario) {
            return scenario == "tasmovie"
                || scenario == "tasmovie_with_validation"
                || scenario == "tasmovie_seedprobe";
        });

    std::error_code ec;
    if (!preserve_prepared_workspace && std::filesystem::exists(root)) {
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

bool ResetTasMovieScenarioWorkspace(
    const CliOptions& options,
    std::filesystem::path* workspace_root_out,
    std::string* error_out) {
    std::error_code ec;
    const auto requested_root = options.workspace_root.value_or(
        std::filesystem::temp_directory_path() / "savor-e2e-default");
    const auto root = std::filesystem::absolute(requested_root, ec).lexically_normal();
    if (ec || root.empty() || root == root.root_path()) {
        if (error_out) {
            *error_out = "refusing to reset an empty or filesystem-root TAS Movie workspace";
        }
        return false;
    }

    const std::array database_names{
        "execution.db", "state.db", "analysis.db", "authoring.db",
        "ui_read.db", "archive.db",
    };
    for (const auto* database_name : database_names) {
        for (const auto* suffix : {"", "-wal", "-shm", "-journal"}) {
            const auto target = root / (std::string(database_name) + suffix);
            std::filesystem::remove(target, ec);
            if (ec) {
                if (error_out) {
                    *error_out = "failed clearing TAS Movie database file '"
                        + target.string() + "': " + ec.message();
                }
                return false;
            }
        }
    }

    const std::array reset_directories{
        "object_store", "archive_store", "workflow-runtime",
        "tasmovie-validation", "verification",
    };
    for (const auto* directory_name : reset_directories) {
        const auto target = root / directory_name;
        std::filesystem::remove_all(target, ec);
        if (ec) {
            if (error_out) {
                *error_out = "failed clearing TAS Movie workspace directory '"
                    + target.string() + "': " + ec.message();
            }
            return false;
        }
    }

    std::filesystem::create_directories(root, ec);
    if (ec) {
        if (error_out) {
            *error_out = "failed creating TAS Movie workspace '"
                + root.string() + "': " + ec.message();
        }
        return false;
    }
    std::filesystem::create_directories(root / "log", ec);
    if (ec) {
        if (error_out) {
            *error_out = "failed creating TAS Movie log directory: " + ec.message();
        }
        return false;
    }

    if (workspace_root_out) {
        *workspace_root_out = root;
    }
    return true;
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

bool SeedStateSavestate(
    savor::db::IStateDb* state_db,
    const std::filesystem::path& savestate_file,
    std::int64_t* savestate_id_out,
    std::string* error_out) {
    if (state_db == nullptr || savestate_id_out == nullptr) {
        if (error_out) *error_out = "state db unavailable";
        return false;
    }

    std::string savestate_sha256;
    try {
        savestate_sha256 =
            hash::sha256_of_file(savestate_file.string());
    } catch (const std::exception& exception) {
        if (error_out) {
            *error_out =
                "failed hashing savestate: "
                + std::string(exception.what());
        }
        return false;
    }
    if (savestate_sha256.size() != 64) {
        if (error_out) {
            *error_out =
                "savestate hash is not a complete SHA-256";
        }
        return false;
    }

    std::int64_t artifact_id = 0;
    if (!state_db->StoreArtifact(
            {
                .sha256 = std::move(savestate_sha256),
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
                .description = "Single-step SeedProbe run scenario",
                .graph_version = 1,
                .graph_hash = "savor-e2e.workflow_graph.seedprobe.v2",
                .nodes = {
                    {
                        .node_key = "probe_1",
                        .unit_kind = "battle_seed_probe",
                        .display_name = "Battle Seed Probe",
                        .authored_ref_kind = std::string("seed_probe_spec"),
                        .authored_ref_id = seed_probe_spec_id,
                        .inputs = {
                            { .input_key = "entry_savestate", .data_kind = "state.movie_inactive_savestate_id", .display_name = "Entry savestate" },
                        },
                        .possible_outputs = {
                            { .output_key = "accepted_input_frames", .data_kind = "analysis.input_frame_set_id", .display_name = "Accepted input frames" },
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
    if (probe_activation->steps.size() != 1
        || probe_activation->steps.front().step_kind
            != "seedprobe.run") {
        if (error_out) {
            *error_out =
                "battle_seed_probe must resolve to exactly one "
                "seedprobe.run workflow step";
        }
        return false;
    }
    command.unit_activations.push_back(std::move(*probe_activation));
    command.input_bindings.push_back({
        .node_key = "probe_1",
        .input_key = "entry_savestate",
        .data_kind = "state.movie_inactive_savestate_id",
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
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || execution_db == nullptr || dtm_artifact_id <= 0) {
        if (error_out) *error_out = "authoring/execution db/dtm artifact unavailable";
        return false;
    }

    savor::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = "SavorE2E TAS Movie root cursor establishment",
                .description = "Singleton handcrafted-DTM root cursor establishment",
                .graph_version = 1,
                .graph_hash = "savor-e2e.workflow_graph.tasmovie.establish_root_cursor.v1",
                .nodes = {
                    {
                        .node_key = "tas_1",
                        .unit_kind = "tas_movie_establish_root_cursor",
                        .display_name = "TAS Movie: Establish Root Cursor",
                        .inputs = {
                            { .input_key = "root_dtm", .data_kind = "state_artifact.dtm_artifact_id", .display_name = "Handcrafted root DTM" },
                        },
                        .possible_outputs = {
                            { .output_key = "tas_movie_validation_attempt", .data_kind = "analysis.tas_movie_validation_attempt_id", .display_name = "Validation attempt" },
                            { .output_key = "established_root_cursor_attempt", .data_kind = "analysis.tas_movie_validation_attempt_id", .display_name = "Established root cursor attempt" },
                        },
                    },
                },
                .created_at_utc = UtcNow(),
                .correlation_id = "savor-e2e.workflow_graph.tasmovie.establish_root_cursor",
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
        "tas_movie_establish_root_cursor",
        "TAS Movie: Establish Root Cursor",
        std::nullopt,
        std::nullopt,
        {},
        &activation_error);
    if (!tas_activation.has_value()) {
        if (error_out) *error_out = activation_error;
        return false;
    }
    if (tas_activation->steps.size() != 1
        || tas_activation->steps.front().step_kind != "tasmovie.establish_root_cursor"
        || tas_activation->steps.front().max_attempts != 1) {
        if (error_out) {
            *error_out = "tas_movie_establish_root_cursor must resolve to one single-attempt tasmovie.establish_root_cursor step";
        }
        return false;
    }
    command.unit_activations.push_back(std::move(*tas_activation));
    command.input_bindings.push_back({
        .node_key = "tas_1",
        .input_key = "root_dtm",
        .data_kind = "state_artifact.dtm_artifact_id",
        .ref_kind = "state_artifact",
        .ref_id = dtm_artifact_id,
        .source_kind = "external",
    });
    return execution_db->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
}

bool SeedTasMovieRootValidationWorkflow(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    std::int64_t establishment_attempt_id,
    std::int64_t rtc_value,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || execution_db == nullptr
        || establishment_attempt_id <= 0 || rtc_value < 0
        || static_cast<std::uint64_t>(rtc_value)
            > std::numeric_limits<std::uint32_t>::max()) {
        if (error_out) {
            *error_out = "authoring/execution DB, establishment attempt, or GameCube RTC is invalid";
        }
        return false;
    }

    savor::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = "SavorE2E TAS Movie root validation",
                .description = "Singleton RTC-specific TAS Movie root validation",
                .graph_version = 1,
                .graph_hash = "savor-e2e.workflow_graph.tasmovie.validate_root.v1",
                .nodes = {
                    {
                        .node_key = "tas_validate_1",
                        .unit_kind = "tas_movie_validate_root",
                        .display_name = "TAS Movie: Validate Root",
                        .inputs = {
                            { .input_key = "root_establishment", .data_kind = "analysis.tas_movie_validation_attempt_id", .display_name = "Root cursor establishment" },
                        },
                        .possible_outputs = {
                            { .output_key = "tas_movie_validation_attempt", .data_kind = "analysis.tas_movie_validation_attempt_id", .display_name = "Validation attempt" },
                            { .output_key = "validated_checkpoint_savestate", .data_kind = "state.movie_paired_savestate_id", .display_name = "Validated checkpoint savestate" },
                        },
                    },
                },
                .created_at_utc = UtcNow(),
                .correlation_id = "savor-e2e.workflow_graph.tasmovie.validate_root",
                .causation_id = "savor-e2e.seed",
            },
            &saved,
            error_out)) {
        return false;
    }

    savor::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.root_scope_id = establishment_attempt_id;
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "savor-e2e";
    command.created_at_utc = UtcNow().time_since_epoch().count();
    const auto registry =
        savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto activation =
        savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
            registry,
            "tas_validate_1",
            "tas_validate_1",
            "tas_movie_validate_root",
            "TAS Movie: Validate Root",
            std::nullopt,
            std::nullopt,
            {},
            &activation_error);
    if (!activation.has_value()) {
        if (error_out) *error_out = activation_error;
        return false;
    }
    if (activation->steps.size() != 1
        || activation->steps.front().step_kind != "tasmovie.validate_root"
        || activation->steps.front().max_attempts != 1) {
        if (error_out) {
            *error_out = "tas_movie_validate_root must resolve to one single-attempt tasmovie.validate_root step";
        }
        return false;
    }
    command.unit_activations.push_back(std::move(*activation));
    command.input_bindings.push_back({
        .node_key = "tas_validate_1",
        .input_key = "root_establishment",
        .data_kind = "analysis.tas_movie_validation_attempt_id",
        .ref_kind = "tmv_validation_attempt",
        .ref_id = establishment_attempt_id,
        .source_kind = "external",
    });
    command.arguments.push_back({
        .node_key = "tas_validate_1",
        .argument_key = "rtc",
        .value_type = "integer",
        .integer_value = rtc_value,
        .source_kind = "scenario",
    });
    return execution_db->CreateWorkflowInstance(
        command, workflow_instance_id_out, error_out);
}

bool SeedTasMovieSeedProbeWorkflow(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    std::int64_t dtm_artifact_id,
    std::int64_t seed_probe_spec_id,
    std::int64_t rtc_value,
    const CliOptions& options,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || execution_db == nullptr
        || dtm_artifact_id <= 0 || seed_probe_spec_id <= 0
        || rtc_value < 0
        || static_cast<std::uint64_t>(rtc_value)
            > std::numeric_limits<std::uint32_t>::max()) {
        if (error_out) {
            *error_out = "authoring/execution DB, DTM, SeedProbe spec, or GameCube RTC is invalid";
        }
        return false;
    }

    savor::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = "SavorE2E TAS Movie to SeedProbe",
                .description = "Establish and validate a root DTM, sterilize its checkpoint, then probe the movie-inactive state",
                .graph_version = 1,
                .graph_hash = "savor-e2e.workflow_graph.tasmovie_seedprobe.v2",
                .nodes = {
                    {
                        .node_key = "tas_establish_1",
                        .unit_kind = "tas_movie_establish_root_cursor",
                        .display_name = "TAS Movie: Establish Root Cursor",
                        .inputs = {
                            { .input_key = "root_dtm", .data_kind = "state_artifact.dtm_artifact_id", .display_name = "Handcrafted root DTM" },
                        },
                        .possible_outputs = {
                            { .output_key = "tas_movie_validation_attempt", .data_kind = "analysis.tas_movie_validation_attempt_id", .display_name = "Validation attempt" },
                            { .output_key = "established_root_cursor_attempt", .data_kind = "analysis.tas_movie_validation_attempt_id", .display_name = "Established root cursor attempt" },
                        },
                    },
                    {
                        .node_key = "tas_validate_1",
                        .unit_kind = "tas_movie_validate_root",
                        .display_name = "TAS Movie: Validate Root",
                        .inputs = {
                            { .input_key = "root_establishment", .data_kind = "analysis.tas_movie_validation_attempt_id", .display_name = "Root cursor establishment" },
                        },
                        .possible_outputs = {
                            { .output_key = "tas_movie_validation_attempt", .data_kind = "analysis.tas_movie_validation_attempt_id", .display_name = "Validation attempt" },
                            { .output_key = "validated_checkpoint_savestate", .data_kind = "state.movie_paired_savestate_id", .display_name = "Validated checkpoint savestate" },
                        },
                    },
                    {
                        .node_key = "tas_sterilize_1",
                        .unit_kind = "tas_movie_checkpoint_sterilize",
                        .display_name = "TAS Movie: Sterilize Checkpoint",
                        .inputs = {
                            { .input_key = "paired_checkpoint_savestate", .data_kind = "state.movie_paired_savestate_id", .display_name = "Movie-paired checkpoint" },
                        },
                        .possible_outputs = {
                            { .output_key = "sterilized_checkpoint_savestate", .data_kind = "state.movie_inactive_savestate_id", .display_name = "Movie-inactive checkpoint" },
                        },
                    },
                    {
                        .node_key = "probe_1",
                        .unit_kind = "battle_seed_probe",
                        .display_name = "Battle Seed Probe",
                        .authored_ref_kind = std::string("seed_probe_spec"),
                        .authored_ref_id = seed_probe_spec_id,
                        .inputs = {
                            { .input_key = "entry_savestate", .data_kind = "state.movie_inactive_savestate_id", .display_name = "Sterilized entry savestate" },
                        },
                        .possible_outputs = {
                            { .output_key = "accepted_input_frames", .data_kind = "analysis.input_frame_set_id", .display_name = "Accepted input frames" },
                        },
                    },
                },
                .edges = {
                    {
                        .from_node_key = "tas_establish_1",
                        .output_key = "established_root_cursor_attempt",
                        .to_node_key = "tas_validate_1",
                        .input_key = "root_establishment",
                        .guard_kind = std::string(savor::db::kWorkflowOutputPresentGuard),
                    },
                    {
                        .from_node_key = "tas_validate_1",
                        .output_key = "validated_checkpoint_savestate",
                        .to_node_key = "tas_sterilize_1",
                        .input_key = "paired_checkpoint_savestate",
                        .guard_kind = std::string(savor::db::kWorkflowOutputPresentGuard),
                    },
                    {
                        .from_node_key = "tas_sterilize_1",
                        .output_key = "sterilized_checkpoint_savestate",
                        .to_node_key = "probe_1",
                        .input_key = "entry_savestate",
                        .guard_kind = std::string(savor::db::kWorkflowOutputPresentGuard),
                    },
                },
                .created_at_utc = UtcNow(),
                .correlation_id = "savor-e2e.workflow_graph.tasmovie_seedprobe",
                .causation_id = "savor-e2e.seed",
            },
            &saved,
            error_out)) {
        return false;
    }

    const auto registry =
        savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto establish =
        savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
            registry,
            "tas_establish_1",
            "tas_establish_1",
            "tas_movie_establish_root_cursor",
            "TAS Movie: Establish Root Cursor",
            std::nullopt,
            std::nullopt,
            {},
            &activation_error);
    auto validate =
        savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
            registry,
            "tas_validate_1",
            "tas_validate_1",
            "tas_movie_validate_root",
            "TAS Movie: Validate Root",
            std::nullopt,
            std::nullopt,
            { "tas_establish_1" },
            &activation_error);
    auto probe =
        savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
            registry,
            "probe_1",
            "probe_1",
            "battle_seed_probe",
            "Battle Seed Probe",
            std::optional<std::string>("seed_probe_spec"),
            seed_probe_spec_id,
            { "tas_sterilize_1" },
            &activation_error);
    auto sterilize =
        savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
            registry,
            "tas_sterilize_1",
            "tas_sterilize_1",
            "tas_movie_checkpoint_sterilize",
            "TAS Movie: Sterilize Checkpoint",
            std::nullopt,
            std::nullopt,
            { "tas_validate_1" },
            &activation_error);
    if (!establish || !validate || !sterilize || !probe) {
        if (error_out) *error_out = activation_error;
        return false;
    }
    if (establish->steps.size() != 1
        || establish->steps.front().step_kind
            != "tasmovie.establish_root_cursor"
        || validate->steps.size() != 1
        || validate->steps.front().step_kind != "tasmovie.validate_root"
        || sterilize->steps.size() != 1
        || sterilize->steps.front().step_kind != "tasmovie.checkpoint_sterilize"
        || probe->steps.size() != 1
        || probe->steps.front().step_kind != "seedprobe.run") {
        if (error_out) {
            *error_out = "composed TAS Movie/SeedProbe units did not resolve to their exact singleton steps";
        }
        return false;
    }

    savor::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.root_scope_id = dtm_artifact_id;
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "savor-e2e";
    command.created_at_utc = UtcNow().time_since_epoch().count();
    command.unit_activations.push_back(std::move(*establish));
    command.unit_activations.push_back(std::move(*validate));
    command.unit_activations.push_back(std::move(*sterilize));
    command.unit_activations.push_back(std::move(*probe));
    command.input_bindings.push_back({
        .node_key = "tas_establish_1",
        .input_key = "root_dtm",
        .data_kind = "state_artifact.dtm_artifact_id",
        .ref_kind = "state_artifact",
        .ref_id = dtm_artifact_id,
        .source_kind = "external",
    });
    command.arguments.push_back({
        .node_key = "tas_validate_1",
        .argument_key = "rtc",
        .value_type = "integer",
        .integer_value = rtc_value,
        .source_kind = "scenario",
    });
    command.arguments.push_back({
        .node_key = "probe_1",
        .argument_key = "samples_per_axis",
        .value_type = "integer",
        .integer_value = options.seedprobe_samples_per_axis.value_or(
            kSeedProbeSamplesPerAxis),
        .source_kind = "scenario",
    });
    return execution_db->CreateWorkflowInstance(
        command, workflow_instance_id_out, error_out);
}

} // namespace savor::e2e
