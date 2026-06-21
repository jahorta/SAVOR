#include <gtest/gtest.h>

#include "DbRootCopy.h"

#include "Common/DbService.h"
#include "Common/Types/UtcTimestamp.h"
#include "Execution/IExecutionDb.h"
#include "Runner/IPC/Wire.h"
#include "Utils/IniDoc.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string BuildBattleInputIni(
    std::int64_t wave_id,
    std::int64_t plan_id,
    std::int64_t seed_candidate_id,
    std::int64_t savestate_id) {
    IniDoc ini;
    ini.set("BattleSingleTurn.Job", "wave_id", std::to_string(wave_id));
    ini.set("BattleSingleTurn.Job", "plan_id", std::to_string(plan_id));
    ini.set("BattleSingleTurn.Job", "seed_candidate_id", std::to_string(seed_candidate_id));
    ini.set("BattleSingleTurn.Job", "savestate_id", std::to_string(savestate_id));
    ini.set("BattleSingleTurn.Job", "turn_index", "1");
    ini.set("BattleSingleTurn.Job", "fake_attacks_used_before", "0");
    ini.set("BattleSingleTurn.Job", "fake_attacks_this_turn", "1");
    ini.set("BattleSingleTurn.Job", "action_key", "attack-enemy:seeded");
    ini.set("BattleSingleTurn.Job", "resolved_turn_commands_blob", "");
    ini.set("BattleSingleTurn.Job", "resolved_turn_variant_key", "seeded");
    ini.set("BattleSingleTurn.Job", "concrete_turn_plan_hex", "");
    ini.set("BattleSingleTurn.Job", "target_variant_key", "seeded");
    return ini.to_string_sorted();
}

std::optional<std::int64_t> QueryI64(const std::filesystem::path& db_path, const char* sql, std::int64_t arg = 0) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) {
        if (db != nullptr) sqlite3_close(db);
        return std::nullopt;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return std::nullopt;
    }
    if (arg != 0) {
        sqlite3_bind_int64(st, 1, arg);
    }
    std::optional<std::int64_t> out;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
        out = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

std::optional<std::string> QueryText(const std::filesystem::path& db_path, const char* sql, std::int64_t arg) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) {
        if (db != nullptr) sqlite3_close(db);
        return std::nullopt;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return std::nullopt;
    }
    sqlite3_bind_int64(st, 1, arg);
    std::optional<std::string> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const auto* text = sqlite3_column_text(st, 0);
        out = text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

void ExecuteSqlOrThrow(const std::filesystem::path& db_path, const std::string& sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) {
        std::string msg = db != nullptr ? sqlite3_errmsg(db) : "sqlite open failed";
        if (db != nullptr) sqlite3_close(db);
        throw std::runtime_error(msg);
    }
    char* err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err != nullptr ? err : sqlite3_errmsg(db);
        sqlite3_free(err);
        sqlite3_close(db);
        throw std::runtime_error(msg);
    }
    sqlite3_close(db);
}

void RequireFixtureStep(bool ok, const std::string& err, const char* context) {
    if (!ok) {
        throw std::runtime_error(std::string(context) + ": " + err);
    }
}

struct SeededSourceBattle {
    std::int64_t savestate_id = 0;
    std::int64_t artifact_id = 0;
    std::int64_t battle_set_id = 0;
    std::int64_t wave_id = 0;
    std::int64_t seed_candidate_id = 0;
    std::int64_t plan_id = 0;
    std::int64_t turn_job_id = 0;
    std::int64_t exec_job_id = 0;
    std::int64_t job_set_id = 0;
    std::int64_t unrelated_exec_job_id = 0;
    std::filesystem::path source_sav_path;
};

class SavorDbUtilsMinimalCopyFixture : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path()
            / ("savor_dbutils_copy_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(std::filesystem::create_directories(root_));
    }

    void TearDown() override {
        if (!root_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(root_, ec);
        }
    }

    SeededSourceBattle SeedSourceDb(const std::filesystem::path& source_root, int turn_index = 1) {
        using namespace savor::db;

        savor::db::core::DBService db_service(
            savor::dbutils::MakeDbConfigPaths(source_root),
            savor::db::migrations::MigrationSourceOptions{ .source_kind = savor::db::migrations::MigrationSourceKind::Embedded });
        std::string err;
        RequireFixtureStep(db_service.Start(&err), err, "start source DB service");

        const auto now = types::UtcTimePoint(std::chrono::milliseconds(1781000000200));
        SeededSourceBattle seeded;
        seeded.source_sav_path = source_root / "seed.sav";
        {
            std::ofstream sav(seeded.source_sav_path, std::ios::binary | std::ios::trunc);
            sav << "source sav bytes";
        }

        RequireFixtureStep(db_service.StateDb()->StoreArtifact(
            {
                .sha256 = "seed-sav-hash-" + std::to_string(turn_index),
                .size_bytes = static_cast<std::int64_t>(std::filesystem::file_size(seeded.source_sav_path)),
                .filename = seeded.source_sav_path.string(),
                .file_ext = ".sav",
                .artifact_kind = "SAV",
                .created_at_utc = now,
                .correlation_id = "dbutils-test",
                .causation_id = "seed",
            },
            &seeded.artifact_id,
            &err), err, "store source artifact");
        RequireFixtureStep(db_service.StateDb()->CreateSavestate(
            {
                .artifact_id = seeded.artifact_id,
                .savestate_type = "BATTLE",
                .note = "source",
                .is_complete = true,
                .created_at_utc = now,
                .correlation_id = "dbutils-test",
                .causation_id = "artifact",
            },
            &seeded.savestate_id,
            &err), err, "create source savestate");

        std::int64_t run_spec_id = 0;
        RequireFixtureStep(db_service.AuthoringDb()->SaveBattleRunSpec(
            {
                .name = "dbutils-run-spec-" + std::to_string(turn_index),
                .priority = 1,
                .run_ms = 1000,
                .vi_stall_ms = 0,
                .use_single_turn_runner = true,
                .created_at_utc = now,
                .correlation_id = "dbutils-test",
                .causation_id = "seed",
            },
            &run_spec_id,
            &err), err, "save battle run spec");
        RequireFixtureStep(db_service.AuthoringDb()->SavePlan(
            {
                .name = "dbutils-plan-" + std::to_string(turn_index),
                .fingerprint = "dbutils-plan-" + std::to_string(turn_index),
                .num_turns = 1,
                .created_at_utc = now,
                .correlation_id = "dbutils-test",
                .causation_id = "seed",
            },
            &seeded.plan_id,
            &err), err, "save battle plan");
        std::int64_t action_preset_id = 0;
        RequireFixtureStep(db_service.AuthoringDb()->SaveBattlePlanActionPreset(
            {
                .name = "dbutils-attack-" + std::to_string(turn_index),
                .macro = BattlePlanActionMacro::Attack,
                .target_kind = BattlePlanTargetKind::SingleEnemy,
                .target_single_slot = 4,
                .created_at_utc = now,
                .correlation_id = "dbutils-test",
                .causation_id = "seed",
            },
            &action_preset_id,
            &err), err, "save action preset");
        std::int64_t plan_turn_id = 0;
        RequireFixtureStep(db_service.AuthoringDb()->SaveBattlePlanTurn(
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
                .correlation_id = "dbutils-test",
                .causation_id = "seed",
            },
            &plan_turn_id,
            &err), err, "save plan turn");
        std::int64_t settings_id = 0;
        RequireFixtureStep(db_service.AuthoringDb()->SaveExplorerSettings(
            {
                .name = "dbutils-settings-" + std::to_string(turn_index),
                .default_plan_id = seeded.plan_id,
                .created_at_utc = now,
                .correlation_id = "dbutils-test",
                .causation_id = "seed",
            },
            &settings_id,
            &err), err, "save explorer settings");

        RequireFixtureStep(db_service.AnalysisDb()->CreateBattleSet(
            {
                .name = "dbutils-set-" + std::to_string(turn_index),
                .entry_savestate_id = seeded.savestate_id,
                .battle_run_spec_id = run_spec_id,
                .explorer_settings_id = settings_id,
                .status = BattleSetStatus::Active,
                .created_at_utc = now,
                .correlation_id = "dbutils-test",
                .causation_id = "seed",
            },
            &seeded.battle_set_id,
            &err), err, "create battle set");
        RequireFixtureStep(db_service.AnalysisDb()->AddBattleSeedCandidate(
            {
                .battle_set_id = seeded.battle_set_id,
                .seed_value = 12345,
                .source_kind = BattleSeedCandidateSourceKind::Synthetic,
                .candidate_status = BattleSeedCandidateStatus::Ready,
                .created_at_utc = now,
                .correlation_id = "dbutils-test",
                .causation_id = "seed",
            },
            &seeded.seed_candidate_id,
            &err), err, "add seed candidate");
        RequireFixtureStep(db_service.AnalysisDb()->CreateBattleTurnWave(
            {
                .battle_set_id = seeded.battle_set_id,
                .turn_index = turn_index,
                .seed_candidate_id = seeded.seed_candidate_id,
                .status = BattleTurnWaveStatus::Ready,
                .created_at_utc = now,
                .correlation_id = "dbutils-test",
                .causation_id = "seed",
            },
            &seeded.wave_id,
            &err), err, "create battle turn wave");
        RequireFixtureStep(db_service.AnalysisDb()->RecordBattleTurnJob(
            {
                .wave_id = seeded.wave_id,
                .plan_id = seeded.plan_id,
                .source_savestate_id = seeded.savestate_id,
                .seed_candidate_id = seeded.seed_candidate_id,
                .authored_plan_id = seeded.plan_id,
                .authored_turn_index = turn_index,
                .resolved_turn_commands_blob = std::string{},
                .resolved_turn_variant_key = std::string("seeded"),
                .fake_attacks_this_turn = 1,
                .fake_attacks_used_before = 0,
                .job_state = BattleTurnJobState::Queued,
                .started_at_utc = now,
                .recorded_at_utc = now,
                .correlation_id = "dbutils-test",
                .causation_id = "seed",
            },
            &seeded.turn_job_id,
            &err), err, "record battle turn job");

        RequireFixtureStep(db_service.ExecutionDb()->CreateJobSet(
            {
                .program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),
                .purpose = "dbutils-test",
                .created_by = std::string("test"),
                .created_at_utc = now.time_since_epoch().count(),
            },
            &seeded.job_set_id,
            &err), err, "create execution job set");
        RequireFixtureStep(db_service.ExecutionDb()->EnqueueJob(
            {
                .job_set_id = seeded.job_set_id,
                .program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),
                .program_version = 4,
                .program_ref_kind = "analysis_battle.turn_job",
                .program_ref_id = seeded.turn_job_id,
                .savestate_id = seeded.savestate_id,
                .fingerprint = "dbutils-original-" + std::to_string(turn_index),
                .priority = 10,
                .max_attempts = 1,
                .input_ini = BuildBattleInputIni(seeded.wave_id, seeded.plan_id, seeded.seed_candidate_id, seeded.savestate_id),
            },
            &seeded.exec_job_id,
            &err), err, "enqueue selected execution job");
        RequireFixtureStep(db_service.AnalysisDb()->SetBattleTurnJobExecJobId(seeded.turn_job_id, seeded.exec_job_id, &err),
            err,
            "link turn job to execution job");
        RequireFixtureStep(db_service.ExecutionDb()->EnqueueJob(
            {
                .job_set_id = seeded.job_set_id,
                .program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),
                .program_version = 4,
                .program_ref_kind = "analysis_battle.turn_job",
                .program_ref_id = seeded.turn_job_id,
                .savestate_id = seeded.savestate_id,
                .fingerprint = "dbutils-unrelated-" + std::to_string(turn_index),
                .priority = 9,
                .max_attempts = 1,
                .input_ini = BuildBattleInputIni(seeded.wave_id, seeded.plan_id, seeded.seed_candidate_id, seeded.savestate_id),
            },
            &seeded.unrelated_exec_job_id,
            &err), err, "enqueue unrelated execution job");

        db_service.Stop();
        const auto workflow_instance_id = 9000 + turn_index;
        const auto workflow_activation_id = 9100 + turn_index;
        const auto workflow_step_id = 9200 + turn_index;
        ExecuteSqlOrThrow(
            source_root / "execution.db",
            "INSERT INTO exec_workflow_instance("
            "workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,created_by,created_at_utc,workflow_graph_revision_id) VALUES("
            + std::to_string(workflow_instance_id) + ",'fixture','COMPLETED','manual',NULL,'test',1781000000200,1);"
            "INSERT INTO exec_workflow_unit_activation("
            "workflow_unit_activation_id,workflow_instance_id,activation_key,graph_node_key,unit_kind,display_name,state,activation_params_json,created_at_utc) VALUES("
            + std::to_string(workflow_activation_id) + "," + std::to_string(workflow_instance_id)
            + ",'fixture_activation','fixture_node','fixture_unit','Fixture Unit','COMPLETED','{}',1781000000200);"
            "INSERT INTO exec_workflow_step("
            "workflow_step_id,workflow_instance_id,workflow_unit_activation_id,step_key,step_kind,state,priority,attempts,max_attempts,job_set_id,ready_at_utc,created_at_utc,graph_node_key) VALUES("
            + std::to_string(workflow_step_id) + "," + std::to_string(workflow_instance_id) + "," + std::to_string(workflow_activation_id)
            + ",'fixture_step','battle.single_turn','COMPLETED',1,0,1," + std::to_string(seeded.job_set_id)
            + ",1781000000200,1781000000200,'fixture_node');");
        return seeded;
    }

    std::filesystem::path root_;
};

TEST_F(SavorDbUtilsMinimalCopyFixture, FullCopyCopiesDbFilesAndStores)
{
    const auto source_root = root_ / "source";
    const auto target_root = root_ / "target";
    ASSERT_TRUE(std::filesystem::create_directories(source_root / "object_store" / "nested"));
    {
        std::ofstream file(source_root / "object_store" / "nested" / "blob.bin", std::ios::binary | std::ios::trunc);
        file << "blob";
    }
    SeedSourceDb(source_root);

    std::ostringstream out;
    std::ostringstream err;
    ASSERT_EQ(savor::dbutils::CopyDbRootFull(
        {
            .source_root = source_root,
            .dest_root = target_root,
            .overwrite = false,
        },
        out,
        err), 0) << err.str();
    EXPECT_TRUE(std::filesystem::exists(target_root / "analysis.db"));
    EXPECT_TRUE(std::filesystem::exists(target_root / "execution.db"));
    EXPECT_TRUE(std::filesystem::exists(target_root / "object_store" / "nested" / "blob.bin"));
}

TEST_F(SavorDbUtilsMinimalCopyFixture, MinimalCopyPreservesSelectedClosureAndLocalizesSavestateArtifact)
{
    const auto source_root = root_ / "source";
    const auto target_root = root_ / "target";
    const auto artifact_root = root_ / "run" / "source-artifacts";
    const auto seeded = SeedSourceDb(source_root);

    savor::dbutils::BattleSingleTurnJobSubsetResult result;
    std::ostringstream out;
    std::ostringstream err;
    ASSERT_EQ(savor::dbutils::HydrateBattleSingleTurnJobSubset(
        {
            .source_root = source_root,
            .target_root = target_root,
            .artifact_root = artifact_root,
            .selector = { .exec_job_id = seeded.exec_job_id },
        },
        &result,
        out,
        err), 0) << err.str();

    EXPECT_EQ(result.source_exec_job_id, seeded.exec_job_id);
    EXPECT_EQ(result.source_turn_job_id, seeded.turn_job_id);
    EXPECT_FALSE(result.table_counts.empty());
    ASSERT_EQ(result.copied_artifacts.size(), 1u);
    EXPECT_EQ(result.copied_artifacts.front().artifact_id, seeded.artifact_id);
    EXPECT_TRUE(std::filesystem::exists(result.copied_artifacts.front().copied_path));

    const auto target_paths = savor::dbutils::MakeDbConfigPaths(target_root);
    EXPECT_EQ(QueryI64(target_paths.execution_db_path, "SELECT COUNT(*) FROM exec_job;").value_or(-1), 1);
    EXPECT_EQ(QueryI64(target_paths.execution_db_path, "SELECT job_id FROM exec_job;", 0).value_or(-1), seeded.exec_job_id);
    EXPECT_EQ(QueryI64(target_paths.execution_db_path, "SELECT COUNT(*) FROM exec_job WHERE job_id=?1;", seeded.unrelated_exec_job_id).value_or(-1), 0);
    EXPECT_EQ(QueryI64(target_paths.execution_db_path, "SELECT COUNT(*) FROM exec_workflow_step WHERE job_set_id=?1;", seeded.job_set_id).value_or(-1), 1);
    EXPECT_EQ(QueryI64(target_paths.execution_db_path, "SELECT COUNT(*) FROM exec_workflow_instance;").value_or(-1), 1);
    EXPECT_EQ(QueryI64(target_paths.execution_db_path, "SELECT COUNT(*) FROM exec_workflow_unit_activation;").value_or(-1), 1);
    EXPECT_EQ(QueryI64(target_paths.analysis_db_path, "SELECT COUNT(*) FROM ab_turn_job WHERE turn_job_id=?1;", seeded.turn_job_id).value_or(-1), 1);

    const auto localized = QueryText(target_paths.state_db_path, "SELECT filename FROM state_artifact WHERE artifact_id=?1;", seeded.artifact_id);
    ASSERT_TRUE(localized.has_value());
    EXPECT_NE(localized->find((root_ / "run" / "source-artifacts").string()), std::string::npos);
}

TEST_F(SavorDbUtilsMinimalCopyFixture, MinimalCopyRejectsMissingSavestateArtifactFile)
{
    const auto source_root = root_ / "source";
    const auto target_root = root_ / "target";
    const auto seeded = SeedSourceDb(source_root);
    ASSERT_TRUE(std::filesystem::remove(seeded.source_sav_path));

    savor::dbutils::BattleSingleTurnJobSubsetResult result;
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_NE(savor::dbutils::HydrateBattleSingleTurnJobSubset(
        {
            .source_root = source_root,
            .target_root = target_root,
            .artifact_root = root_ / "run" / "source-artifacts",
            .selector = { .exec_job_id = seeded.exec_job_id },
        },
        &result,
        out,
        err), 0);
    EXPECT_FALSE(result.validation_errors.empty());
}

TEST_F(SavorDbUtilsMinimalCopyFixture, MinimalCopyRejectsNonFirstTurnJob)
{
    const auto source_root = root_ / "source";
    const auto target_root = root_ / "target";
    const auto seeded = SeedSourceDb(source_root, 2);

    savor::dbutils::BattleSingleTurnJobSubsetResult result;
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_NE(savor::dbutils::HydrateBattleSingleTurnJobSubset(
        {
            .source_root = source_root,
            .target_root = target_root,
            .artifact_root = root_ / "run" / "source-artifacts",
            .selector = { .exec_job_id = seeded.exec_job_id },
        },
        &result,
        out,
        err), 0);
    EXPECT_FALSE(result.validation_errors.empty());
}

} // namespace
