#include "DbSetup.h"

#include <chrono>

#include "Common/Types/UtcTimestamp.h"

namespace simcore::e2e {

using simcore::db::types::UtcNow;

simcore::db::DbConfigPaths BuildDbPaths(const CliOptions& options) {
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto root = options.workspace_root.value_or(std::filesystem::temp_directory_path() / ("simcoredbe2e-" + std::to_string(stamp)));

    std::error_code ec;
    std::filesystem::create_directories(root, ec);
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
                .filename = savestate_file.filename().string(),
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
            .samples_per_axis = 5,
            .min_value = 47,
            .max_value = 207,
            .cap_trigger_top = true,
            .ignore_trigger_minmax = true,
            .combo_attempts_per_target = 10,
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
    simcore::db::execution::workflow::SqliteExecutionDb* execution_db,
    std::int64_t savestate_id,
    std::int64_t seed_probe_spec_id,
    std::string* error_out) {
    if (execution_db == nullptr) {
        if (error_out) *error_out = "execution db unavailable";
        return false;
    }

    const std::string sql =
        "INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, root_scope_id, input_ref_kind, input_ref_id, created_by, created_at_utc, started_at_utc) "
        "VALUES(9101, 'SEED_PROBE_CHAIN', 'RUNNING', 'authoring.seed_probe_spec', " + std::to_string(seed_probe_spec_id) + ", 'general.transition_savestate', " + std::to_string(savestate_id) + ", 'simcoredbe2e', unixepoch()*1000, unixepoch()*1000);"
        "INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, priority, attempts, max_attempts, input_ref_kind, created_at_utc, ready_at_utc) "
        "VALUES(9102, 9101, 'Neutral', 'seedprobe.neutral', 'READY', 1, 0, 2, 'general.transition_savestate', unixepoch()*1000, unixepoch()*1000);";

    return execution_db->ValidationExecuteSql(sql, error_out);
}

} // namespace simcore::e2e
