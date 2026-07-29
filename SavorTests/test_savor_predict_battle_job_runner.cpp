#include <gtest/gtest.h>

#include "BattleJobBatchRunOptions.h"
#include "BattleJobBatchRunManifest.h"
#include "BattleJobClone.h"
#include "BattleJobRunManifest.h"
#include "BattleJobRunOptions.h"
#include "ProbeCpuCoreEnvironment.h"
#include "common/SqliteDbFixture.h"

#include "Common/Types/UtcTimestamp.h"
#include "Execution/ProgramDB/BattleSingleTurn/BattleSingleTurnAdapters.h"
#include "Phases/Programs/BattleTurnRunner/BattleTurnRunnerPayload.h"
#include "Runner/IPC/Wire.h"
#include "Runner/Script/CtxRegistry.h"
#include "Runner/Script/PSContext.h"
#include "Utils/IniDoc.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace savor::predict;

BattlePredictorResourceBundlePtr MakeManifestResourceInputs(
    BattlePredictorResourceProviderKind provider) {
    auto bundle = std::make_shared<BattlePredictorResourceBundle>();
    bundle->status = BattlePredictorResourceInputStatus::Ready;
    bundle->provider_kind = provider;
    bundle->adapter_version = "manifest-test-adapter";
    bundle->spice_revision = "manifest-test-spice";
    bundle->bundle_digest = "manifest-test-digest";
    bundle->sources.push_back({
        .logical_role = "ma000.primary_std",
        .relative_path = "bchara/MA000.std",
        .normalized_relative_path = "bchara/ma000.std",
        .source_path = "D:/fixture/bchara/MA000.std",
        .size_bytes = 4,
        .sha256 = "manifest-test-sha256",
        .parser_identity = "manifest-test-parser",
        .parser_status = "ready",
    });
    return bundle;
}

std::string ReadManifestText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

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
            "--override-start-rng-seed", "0x12345678",
            "--override-fake-attacks", "2",
        },
        "C:/repo/bin/x64/Debug/SavorPredict.exe");

    EXPECT_TRUE(parsed.errors.empty()) << (parsed.errors.empty() ? "" : parsed.errors.front());
    ASSERT_TRUE(parsed.options.turn_job_id.has_value());
    EXPECT_EQ(*parsed.options.turn_job_id, 12);
    EXPECT_EQ(parsed.options.worker_exe_path.generic_string(), "C:/repo/bin/x64/Debug/SavorWorker.exe");
    EXPECT_EQ(parsed.options.sandbox_mode, savor::dbutils::SandboxMode::MinimalBattleSingleTurn);
    ASSERT_TRUE(parsed.options.override_start_rng_seed.has_value());
    EXPECT_EQ(*parsed.options.override_start_rng_seed, 0x12345678u);
    ASSERT_TRUE(parsed.options.override_fake_attacks_this_turn.has_value());
    EXPECT_EQ(*parsed.options.override_fake_attacks_this_turn, 2u);
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

    const auto std_options = parse_battle_job_run_tokens(
        {
            "--exec-job-id", "34",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
            "--disc-dump-root", "D:/disc",
            "--std-disc-dump-root", "D:/disc/.",
            "--spice-file-parsing-exe", "D:/tools/SpiceFileParsing.exe",
        },
        "SavorPredict.exe");
    EXPECT_TRUE(std_options.errors.empty())
        << (std_options.errors.empty() ? "" : std_options.errors.front());
    EXPECT_EQ(std_options.options.disc_dump_root, std::filesystem::path("D:/disc"));
    EXPECT_NE(
        std::find_if(
            std_options.warnings.begin(),
            std_options.warnings.end(),
            [](const std::string& warning) {
                return warning.find("--spice-file-parsing-exe")
                    != std::string::npos
                    && warning.find("ignored") != std::string::npos;
            }),
        std_options.warnings.end());
    EXPECT_EQ(
        std_options.options.spice_file_parsing_exe,
        std::filesystem::path("D:/tools/SpiceFileParsing.exe"));
    const auto conflicting_resource_roots =
        parse_battle_job_run_tokens(
            {
                "--exec-job-id", "34",
                "--iso", "D:/SoATAS/game.gcm",
                "--dolphin-base-dir", "D:/SoATAS/dolphin",
                "--disc-dump-root", "D:/disc",
                "--std-disc-dump-root", "D:/other",
            },
            "SavorPredict.exe");
    EXPECT_NE(
        std::find_if(
            conflicting_resource_roots.errors.begin(),
            conflicting_resource_roots.errors.end(),
            [](const std::string& error) {
                return error.find("must resolve to the same path")
                    != std::string::npos;
            }),
        conflicting_resource_roots.errors.end());

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

    const auto removed_battle_run_option = parse_battle_job_run_tokens(
        {
            "--exec-job-id", "34",
            "--battle-run-ms", "900000",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_FALSE(removed_battle_run_option.errors.empty());

    const auto progress_only = parse_battle_job_run_tokens(
        {
            "--exec-job-id", "34", "--probe-mode", "progress-only",
            "--iso", "D:/SoATAS/game.gcm", "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_TRUE(progress_only.errors.empty())
        << (progress_only.errors.empty() ? "" : progress_only.errors.front());
    EXPECT_EQ(progress_only.options.probe_mode, ProbeMode::ProgressOnly);

    const auto control_only = parse_battle_job_run_tokens(
        {
            "--exec-job-id", "34", "--probe-mode", "control-only",
            "--iso", "D:/SoATAS/game.gcm", "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_TRUE(control_only.errors.empty())
        << (control_only.errors.empty() ? "" : control_only.errors.front());
    EXPECT_EQ(control_only.options.probe_mode, ProbeMode::ControlOnly);

    const auto interpreter = parse_battle_job_run_tokens(
        {
            "--exec-job-id", "34", "--probe-cpu-core", "interpreter",
            "--iso", "D:/SoATAS/game.gcm", "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_TRUE(interpreter.errors.empty())
        << (interpreter.errors.empty() ? "" : interpreter.errors.front());
    EXPECT_EQ(interpreter.options.probe_cpu_core, ProbeCpuCore::Interpreter);

    const auto invalid_cpu_core = parse_battle_job_run_tokens(
        {
            "--exec-job-id", "34", "--probe-cpu-core", "fast",
            "--iso", "D:/SoATAS/game.gcm", "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_FALSE(invalid_cpu_core.errors.empty());

    const auto incompatible_profile = parse_battle_job_run_tokens(
        {
            "--exec-job-id", "34", "--probe-mode", "progress-only",
            "--capture-profile", "capture.json",
            "--iso", "D:/SoATAS/game.gcm", "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_FALSE(incompatible_profile.errors.empty());
}

TEST(
    SavorPredictBattleJobBatchRunOptions,
    ParsesRepeatedIdsListFileWithoutExecutionDeadline)
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
            "--wait-for-workers-ready",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
            "--override-start-rng-seed", "305419896",
            "--override-fake-attacks", "2",
        },
        "C:/repo/bin/x64/Debug/SavorPredict.exe");
    std::error_code ec;
    std::filesystem::remove(list_path, ec);

    EXPECT_TRUE(parsed.errors.empty()) << (parsed.errors.empty() ? "" : parsed.errors.front());
    ASSERT_EQ(parsed.options.exec_job_ids.size(), 2u);
    EXPECT_EQ(parsed.options.exec_job_ids[0], 101);
    EXPECT_EQ(parsed.options.exec_job_ids[1], 102);
    const auto parsed_requests = resolved_battle_job_batch_requests(parsed.options);
    ASSERT_EQ(parsed_requests.size(), 2u);
    EXPECT_EQ(parsed_requests[0].exec_job_id, 101);
    EXPECT_EQ(parsed_requests[1].exec_job_id, 102);
    ASSERT_TRUE(parsed_requests[0].override_start_rng_seed.has_value());
    ASSERT_TRUE(parsed_requests[1].override_start_rng_seed.has_value());
    ASSERT_TRUE(parsed_requests[0].override_fake_attacks_this_turn.has_value());
    ASSERT_TRUE(parsed_requests[1].override_fake_attacks_this_turn.has_value());
    EXPECT_EQ(*parsed_requests[0].override_start_rng_seed, 0x12345678u);
    EXPECT_EQ(*parsed_requests[1].override_start_rng_seed, 0x12345678u);
    EXPECT_EQ(*parsed_requests[0].override_fake_attacks_this_turn, 2u);
    EXPECT_EQ(*parsed_requests[1].override_fake_attacks_this_turn, 2u);
    EXPECT_EQ(parsed.options.max_workers, 2);
    EXPECT_TRUE(parsed.options.wait_for_workers_ready);
    ASSERT_TRUE(parsed.options.override_start_rng_seed.has_value());
    EXPECT_EQ(*parsed.options.override_start_rng_seed, 0x12345678u);
    EXPECT_EQ(parsed.options.worker_exe_path.generic_string(), "C:/repo/bin/x64/Debug/SavorWorker.exe");
    EXPECT_EQ(parsed.options.sandbox_mode, savor::dbutils::SandboxMode::MinimalBattleSingleTurn);
    EXPECT_NE(parsed.options.run_root.generic_string().find("Analyses/battle_runs_first_battle/live_capture_runs/batch_"), std::string::npos);

    const auto seeded_runs = parse_battle_job_batch_run_tokens(
        {
            "--exec-job-seed", "101:0x11111111",
            "--exec-job-seed", "101:572662306:3",
            "--exec-job-fake-attacks", "101:4",
            "--max-workers", "2",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_TRUE(seeded_runs.errors.empty()) << (seeded_runs.errors.empty() ? "" : seeded_runs.errors.front());
    EXPECT_FALSE(seeded_runs.options.wait_for_workers_ready);
    const auto seeded_requests = resolved_battle_job_batch_requests(seeded_runs.options);
    ASSERT_EQ(seeded_requests.size(), 3u);
    EXPECT_EQ(seeded_requests[0].exec_job_id, 101);
    EXPECT_EQ(seeded_requests[1].exec_job_id, 101);
    EXPECT_EQ(seeded_requests[2].exec_job_id, 101);
    ASSERT_TRUE(seeded_requests[0].override_start_rng_seed.has_value());
    ASSERT_TRUE(seeded_requests[1].override_start_rng_seed.has_value());
    EXPECT_EQ(*seeded_requests[0].override_start_rng_seed, 0x11111111u);
    EXPECT_EQ(*seeded_requests[1].override_start_rng_seed, 0x22222222u);
    EXPECT_FALSE(seeded_requests[0].override_fake_attacks_this_turn.has_value());
    ASSERT_TRUE(seeded_requests[1].override_fake_attacks_this_turn.has_value());
    ASSERT_TRUE(seeded_requests[2].override_fake_attacks_this_turn.has_value());
    EXPECT_EQ(*seeded_requests[1].override_fake_attacks_this_turn, 3u);
    EXPECT_EQ(*seeded_requests[2].override_fake_attacks_this_turn, 4u);
    const auto seeded_sources = unique_battle_job_batch_source_exec_job_ids(seeded_runs.options);
    ASSERT_EQ(seeded_sources.size(), 1u);
    EXPECT_EQ(seeded_sources[0], 101);

    const auto duplicate = parse_battle_job_batch_run_tokens(
        {
            "--exec-job-id", "101",
            "--exec-job-id", "101",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_FALSE(duplicate.errors.empty());

    const auto duplicate_seeded = parse_battle_job_batch_run_tokens(
        {
            "--exec-job-seed", "101:0x11111111:2",
            "--exec-job-seed", "101:0x11111111:2",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_FALSE(duplicate_seeded.errors.empty());

    const auto same_seed_different_fake = parse_battle_job_batch_run_tokens(
        {
            "--exec-job-seed", "101:0x11111111:1",
            "--exec-job-seed", "101:0x11111111:2",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_TRUE(same_seed_different_fake.errors.empty())
        << (same_seed_different_fake.errors.empty() ? "" : same_seed_different_fake.errors.front());

    const auto invalid_fake_override = parse_battle_job_batch_run_tokens(
        {
            "--exec-job-fake-attacks", "101:256",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_FALSE(invalid_fake_override.errors.empty());

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

    const auto std_options = parse_battle_job_batch_run_tokens(
        {
            "--exec-job-id", "101",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
            "--disc-dump-root", "D:/disc",
            "--std-disc-dump-root", "D:/disc/.",
            "--spice-file-parsing-exe", "D:/tools/SpiceFileParsing.exe",
        },
        "SavorPredict.exe");
    EXPECT_TRUE(std_options.errors.empty())
        << (std_options.errors.empty() ? "" : std_options.errors.front());
    EXPECT_EQ(std_options.options.disc_dump_root, std::filesystem::path("D:/disc"));
    EXPECT_NE(
        std::find_if(
            std_options.warnings.begin(),
            std_options.warnings.end(),
            [](const std::string& warning) {
                return warning.find("--spice-file-parsing-exe")
                    != std::string::npos
                    && warning.find("ignored") != std::string::npos;
            }),
        std_options.warnings.end());
    EXPECT_EQ(
        std_options.options.spice_file_parsing_exe,
        std::filesystem::path("D:/tools/SpiceFileParsing.exe"));
    const auto conflicting_resource_roots =
        parse_battle_job_batch_run_tokens(
            {
                "--exec-job-id", "101",
                "--iso", "D:/SoATAS/game.gcm",
                "--dolphin-base-dir", "D:/SoATAS/dolphin",
                "--disc-dump-root", "D:/disc",
                "--std-disc-dump-root", "D:/other",
            },
            "SavorPredict.exe");
    EXPECT_NE(
        std::find_if(
            conflicting_resource_roots.errors.begin(),
            conflicting_resource_roots.errors.end(),
            [](const std::string& error) {
                return error.find("must resolve to the same path")
                    != std::string::npos;
            }),
        conflicting_resource_roots.errors.end());

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

    const auto removed_battle_run_option = parse_battle_job_batch_run_tokens(
        {
            "--exec-job-id", "101",
            "--battle-run-ms", "900000",
            "--iso", "D:/SoATAS/game.gcm",
            "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_FALSE(removed_battle_run_option.errors.empty());

    const auto control_only = parse_battle_job_batch_run_tokens(
        {
            "--exec-job-id", "101", "--probe-mode", "control-only",
            "--iso", "D:/SoATAS/game.gcm", "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_TRUE(control_only.errors.empty())
        << (control_only.errors.empty() ? "" : control_only.errors.front());
    EXPECT_EQ(control_only.options.probe_mode, ProbeMode::ControlOnly);

    const auto jit = parse_battle_job_batch_run_tokens(
        {
            "--exec-job-id", "101", "--probe-cpu-core", "jit",
            "--iso", "D:/SoATAS/game.gcm", "--dolphin-base-dir", "D:/SoATAS/dolphin",
        },
        "SavorPredict.exe");
    EXPECT_TRUE(jit.errors.empty()) << (jit.errors.empty() ? "" : jit.errors.front());
    EXPECT_EQ(jit.options.probe_cpu_core, ProbeCpuCore::Jit);
}

TEST(
    SavorPredictBattleJobManifests,
    SerializeDirectAndLegacyResourceInputFieldsConditionally) {
    const auto root = std::filesystem::temp_directory_path()
        / ("savor_predict_resource_manifest_test_"
            + std::to_string(
                std::chrono::steady_clock::now()
                    .time_since_epoch()
                    .count()));
    ASSERT_TRUE(std::filesystem::create_directories(root));

    const auto direct_inputs = MakeManifestResourceInputs(
        BattlePredictorResourceProviderKind::DirectSpice);
    BattleJobRunSummary direct_single;
    direct_single.resource_inputs = direct_inputs;
    direct_single.options.resource_inputs = direct_inputs;
    std::ostringstream error;
    const auto direct_single_path = root / "direct-single.json";
    ASSERT_TRUE(write_battle_job_run_manifest(
        direct_single, direct_single_path, error))
        << error.str();
    const auto direct_single_json =
        ReadManifestText(direct_single_path);
    EXPECT_NE(
        direct_single_json.find(
            "\"schema\": \"savor_predict_battle_job_run_v2\""),
        std::string::npos);
    EXPECT_NE(
        direct_single_json.find(
            "\"provider_kind\":\"direct_spice\""),
        std::string::npos);
    EXPECT_NE(
        direct_single_json.find("\"normalized_relative_path\""),
        std::string::npos);
    EXPECT_NE(
        direct_single_json.find("\"source_path\""),
        std::string::npos);
    EXPECT_NE(
        direct_single_json.find("\"parser_identity\""),
        std::string::npos);
    EXPECT_NE(
        direct_single_json.find("\"diagnostic_count\""),
        std::string::npos);
    EXPECT_EQ(
        direct_single_json.find("\"std_json_cache\""),
        std::string::npos);

    BattleJobBatchRunSummary direct_batch;
    direct_batch.resource_inputs = direct_inputs;
    direct_batch.options.resource_inputs = direct_inputs;
    const auto direct_batch_path = root / "direct-batch.json";
    ASSERT_TRUE(write_battle_job_batch_run_manifest(
        direct_batch, direct_batch_path, error))
        << error.str();
    const auto direct_batch_json =
        ReadManifestText(direct_batch_path);
    EXPECT_NE(
        direct_batch_json.find(
            "\"schema\": \"savor_predict_battle_job_batch_run_v2\""),
        std::string::npos);
    EXPECT_NE(
        direct_batch_json.find(
            "\"provider_kind\":\"direct_spice\""),
        std::string::npos);
    EXPECT_EQ(
        direct_batch_json.find("\"std_json_cache\""),
        std::string::npos);

    const auto legacy_inputs = MakeManifestResourceInputs(
        BattlePredictorResourceProviderKind::LegacyStdJsonDirectMld);
    BattleJobRunSummary legacy_single;
    legacy_single.resource_inputs = legacy_inputs;
    legacy_single.options.resource_inputs = legacy_inputs;
    legacy_single.options.action_view_std_json_dir =
        "D:/fixture/std_json";
    const auto legacy_single_path = root / "legacy-single.json";
    ASSERT_TRUE(write_battle_job_run_manifest(
        legacy_single, legacy_single_path, error))
        << error.str();
    const auto legacy_single_json =
        ReadManifestText(legacy_single_path);
    EXPECT_NE(
        legacy_single_json.find(
            "\"provider_kind\":\"legacy_std_json_direct_mld\""),
        std::string::npos);
    EXPECT_NE(
        legacy_single_json.find("\"std_json_cache\""),
        std::string::npos);

    BattleJobBatchRunSummary legacy_batch;
    legacy_batch.resource_inputs = legacy_inputs;
    legacy_batch.options.resource_inputs = legacy_inputs;
    legacy_batch.options.action_view_std_json_dir =
        "D:/fixture/std_json";
    const auto legacy_batch_path = root / "legacy-batch.json";
    ASSERT_TRUE(write_battle_job_batch_run_manifest(
        legacy_batch, legacy_batch_path, error))
        << error.str();
    const auto legacy_batch_json =
        ReadManifestText(legacy_batch_path);
    EXPECT_NE(
        legacy_batch_json.find(
            "\"provider_kind\":\"legacy_std_json_direct_mld\""),
        std::string::npos);
    EXPECT_NE(
        legacy_batch_json.find("\"std_json_cache\""),
        std::string::npos);

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
}

TEST(SavorPredictProbeCpuCoreEnvironment, SetsAndRestoresWorkerOverride)
{
    constexpr const char* variable = "SAVOR_PROBE_CPU_CORE";
    ASSERT_EQ(_putenv_s(variable, "preserved"), 0);
    {
        ScopedProbeCpuCoreEnvironment environment(ProbeCpuCore::Interpreter);
        ASSERT_TRUE(environment.ok());
        const char* value = std::getenv(variable);
        ASSERT_NE(value, nullptr);
        EXPECT_STREQ(value, "interpreter");
    }
    const char* value = std::getenv(variable);
    ASSERT_NE(value, nullptr);
    EXPECT_STREQ(value, "preserved");
    ASSERT_EQ(_putenv_s(variable, ""), 0);
}

TEST(SavorPredictBattleJobClone, PatchesCaptureProfileWithoutDroppingJobFields)
{
    const auto patched = patch_battle_single_turn_capture_profile(
        BuildInputIni(10, 20, 30, 40, 2),
        "C:/runs/capture_profile.json",
        0x12345678u);
    const auto ini = IniDoc::parse(patched);
    EXPECT_EQ(ini.get_i64("BattleSingleTurn.Job", "wave_id", 0), 10);
    EXPECT_EQ(ini.get_i64("BattleSingleTurn.Job", "plan_id", 0), 20);
    EXPECT_EQ(ini.get_i64("BattleSingleTurn.Job", "seed_candidate_id", 0), 30);
    EXPECT_EQ(ini.get_i64("BattleSingleTurn.Job", "savestate_id", 0), 40);
    EXPECT_EQ(ini.get_i64("BattleSingleTurn.Job", "fake_attacks_this_turn", 0), 2);
    EXPECT_EQ(ini.get("BattleSingleTurn.Job", "target_variant_key", ""), "seeded");
    EXPECT_EQ(ini.get("BattleSingleTurn.Job", "capture_profile_path", ""), "C:/runs/capture_profile.json");
    EXPECT_EQ(ini.get_u32("BattleSingleTurn.Job", "override_start_rng_seed", 0), 0x12345678u);

    const auto patched_with_fake_attack_override = patch_battle_single_turn_capture_profile(
        BuildInputIni(10, 20, 30, 40, 2),
        "C:/runs/capture_profile.json",
        0x12345678u,
        0u);
    const auto overridden_ini = IniDoc::parse(patched_with_fake_attack_override);
    EXPECT_EQ(overridden_ini.get_u32("BattleSingleTurn.Job", "override_start_rng_seed", 0), 0x12345678u);
    EXPECT_EQ(overridden_ini.get_i64("BattleSingleTurn.Job", "fake_attacks_this_turn", -1), 0);
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
    options.override_start_rng_seed = 0xA5A5A5A5u;
    options.override_fake_attacks_this_turn = 0u;

    BattleJobCloneResult clone;
    std::ostringstream err;
    ASSERT_TRUE(clone_battle_job_for_capture(
        *db_service_,
        options,
        temp_root_ / "capture_profile.json",
        &clone,
        err)) << err.str();

    EXPECT_EQ(clone.original_turn_job_id, seeded.turn_job_id);
    EXPECT_EQ(clone.original_exec_job_id, seeded.exec_job_id);
    EXPECT_NE(clone.cloned_turn_job_id, seeded.turn_job_id);
    EXPECT_NE(clone.cloned_exec_job_id, seeded.exec_job_id);
    EXPECT_EQ(clone.source_fake_attacks_this_turn, 1);
    EXPECT_EQ(clone.fake_attacks_this_turn, 0);
    EXPECT_GE(clone.quarantined_ready_jobs, 1);
    ASSERT_TRUE(clone.override_fake_attacks_this_turn.has_value());
    EXPECT_EQ(*clone.override_fake_attacks_this_turn, 0u);

    const auto source_turn = db_service_->AnalysisDb()->GetBattleTurnJobForExecJob(seeded.exec_job_id);
    ASSERT_TRUE(source_turn.has_value());
    EXPECT_EQ(source_turn->turn_job_id, seeded.turn_job_id);

    const auto cloned_turn = db_service_->AnalysisDb()->GetBattleTurnJobForExecJob(clone.cloned_exec_job_id);
    ASSERT_TRUE(cloned_turn.has_value());
    EXPECT_EQ(cloned_turn->turn_job_id, clone.cloned_turn_job_id);
    EXPECT_EQ(cloned_turn->fake_attacks_this_turn, 0);

    const auto cloned_exec = db_service_->ExecutionDb()->GetJob(clone.cloned_exec_job_id);
    ASSERT_TRUE(cloned_exec.has_value());
    EXPECT_EQ(cloned_exec->program_ref_kind, "analysis_battle.turn_job");
    EXPECT_EQ(cloned_exec->program_ref_id, clone.cloned_turn_job_id);
    EXPECT_EQ(cloned_exec->state, "QUEUED");
    EXPECT_NE(cloned_exec->input_ini.find("capture_profile_path"), std::string::npos);
    const auto cloned_ini = IniDoc::parse(cloned_exec->input_ini);
    EXPECT_EQ(cloned_ini.get_i64("BattleSingleTurn.Job", "fake_attacks_this_turn", -1), 0);

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
        temp_root_ / "capture_profile.json",
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

TEST_F(SavorPredictBattleJobRunnerDb, BatchCloneAllowsSameSourceExecWithDifferentSeeds)
{
    const auto seeded = SeedBattleJob(201);

    BattleJobBatchCloneResult batch;
    std::ostringstream err;
    ASSERT_TRUE(clone_battle_jobs_for_capture(
        *db_service_,
        std::vector<BattleJobCloneRequest>{
            {
                .source_exec_job_id = seeded.exec_job_id,
                .override_start_rng_seed = 0x11111111u,
                .override_fake_attacks_this_turn = 0u,
            },
            {
                .source_exec_job_id = seeded.exec_job_id,
                .override_start_rng_seed = 0x22222222u,
                .override_fake_attacks_this_turn = 2u,
            },
        },
        temp_root_ / "capture_profile.json",
        &batch,
        err)) << err.str();

    ASSERT_EQ(batch.clones.size(), 2u);
    EXPECT_EQ(batch.clones[0].original_exec_job_id, seeded.exec_job_id);
    EXPECT_EQ(batch.clones[1].original_exec_job_id, seeded.exec_job_id);
    EXPECT_NE(batch.clones[0].cloned_exec_job_id, batch.clones[1].cloned_exec_job_id);
    EXPECT_NE(batch.clones[0].cloned_turn_job_id, batch.clones[1].cloned_turn_job_id);
    ASSERT_TRUE(batch.clones[0].override_start_rng_seed.has_value());
    ASSERT_TRUE(batch.clones[1].override_start_rng_seed.has_value());
    ASSERT_TRUE(batch.clones[0].override_fake_attacks_this_turn.has_value());
    ASSERT_TRUE(batch.clones[1].override_fake_attacks_this_turn.has_value());
    EXPECT_EQ(*batch.clones[0].override_start_rng_seed, 0x11111111u);
    EXPECT_EQ(*batch.clones[1].override_start_rng_seed, 0x22222222u);
    EXPECT_EQ(*batch.clones[0].override_fake_attacks_this_turn, 0u);
    EXPECT_EQ(*batch.clones[1].override_fake_attacks_this_turn, 2u);
    EXPECT_EQ(batch.clones[0].source_fake_attacks_this_turn, 1);
    EXPECT_EQ(batch.clones[1].source_fake_attacks_this_turn, 1);
    EXPECT_EQ(batch.clones[0].fake_attacks_this_turn, 0);
    EXPECT_EQ(batch.clones[1].fake_attacks_this_turn, 2);

    const auto first_ini = IniDoc::parse(batch.clones[0].patched_input_ini);
    const auto second_ini = IniDoc::parse(batch.clones[1].patched_input_ini);
    EXPECT_EQ(first_ini.get_u32("BattleSingleTurn.Job", "override_start_rng_seed", 0), 0x11111111u);
    EXPECT_EQ(second_ini.get_u32("BattleSingleTurn.Job", "override_start_rng_seed", 0), 0x22222222u);
    EXPECT_EQ(first_ini.get_i64("BattleSingleTurn.Job", "fake_attacks_this_turn", -1), 0);
    EXPECT_EQ(second_ini.get_i64("BattleSingleTurn.Job", "fake_attacks_this_turn", -1), 2);

    for (const auto& clone : batch.clones) {
        const auto cloned_turn = db_service_->AnalysisDb()->GetBattleTurnJobForExecJob(clone.cloned_exec_job_id);
        ASSERT_TRUE(cloned_turn.has_value());
        EXPECT_EQ(cloned_turn->turn_job_id, clone.cloned_turn_job_id);

        const auto cloned_exec = db_service_->ExecutionDb()->GetJob(clone.cloned_exec_job_id);
        ASSERT_TRUE(cloned_exec.has_value());
        EXPECT_EQ(cloned_exec->state, "QUEUED");
        EXPECT_NE(cloned_exec->input_ini.find("capture_profile_path"), std::string::npos);
        EXPECT_NE(cloned_exec->input_ini.find("override_start_rng_seed"), std::string::npos);
    }

    const auto unrelated_state = QueryText(db_, "SELECT state FROM exec_job WHERE job_id=?1;", seeded.unrelated_exec_job_id);
    ASSERT_TRUE(unrelated_state.has_value());
    EXPECT_EQ(*unrelated_state, "SUPERSEDED");
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
    options.override_start_rng_seed = 0xA5A5A5A5u;
    options.override_fake_attacks_this_turn = 0u;

    const auto capture_profile = temp_root_ / "capture_profile.json";
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
    EXPECT_NE(output_path.find("battle_capture.scap"), std::string::npos);
    EXPECT_NE(output_path.find("job-" + std::to_string(clone.cloned_exec_job_id)), std::string::npos);

    uint32_t override_enabled = 0;
    uint32_t override_seed = 0;
    ASSERT_TRUE(ctx.get(savor::context::key::battle::RNG_OVERRIDE_ENABLED, override_enabled));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::RNG_OVERRIDE_SEED, override_seed));
    EXPECT_EQ(override_enabled, 1u);
    EXPECT_EQ(override_seed, 0xA5A5A5A5u);
    uint32_t fake_attacks_this_turn = 99;
    ASSERT_TRUE(ctx.get(savor::context::key::battle::FAKE_ATTACK_COUNT_THIS_TURN, fake_attacks_this_turn));
    EXPECT_EQ(fake_attacks_this_turn, 0u);
}

} // namespace
