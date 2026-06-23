#include <gtest/gtest.h>

#include "BattleJobBatchRunOptions.h"
#include "BattleJobClone.h"
#include "BattleJobRunOptions.h"
#include "common/SqliteDbFixture.h"

#include "Common/Types/UtcTimestamp.h"
#include "Execution/ProgramDB/BattleSingleTurn/BattleSingleTurnAdapters.h"
#include "Phases/Programs/BattleTurnRunner/BattleTurnRunnerPayload.h"
#include "Runner/IPC/Wire.h"
#include "Runner/Script/CtxRegistry.h"
#include "Runner/Script/PSContext.h"
#include "Utils/IniDoc.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace savor::predict;

std::optional<std::string> QueryText(sqlite3* db, const char* sql, std::int64_t arg) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st, 1, arg);
    std::optional<std::string> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const auto* text = sqlite3_column_text(st, 0);
        out = text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
    }
    sqlite3_finalize(st);
    return out;
}

std::string BuildInputIni(
    std::int64_t wave_id,
    std::int64_t plan_id,
    std::int64_t seed_candidate_id,
    std::int64_t savestate_id,
    int fake_attacks) {
    IniDoc ini;
    ini.set("BattleSingleTurn.Job", "wave_id", std::to_string(wave_id));
    ini.set("BattleSingleTurn.Job", "plan_id", std::to_string(plan_id));
    ini.set("BattleSingleTurn.Job", "seed_candidate_id", std::to_string(seed_candidate_id));
    ini.set("BattleSingleTurn.Job", "savestate_id", std::to_string(savestate_id));
    ini.set("BattleSingleTurn.Job", "turn_index", "1");
    ini.set("BattleSingleTurn.Job", "fake_attacks_used_before", "0");
    ini.set("BattleSingleTurn.Job", "fake_attacks_this_turn", std::to_string(fake_attacks));
    ini.set("BattleSingleTurn.Job", "action_key", "attack-enemy:seeded");
    ini.set("BattleSingleTurn.Job", "resolved_turn_commands_blob", "");
    ini.set("BattleSingleTurn.Job", "resolved_turn_variant_key", "seeded");
    ini.set("BattleSingleTurn.Job", "concrete_turn_plan_hex", "");
    ini.set("BattleSingleTurn.Job", "target_variant_key", "seeded");
    return ini.to_string_sorted();
}

struct SeededBattleJob {
    std::int64_t plan_id = 0;
    std::int64_t battle_set_id = 0;
    std::int64_t wave_id = 0;
    std::int64_t seed_candidate_id = 0;
    std::int64_t turn_job_id = 0;
    std::int64_t exec_job_id = 0;
    std::int64_t unrelated_exec_job_id = 0;
};

class SavorPredictBattleJobRunnerDb : public SqliteDbFixture {
protected:
SeededBattleJob SeedBattleJob(int unique_key = 1) {
    using namespace savor::db;

    auto* authoring_db = db_service_->AuthoringDb();
    auto* analysis_db = db_service_->AnalysisDb();
    auto* execution_db = db_service_->ExecutionDb();
    EXPECT_NE(authoring_db, nullptr);
    EXPECT_NE(analysis_db, nullptr);
    EXPECT_NE(execution_db, nullptr);

    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1781000000200 + unique_key));
    const auto key = std::to_string(unique_key);
    std::string err;
    SeededBattleJob seeded;

    std::int64_t run_spec_id = 0;
    EXPECT_TRUE(authoring_db->SaveBattleRunSpec(
        {
            .name = "predict-runner-run-" + key,
            .priority = 7,
            .run_ms = 1000,
            .vi_stall_ms = 0,
            .use_single_turn_runner = true,
            .created_at_utc = now,
            .correlation_id = "predict-runner",
            .causation_id = "test",
        },
        &run_spec_id,
        &err)) << err;

    EXPECT_TRUE(authoring_db->SavePlan(
        {
            .name = "predict-runner-plan-" + key,
            .fingerprint = "predict-runner-plan-" + key,
            .num_turns = 1,
            .created_at_utc = now,
            .correlation_id = "predict-runner",
            .causation_id = "test",
        },
        &seeded.plan_id,
        &err)) << err;

    std::int64_t action_preset_id = 0;
    EXPECT_TRUE(authoring_db->SaveBattlePlanActionPreset(
        {
            .name = "attack-enemy-" + key,
            .macro = BattlePlanActionMacro::Attack,
            .target_kind = BattlePlanTargetKind::SingleEnemy,
            .target_single_slot = 4,
            .created_at_utc = now,
            .correlation_id = "predict-runner",
            .causation_id = "test",
        },
        &action_preset_id,
        &err)) << err;

    std::int64_t plan_turn_id = 0;
    EXPECT_TRUE(authoring_db->SaveBattlePlanTurn(
        {
            .plan_id = seeded.plan_id,
            .turn_index = 1,
            .actions = {
                {
                    .actor_slot = 0,
                    .action_preset_id = action_preset_id,
                    .ordinal = 0,
                },
            },
            .replace_existing_actions = true,
            .created_at_utc = now,
            .correlation_id = "predict-runner",
            .causation_id = "test",
        },
        &plan_turn_id,
        &err)) << err;

    std::int64_t settings_id = 0;
    EXPECT_TRUE(authoring_db->SaveExplorerSettings(
        {
            .name = "predict-runner-settings-" + key,
            .default_plan_id = seeded.plan_id,
            .created_at_utc = now,
            .correlation_id = "predict-runner",
            .causation_id = "test",
        },
        &settings_id,
        &err)) << err;

    EXPECT_TRUE(analysis_db->CreateBattleSet(
        {
            .name = "predict-runner-set-" + key,
            .entry_savestate_id = 101,
            .battle_run_spec_id = run_spec_id,
            .explorer_settings_id = settings_id,
            .status = BattleSetStatus::Active,
            .created_at_utc = now,
            .correlation_id = "predict-runner",
            .causation_id = "test",
        },
        &seeded.battle_set_id,
        &err)) << err;

    EXPECT_TRUE(analysis_db->AddBattleSeedCandidate(
        {
            .battle_set_id = seeded.battle_set_id,
            .seed_value = 12345,
            .source_kind = BattleSeedCandidateSourceKind::Synthetic,
            .candidate_status = BattleSeedCandidateStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "predict-runner",
            .causation_id = "test",
        },
        &seeded.seed_candidate_id,
        &err)) << err;

    EXPECT_TRUE(analysis_db->CreateBattleTurnWave(
        {
            .battle_set_id = seeded.battle_set_id,
            .turn_index = 1,
            .seed_candidate_id = seeded.seed_candidate_id,
            .status = BattleTurnWaveStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "predict-runner",
            .causation_id = "test",
        },
        &seeded.wave_id,
        &err)) << err;

    EXPECT_TRUE(analysis_db->RecordBattleTurnJob(
        {
            .wave_id = seeded.wave_id,
            .plan_id = seeded.plan_id,
            .source_savestate_id = 101,
            .seed_candidate_id = seeded.seed_candidate_id,
            .authored_plan_id = seeded.plan_id,
            .authored_turn_index = 1,
            .resolved_turn_commands_blob = std::string{},
            .resolved_turn_variant_key = std::string("seeded"),
            .fake_attacks_this_turn = 1,
            .fake_attacks_used_before = 0,
            .job_state = BattleTurnJobState::Queued,
            .started_at_utc = now,
            .recorded_at_utc = now,
            .correlation_id = "predict-runner",
            .causation_id = "test",
        },
        &seeded.turn_job_id,
        &err)) << err;

    std::int64_t job_set_id = 0;
    EXPECT_TRUE(execution_db->CreateJobSet(
        {
            .program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),
            .purpose = "predict-runner-" + key,
            .created_by = std::string("test"),
            .created_at_utc = now.time_since_epoch().count(),
        },
        &job_set_id,
        &err)) << err;

    EXPECT_TRUE(execution_db->EnqueueJob(
        {
            .job_set_id = job_set_id,
            .program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),
            .program_version = 4,
            .program_ref_kind = "analysis_battle.turn_job",
            .program_ref_id = seeded.turn_job_id,
            .savestate_id = 101,
            .fingerprint = "predict-runner-original-" + key,
            .priority = 10,
            .max_attempts = 1,
            .input_ini = BuildInputIni(seeded.wave_id, seeded.plan_id, seeded.seed_candidate_id, 101, 1),
        },
        &seeded.exec_job_id,
        &err)) << err;
    EXPECT_TRUE(analysis_db->SetBattleTurnJobExecJobId(seeded.turn_job_id, seeded.exec_job_id, &err)) << err;

    EXPECT_TRUE(execution_db->EnqueueJob(
        {
            .job_set_id = job_set_id,
            .program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),
            .program_version = 4,
            .program_ref_kind = "analysis_battle.turn_job",
            .program_ref_id = seeded.turn_job_id,
            .savestate_id = 101,
            .fingerprint = "predict-runner-unrelated-" + key,
            .priority = 9,
            .max_attempts = 1,
            .input_ini = BuildInputIni(seeded.wave_id, seeded.plan_id, seeded.seed_candidate_id, 101, 0),
        },
        &seeded.unrelated_exec_job_id,
        &err)) << err;

    return seeded;
}
};

TEST(SavorPredictBattleJobRunOptions, ValidatesSelectorsAndRuntimePaths)
{
    const auto parsed = parse_battle_job_run_tokens(
        {
            "--turn-job-id", "12",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "C:/repo/bin/x64/Debug/SavorPredict.exe");

    EXPECT_TRUE(parsed.errors.empty()) << (parsed.errors.empty() ? "" : parsed.errors.front());
    ASSERT_TRUE(parsed.options.turn_job_id.has_value());
    EXPECT_EQ(*parsed.options.turn_job_id, 12);
    EXPECT_EQ(parsed.options.worker_exe_path.generic_string(), "C:/repo/bin/x64/Debug/SavorWorker.exe");
    EXPECT_EQ(parsed.options.sandbox_mode, savor::dbutils::SandboxMode::MinimalBattleSingleTurn);
    EXPECT_NE(parsed.options.run_root.generic_string().find("Analyses/battle_runs_first_battle/live_capture_runs/"), std::string::npos);

    const auto full_copy = parse_battle_job_run_tokens(
        {
            "--exec-job-id", "34",
            "--sandbox-mode", "full-copy",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_TRUE(full_copy.errors.empty()) << (full_copy.errors.empty() ? "" : full_copy.errors.front());
    EXPECT_EQ(full_copy.options.sandbox_mode, savor::dbutils::SandboxMode::FullCopy);

    const auto both = parse_battle_job_run_tokens(
        {
            "--turn-job-id", "12",
            "--exec-job-id", "34",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_FALSE(both.errors.empty());

    const auto mutable_db = parse_battle_job_run_tokens(
        {
            "--exec-job-id", "34",
            "--db-root", "D:/SoaSimDBDebug",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_FALSE(mutable_db.errors.empty());
}

TEST(SavorPredictBattleJobBatchRunOptions, ParsesRepeatedIdsListFileAndTimeoutDefaults)
{
    const auto list_path = std::filesystem::temp_directory_path()
        / ("savor_batch_ids_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    {
        std::ofstream file(list_path, std::ios::binary | std::ios::trunc);
        file << "# comment\n102\n\n";
    }

    const auto parsed = parse_battle_job_batch_run_tokens(
        {
            "--exec-job-id", "101",
            "--exec-job-list", list_path.string(),
            "--max-workers", "2",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "C:/repo/bin/x64/Debug/SavorPredict.exe");
    std::error_code ec;
    std::filesystem::remove(list_path, ec);

    EXPECT_TRUE(parsed.errors.empty()) << (parsed.errors.empty() ? "" : parsed.errors.front());
    ASSERT_EQ(parsed.options.exec_job_ids.size(), 2u);
    EXPECT_EQ(parsed.options.exec_job_ids[0], 101);
    EXPECT_EQ(parsed.options.exec_job_ids[1], 102);
    EXPECT_EQ(parsed.options.max_workers, 2);
    EXPECT_EQ(parsed.options.worker_exe_path.generic_string(), "C:/repo/bin/x64/Debug/SavorWorker.exe");
    EXPECT_EQ(parsed.options.sandbox_mode, savor::dbutils::SandboxMode::MinimalBattleSingleTurn);
    EXPECT_NE(parsed.options.run_root.generic_string().find("Analyses/battle_runs_first_battle/live_capture_runs/batch_"), std::string::npos);
    EXPECT_EQ(resolved_battle_job_batch_timeout_ms(parsed.options), 180000);

    auto timeout_options = parsed.options;
    timeout_options.max_workers = 2;
    timeout_options.exec_job_ids = { 1, 2, 3, 4, 5 };
    EXPECT_EQ(resolved_battle_job_batch_timeout_ms(timeout_options), 540000);

    const auto duplicate = parse_battle_job_batch_run_tokens(
        {
            "--exec-job-id", "101",
            "--exec-job-id", "101",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_FALSE(duplicate.errors.empty());

    const auto full_copy = parse_battle_job_batch_run_tokens(
        {
            "--exec-job-id", "101",
            "--sandbox-mode", "full-copy",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_TRUE(full_copy.errors.empty()) << (full_copy.errors.empty() ? "" : full_copy.errors.front());
    EXPECT_EQ(full_copy.options.sandbox_mode, savor::dbutils::SandboxMode::FullCopy);

    const auto invalid_workers = parse_battle_job_batch_run_tokens(
        {
            "--exec-job-id", "101",
            "--max-workers", "0",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_FALSE(invalid_workers.errors.empty());

    const auto mutable_db = parse_battle_job_batch_run_tokens(
        {
            "--exec-job-id", "101",
            "--db-root", "D:/SoaSimDBDebug",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_FALSE(mutable_db.errors.empty());
}

TEST(SavorPredictBattleJobClone, PatchesCaptureProfileWithoutDroppingJobFields)
{
    const auto patched = patch_battle_single_turn_capture_profile(
        BuildInputIni(10, 20, 30, 40, 2),
        "C:/runs/capture_profile.ini");
    const auto ini = IniDoc::parse(patched);
    EXPECT_EQ(ini.get_i64("BattleSingleTurn.Job", "wave_id", 0), 10);
    EXPECT_EQ(ini.get_i64("BattleSingleTurn.Job", "plan_id", 0), 20);
    EXPECT_EQ(ini.get_i64("BattleSingleTurn.Job", "seed_candidate_id", 0), 30);
    EXPECT_EQ(ini.get_i64("BattleSingleTurn.Job", "savestate_id", 0), 40);
    EXPECT_EQ(ini.get_i64("BattleSingleTurn.Job", "fake_attacks_this_turn", 0), 2);
    EXPECT_EQ(ini.get("BattleSingleTurn.Job", "target_variant_key", ""), "seeded");
    EXPECT_EQ(ini.get("BattleSingleTurn.Job", "capture_profile_path", ""), "C:/runs/capture_profile.ini");
}

TEST_F(SavorPredictBattleJobRunnerDb, ClonesTurnAndExecRowsThenQuarantinesOtherReadyJobs)
{
    const auto seeded = SeedBattleJob();
    BattleJobRunOptions options;
    options.exec_job_id = seeded.exec_job_id;
    options.iso_path = "D:/SoATAS/game.gcm";
    options.dolphin_base_dir = "D:/SoATAS/dolphin";
    options.worker_exe_path = "C:/repo/SavorWorker.exe";
    options.run_root = temp_root_ / "run";

    BattleJobCloneResult clone;
    std::ostringstream err;
    ASSERT_TRUE(clone_battle_job_for_capture(
        *db_service_,
        options,
        temp_root_ / "capture_profile.ini",
        &clone,
        err)) << err.str();

    EXPECT_EQ(clone.original_turn_job_id, seeded.turn_job_id);
    EXPECT_EQ(clone.original_exec_job_id, seeded.exec_job_id);
    EXPECT_NE(clone.cloned_turn_job_id, seeded.turn_job_id);
    EXPECT_NE(clone.cloned_exec_job_id, seeded.exec_job_id);
    EXPECT_EQ(clone.fake_attacks_this_turn, 1);
    EXPECT_GE(clone.quarantined_ready_jobs, 1);

    const auto source_turn = db_service_->AnalysisDb()->GetBattleTurnJobForExecJob(seeded.exec_job_id);
    ASSERT_TRUE(source_turn.has_value());
    EXPECT_EQ(source_turn->turn_job_id, seeded.turn_job_id);

    const auto cloned_turn = db_service_->AnalysisDb()->GetBattleTurnJobForExecJob(clone.cloned_exec_job_id);
    ASSERT_TRUE(cloned_turn.has_value());
    EXPECT_EQ(cloned_turn->turn_job_id, clone.cloned_turn_job_id);

    const auto cloned_exec = db_service_->ExecutionDb()->GetJob(clone.cloned_exec_job_id);
    ASSERT_TRUE(cloned_exec.has_value());
    EXPECT_EQ(cloned_exec->program_ref_kind, "analysis_battle.turn_job");
    EXPECT_EQ(cloned_exec->program_ref_id, clone.cloned_turn_job_id);
    EXPECT_EQ(cloned_exec->state, "QUEUED");
    EXPECT_NE(cloned_exec->input_ini.find("capture_profile_path"), std::string::npos);

    const auto unrelated_state = QueryText(db_, "SELECT state FROM exec_job WHERE job_id=?1;", seeded.unrelated_exec_job_id);
    ASSERT_TRUE(unrelated_state.has_value());
    EXPECT_EQ(*unrelated_state, "SUPERSEDED");
}

TEST_F(SavorPredictBattleJobRunnerDb, BatchCloneKeepsAllClonesAndQuarantinesOtherReadyJobs)
{
    const auto first = SeedBattleJob(101);
    const auto second = SeedBattleJob(102);

    BattleJobBatchCloneResult batch;
    std::ostringstream err;
    ASSERT_TRUE(clone_battle_jobs_for_capture(
        *db_service_,
        { first.exec_job_id, second.exec_job_id },
        temp_root_ / "capture_profile.ini",
        &batch,
        err)) << err.str();

    ASSERT_EQ(batch.clones.size(), 2u);
    EXPECT_GE(batch.quarantined_ready_jobs, 2);
    EXPECT_EQ(batch.clones[0].original_exec_job_id, first.exec_job_id);
    EXPECT_EQ(batch.clones[1].original_exec_job_id, second.exec_job_id);
    EXPECT_NE(batch.clones[0].cloned_exec_job_id, batch.clones[1].cloned_exec_job_id);
    EXPECT_NE(batch.clones[0].cloned_turn_job_id, batch.clones[1].cloned_turn_job_id);

    for (const auto& clone : batch.clones) {
        const auto cloned_turn = db_service_->AnalysisDb()->GetBattleTurnJobForExecJob(clone.cloned_exec_job_id);
        ASSERT_TRUE(cloned_turn.has_value());
        EXPECT_EQ(cloned_turn->turn_job_id, clone.cloned_turn_job_id);

        const auto cloned_exec = db_service_->ExecutionDb()->GetJob(clone.cloned_exec_job_id);
        ASSERT_TRUE(cloned_exec.has_value());
        EXPECT_EQ(cloned_exec->state, "QUEUED");
        EXPECT_NE(cloned_exec->input_ini.find("capture_profile_path"), std::string::npos);
    }

    const auto first_unrelated_state = QueryText(db_, "SELECT state FROM exec_job WHERE job_id=?1;", first.unrelated_exec_job_id);
    const auto second_unrelated_state = QueryText(db_, "SELECT state FROM exec_job WHERE job_id=?1;", second.unrelated_exec_job_id);
    ASSERT_TRUE(first_unrelated_state.has_value());
    ASSERT_TRUE(second_unrelated_state.has_value());
    EXPECT_EQ(*first_unrelated_state, "SUPERSEDED");
    EXPECT_EQ(*second_unrelated_state, "SUPERSEDED");
}

TEST_F(SavorPredictBattleJobRunnerDb, ClonedJobMaterializesCapturePayload)
{
    using namespace savor::db::execution::programdb::battle;

    const auto seeded = SeedBattleJob();
    BattleJobRunOptions options;
    options.exec_job_id = seeded.exec_job_id;
    options.iso_path = "D:/SoATAS/game.gcm";
    options.dolphin_base_dir = "D:/SoATAS/dolphin";
    options.worker_exe_path = "C:/repo/SavorWorker.exe";
    options.run_root = temp_root_ / "run";

    const auto capture_profile = temp_root_ / "capture_profile.ini";
    BattleJobCloneResult clone;
    std::ostringstream err;
    ASSERT_TRUE(clone_battle_job_for_capture(*db_service_, options, capture_profile, &clone, err)) << err.str();

    const auto working_root = temp_root_ / "workflow-runtime" / "battle-single-turn";
    const auto descriptor = BuildBattleSingleTurnDescriptor(
        db_service_->ExecutionDb(),
        db_service_->StateDb(),
        db_service_->AnalysisDb(),
        BattleSingleTurnPhaseRegistrationConfig{
            .authoring_db = db_service_->AuthoringDb(),
            .working_dir_root = working_root,
        });
    ASSERT_NE(descriptor.runtime_init, nullptr);

    const auto init = descriptor.runtime_init->BuildRuntimeInit(clone.cloned_exec_job_id);
    const auto ps_job = descriptor.runtime_init->MaterializePsJob(clone.cloned_exec_job_id, init);
    ASSERT_TRUE(ps_job.has_value());

    savor::PSContext ctx;
    ASSERT_TRUE(phase::battle::turnrunner::decode_payload(ps_job->payload, ctx));
    std::string profile_path;
    std::string output_path;
    ASSERT_TRUE(ctx.get(savor::context::key::core::CAPTURE_PROFILE_PATH, profile_path));
    ASSERT_TRUE(ctx.get(savor::context::key::core::CAPTURE_OUTPUT_PATH, output_path));
    EXPECT_EQ(profile_path, capture_profile.string());
    EXPECT_NE(output_path.find("battle_checkpoint_capture.jsonl"), std::string::npos);
    EXPECT_NE(output_path.find("job-" + std::to_string(clone.cloned_exec_job_id)), std::string::npos);
}

} // namespace
