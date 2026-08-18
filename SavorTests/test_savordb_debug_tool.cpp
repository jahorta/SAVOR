#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "BackfillAnalysisBattle.h"
#include "ReprojectUiRead.h"

#include "Analysis/IAnalysisDb.h"
#include "Common/DbService.h"
#include "Common/Migrations/MigrationRunner.h"
#include "Execution/IExecutionDb.h"
#include "Runner/Runtime/ProgramKind.h"
#include "common/savordb_helpers.h"

namespace {

struct SqliteHandle {
    sqlite3* db = nullptr;
    ~SqliteHandle() {
        if (db != nullptr) {
            sqlite3_close(db);
        }
    }
};

std::filesystem::path MakeDebugToolTempRoot(const std::string& suffix) {
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto root = std::filesystem::temp_directory_path() / ("savordb-debug-tool-" + suffix + "-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    return root;
}

savor::db::DbConfigPaths MakeDebugToolDbPaths(const std::filesystem::path& root) {
    savor::db::DbConfigPaths paths{};
    paths.execution_db_path = root / "execution.db";
    paths.state_db_path = root / "state.db";
    paths.analysis_db_path = root / "analysis.db";
    paths.authoring_db_path = root / "authoring.db";
    paths.ui_read_db_path = root / "ui_read.db";
    paths.archive_db_path = root / "archive.db";
    paths.object_store_root = root / "object_store";
    paths.archive_store_root = root / "archive_store";
    return paths;
}

bool OpenSqlite(const std::filesystem::path& path, SqliteHandle* handle) {
    return handle != nullptr
        && sqlite3_open(path.string().c_str(), &handle->db) == SQLITE_OK
        && sqlite3_busy_timeout(handle->db, 5000) == SQLITE_OK;
}

std::int64_t ReadInt64(sqlite3* db, const char* sql) {
    sqlite3_stmt* st = nullptr;
    EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &st, nullptr));
    if (st == nullptr) {
        return 0;
    }
    std::int64_t value = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        value = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return value;
}

std::string ReadText(sqlite3* db, const char* sql) {
    sqlite3_stmt* st = nullptr;
    EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &st, nullptr));
    if (st == nullptr) {
        return {};
    }
    std::string value;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0) != nullptr) {
        value = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    return value;
}

bool ExecSqlWithError(sqlite3* db, const char* sql, std::string* error_out = nullptr) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK && error_out != nullptr) {
        *error_out = err != nullptr ? err : sqlite3_errmsg(db);
    }
    sqlite3_free(err);
    return rc == SQLITE_OK;
}

const savor::db::uiread::projectors::UiReadProjectionStreamTelemetrySnapshot* FindProjectionStream(
    const savor::db::core::DBServicePerformanceSnapshot& snapshot,
    const std::string& stream_id) {
    const auto& streams = snapshot.ui_read_projection.streams;
    const auto it = std::find_if(streams.begin(), streams.end(), [&](const auto& stream) {
        return stream.stream_id == stream_id;
    });
    return it == streams.end() ? nullptr : &(*it);
}

void RunProjectionUntilCaughtUp(savor::db::core::DBService& service, const std::string& stream_id) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::string err;
        ASSERT_TRUE(service.RunUiReadProjectionOnce(&err)) << err;
        const auto snapshot = service.SnapshotPerformance();
        const auto* stream = FindProjectionStream(snapshot, stream_id);
        ASSERT_NE(stream, nullptr);
        if (stream->lag_count == 0 && stream->dirty_count == 0 && stream->last_outbox_id == stream->source_high_water_outbox_id) {
            return;
        }
    }
    FAIL() << "projection did not catch up: " << stream_id;
}

struct SeededDebugDb {
    std::filesystem::path root;
    savor::db::DbConfigPaths paths;
    std::int64_t battle_set_id = 0;
    std::int64_t wave_id = 0;
};

struct BackfillSeededDebugDb : SeededDebugDb {
    std::int64_t turn1_wave_id = 0;
    std::int64_t turn1_job_id = 0;
    std::int64_t turn2_wave_id = 0;
    std::int64_t turn2_job_id = 0;
};

std::string BattleSingleTurnIni(
    std::int64_t wave_id,
    std::int64_t plan_id,
    std::int64_t seed_candidate_id,
    std::int64_t savestate_id,
    int turn_index,
    const std::string& command_blob,
    const std::string& variant_key,
    bool use_new_keys) {
    std::ostringstream out;
    out << "[BattleSingleTurn.Job]\n"
        << "wave_id=" << wave_id << "\n"
        << "plan_id=" << plan_id << "\n"
        << "seed_candidate_id=" << seed_candidate_id << "\n"
        << "savestate_id=" << savestate_id << "\n"
        << "turn_index=" << turn_index << "\n"
        << "fake_attacks_used_before=2\n"
        << "fake_attacks_this_turn=1\n";
    if (use_new_keys) {
        out << "resolved_turn_commands_blob=" << command_blob << "\n"
            << "resolved_turn_variant_key=" << variant_key << "\n";
    } else {
        out << "concrete_turn_plan_hex=" << command_blob << "\n"
            << "target_variant_key=" << variant_key << "\n";
    }
    return out.str();
}

std::string BattleSingleTurnIniWithFallbacksOnly(const std::string& command_blob) {
    std::ostringstream out;
    out << "[BattleSingleTurn.Job]\n"
        << "fake_attacks_used_before=2\n"
        << "fake_attacks_this_turn=1\n"
        << "resolved_turn_commands_blob=" << command_blob << "\n";
    return out.str();
}

std::int64_t EnqueueBackfillExecJob(
    savor::db::IExecutionDb* execution_db,
    std::int64_t job_set_id,
    std::int64_t turn_job_id,
    std::int64_t savestate_id,
    const std::string& input_ini,
    const std::string& fingerprint_suffix) {
    std::int64_t exec_job_id = 0;
    std::string err;
    EXPECT_TRUE(execution_db->EnqueueJob(
        {
            .job_set_id = job_set_id,
            .program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),
            .program_version = 1,
            .program_ref_kind = "analysis_battle.turn_job",
            .program_ref_id = turn_job_id,
            .savestate_id = savestate_id,
            .fingerprint = "backfill-test-" + fingerprint_suffix,
            .priority = 0,
            .max_attempts = 1,
            .input_ini = input_ini,
        },
        &exec_job_id,
        &err)) << err;
    return exec_job_id;
}

std::int64_t RecordBackfillTurnJob(
    savor::db::IAnalysisDb* analysis_db,
    std::int64_t wave_id,
    std::int64_t plan_id,
    const savor::db::types::UtcTimePoint& now,
    std::optional<std::int64_t> source_savestate_id = std::nullopt,
    std::optional<std::int64_t> seed_candidate_id = std::nullopt,
    std::optional<std::int64_t> authored_plan_id = std::nullopt,
    std::optional<int> authored_turn_index = std::nullopt,
    std::optional<std::string> resolved_turn_commands_blob = std::nullopt,
    std::optional<std::string> resolved_turn_variant_key = std::nullopt,
    std::optional<std::int64_t> output_savestate_id = std::nullopt) {
    std::int64_t turn_job_id = 0;
    std::string err;
    EXPECT_TRUE(analysis_db->RecordBattleTurnJob(
        {
            .wave_id = wave_id,
            .plan_id = plan_id,
            .source_savestate_id = source_savestate_id,
            .seed_candidate_id = seed_candidate_id,
            .authored_plan_id = authored_plan_id,
            .authored_turn_index = authored_turn_index,
            .resolved_turn_commands_blob = resolved_turn_commands_blob,
            .resolved_turn_variant_key = resolved_turn_variant_key,
            .fake_attacks_this_turn = 1,
            .fake_attacks_used_before = 2,
            .job_state = savor::db::BattleTurnJobState::Queued,
            .output_savestate_id = output_savestate_id,
            .recorded_at_utc = now,
            .correlation_id = "backfill",
            .causation_id = "test",
        },
        &turn_job_id,
        &err)) << err;
    return turn_job_id;
}

void LinkBackfillExecJob(savor::db::IAnalysisDb* analysis_db, std::int64_t turn_job_id, std::int64_t exec_job_id) {
    std::string err;
    ASSERT_TRUE(analysis_db->SetBattleTurnJobExecJobId(turn_job_id, exec_job_id, &err)) << err;
}

void SeedAnalysisBattleBackfillDb(BackfillSeededDebugDb* seeded) {
    using namespace savor::db;

    ASSERT_NE(seeded, nullptr);
    seeded->root = MakeDebugToolTempRoot("analysis-battle-backfill");
    seeded->paths = MakeDebugToolDbPaths(seeded->root);

    savor::db::core::DBService service(
        seeded->paths,
        savor::db::migrations::MigrationSourceOptions{ .source_kind = savor::db::migrations::MigrationSourceKind::Embedded });
    std::string err;
    EXPECT_TRUE(service.Start(&err)) << err;

    auto* analysis_db = service.AnalysisDb();
    auto* execution_db = service.ExecutionDb();
    EXPECT_NE(analysis_db, nullptr);
    EXPECT_NE(execution_db, nullptr);
    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304200000));

    ASSERT_TRUE(analysis_db->CreateBattleSet(
        {
            .name = "debug-tool-backfill",
            .entry_savestate_id = 111,
            .battle_plan_id = 222,
            .battle_plan_fingerprint = "debug-tool-plan",
            .continuation_mode = BattleContinuationMode::AutomaticBestPerEndingRng,
            .status = BattleSetStatus::Active,
            .created_at_utc = now,
            .correlation_id = "backfill",
            .causation_id = "test",
        },
        &seeded->battle_set_id,
        &err)) << err;

    std::int64_t seed_candidate_id = 0;
    ASSERT_TRUE(analysis_db->AddBattleSeedCandidate(
        {
            .battle_set_id = seeded->battle_set_id,
            .seed_value = 777,
            .source_kind = BattleSeedCandidateSourceKind::Synthetic,
            .candidate_status = BattleSeedCandidateStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "backfill",
            .causation_id = "test",
        },
        &seed_candidate_id,
        &err)) << err;

    ASSERT_TRUE(analysis_db->CreateBattleTurnWave(
        {
            .battle_set_id = seeded->battle_set_id,
            .turn_index = 1,
            .seed_candidate_id = seed_candidate_id,
            .status = BattleTurnWaveStatus::Running,
            .created_at_utc = now,
            .correlation_id = "backfill",
            .causation_id = "test",
        },
        &seeded->turn1_wave_id,
        &err)) << err;

    seeded->turn1_job_id = RecordBackfillTurnJob(analysis_db, seeded->turn1_wave_id, 9101, now, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, 2222);

    ASSERT_TRUE(analysis_db->CreateBattleTurnWave(
        {
            .battle_set_id = seeded->battle_set_id,
            .turn_index = 2,
            .parent_wave_id = seeded->turn1_wave_id,
            .parent_turn_job_id = seeded->turn1_job_id,
            .seed_candidate_id = seed_candidate_id,
            .status = BattleTurnWaveStatus::Running,
            .created_at_utc = now,
            .correlation_id = "backfill",
            .causation_id = "test",
        },
        &seeded->turn2_wave_id,
        &err)) << err;
    seeded->wave_id = seeded->turn2_wave_id;

    seeded->turn2_job_id = RecordBackfillTurnJob(analysis_db, seeded->turn2_wave_id, 9102, now);

    const auto complete_job_id = RecordBackfillTurnJob(
        analysis_db,
        seeded->turn2_wave_id,
        9103,
        now,
        9999,
        9998,
        9997,
        9,
        std::string("01000000000004ffff"),
        std::string("preserve-variant"));
    const auto invalid_blob_job_id = RecordBackfillTurnJob(analysis_db, seeded->turn2_wave_id, 9104, now);
    const auto missing_exec_job_id = RecordBackfillTurnJob(analysis_db, seeded->turn2_wave_id, 9105, now);
    (void)missing_exec_job_id;
    const auto invalid_ini_job_id = RecordBackfillTurnJob(analysis_db, seeded->turn2_wave_id, 9106, now);
    const auto fallback_job_id = RecordBackfillTurnJob(analysis_db, seeded->turn2_wave_id, 9107, now);

    std::int64_t job_set_id = 0;
    ASSERT_TRUE(execution_db->CreateJobSet(
        {
            .program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),
            .purpose = "Backfill Test",
            .created_by = "test",
            .created_at_utc = now.time_since_epoch().count(),
            .expected_total = 6,
            .domain_ref_kind = "analysis_battle.turn_wave",
            .domain_ref_id = seeded->turn2_wave_id,
        },
        &job_set_id,
        &err)) << err;

    LinkBackfillExecJob(
        analysis_db,
        seeded->turn1_job_id,
        EnqueueBackfillExecJob(
            execution_db,
            job_set_id,
            seeded->turn1_job_id,
            1111,
            BattleSingleTurnIni(seeded->turn1_wave_id, 9101, seed_candidate_id, 1111, 1, "01000000000004ffff", "turn1-old", false),
            "turn1"));
    LinkBackfillExecJob(
        analysis_db,
        seeded->turn2_job_id,
        EnqueueBackfillExecJob(
            execution_db,
            job_set_id,
            seeded->turn2_job_id,
            2222,
            BattleSingleTurnIni(seeded->turn2_wave_id, 9102, seed_candidate_id, 2222, 2, "01000000000004ffff", "turn2-new", true),
            "turn2"));
    LinkBackfillExecJob(
        analysis_db,
        complete_job_id,
        EnqueueBackfillExecJob(
            execution_db,
            job_set_id,
            complete_job_id,
            3333,
            BattleSingleTurnIni(seeded->turn2_wave_id, 9103, seed_candidate_id, 3333, 2, "01000000000004ffff", "should-not-overwrite", true),
            "complete"));
    LinkBackfillExecJob(
        analysis_db,
        invalid_blob_job_id,
        EnqueueBackfillExecJob(
            execution_db,
            job_set_id,
            invalid_blob_job_id,
            4444,
            BattleSingleTurnIni(seeded->turn2_wave_id, 9104, seed_candidate_id, 4444, 2, "not-hex", "invalid-command-variant", true),
            "invalid-command"));
    LinkBackfillExecJob(
        analysis_db,
        invalid_ini_job_id,
        EnqueueBackfillExecJob(
            execution_db,
            job_set_id,
            invalid_ini_job_id,
            5555,
            "[Other]\nvalue=1\n",
            "invalid-ini"));
    LinkBackfillExecJob(
        analysis_db,
        fallback_job_id,
        EnqueueBackfillExecJob(
            execution_db,
            job_set_id,
            fallback_job_id,
            6666,
            BattleSingleTurnIniWithFallbacksOnly("01000000000004ffff"),
            "fallback"));

    RunProjectionUntilCaughtUp(service, "analysis-battle");
    service.Stop();
}

savor::debugtool::BackfillAnalysisBattleOptions BackfillOptionsFor(const std::filesystem::path& root, bool apply) {
    return savor::debugtool::BackfillAnalysisBattleOptions{
        .db_root = root,
        .migration_root = ResolveMigrationRootForTests(),
        .apply = apply,
    };
}

void SeedSecondTurnQueuedBattleJobs(SeededDebugDb* seeded) {
    using namespace savor::db;

    ASSERT_NE(seeded, nullptr);
    seeded->root = MakeDebugToolTempRoot("analysis-battle");
    seeded->paths = MakeDebugToolDbPaths(seeded->root);

    savor::db::core::DBService service(
        seeded->paths,
        savor::db::migrations::MigrationSourceOptions{ .source_kind = savor::db::migrations::MigrationSourceKind::Embedded });
    std::string err;
    EXPECT_TRUE(service.Start(&err)) << err;

    auto* analysis_db = service.AnalysisDb();
    EXPECT_NE(analysis_db, nullptr);
    const auto now = types::UtcTimePoint(std::chrono::milliseconds(1712304100000));

    ASSERT_TRUE(analysis_db->CreateBattleSet(
        {
            .name = "debug-tool-turn-two",
            .entry_savestate_id = 111,
            .battle_plan_id = 222,
            .battle_plan_fingerprint = "debug-tool-plan",
            .continuation_mode = BattleContinuationMode::AutomaticBestPerEndingRng,
            .status = BattleSetStatus::Active,
            .created_at_utc = now,
            .correlation_id = "debug-tool",
            .causation_id = "test",
        },
        &seeded->battle_set_id,
        &err)) << err;

    std::int64_t seed_candidate_id = 0;
    ASSERT_TRUE(analysis_db->AddBattleSeedCandidate(
        {
            .battle_set_id = seeded->battle_set_id,
            .seed_value = 777,
            .source_kind = BattleSeedCandidateSourceKind::Synthetic,
            .candidate_status = BattleSeedCandidateStatus::Ready,
            .created_at_utc = now,
            .correlation_id = "debug-tool",
            .causation_id = "test",
        },
        &seed_candidate_id,
        &err)) << err;

    ASSERT_TRUE(analysis_db->CreateBattleTurnWave(
        {
            .battle_set_id = seeded->battle_set_id,
            .turn_index = 2,
            .seed_candidate_id = seed_candidate_id,
            .status = BattleTurnWaveStatus::Running,
            .created_at_utc = now,
            .correlation_id = "debug-tool",
            .causation_id = "test",
        },
        &seeded->wave_id,
        &err)) << err;

    for (int index = 0; index < 4; ++index) {
        std::int64_t turn_job_id = 0;
        ASSERT_TRUE(analysis_db->RecordBattleTurnJob(
            {
                .wave_id = seeded->wave_id,
                .exec_job_id = 7201 + index,
                .plan_id = 9201 + index,
                .fake_attacks_this_turn = 1,
                .fake_attacks_used_before = 2,
                .job_state = BattleTurnJobState::Queued,
                .has_results = false,
                .recorded_at_utc = now,
                .correlation_id = "debug-tool",
                .causation_id = "test",
            },
            &turn_job_id,
            &err)) << err;
    }

    RunProjectionUntilCaughtUp(service, "analysis-battle");
    service.Stop();

    SqliteHandle analysis;
    ASSERT_TRUE(OpenSqlite(seeded->paths.analysis_db_path, &analysis));
    ASSERT_TRUE(ExecSqlWithError(
        analysis.db,
        "UPDATE ab_turn_job SET job_state='SUCCEEDED',has_results=1,battle_outcome=0,ended_at_utc=1712304100000,recorded_at_utc=1712304100000;"));
    ASSERT_TRUE(ExecSqlWithError(
        analysis.db,
        "UPDATE ab_turn_wave SET status='COMPLETED',completed_at_utc=1712304100000;"));
    ASSERT_TRUE(ExecSqlWithError(
        analysis.db,
        "UPDATE ab_battle_set SET status='VICTORY',completed_at_utc=1712304100000;"));
}

savor::debugtool::ReprojectUiReadOptions ToolOptionsFor(const std::filesystem::path& root, bool apply) {
    return savor::debugtool::ReprojectUiReadOptions{
        .db_root = root,
        .migration_root = ResolveMigrationRootForTests(),
        .apply = apply,
        .max_iterations = 20,
    };
}

} // namespace

TEST(SavorDbDebugTool, MissingDbRootFailsWithActionableError) {
    const auto root = MakeDebugToolTempRoot("missing");
    auto cleanup = std::unique_ptr<void, void (*)(void*)>(
        root.empty() ? nullptr : static_cast<void*>(&const_cast<std::filesystem::path&>(root)),
        [](void* ptr) {
            if (ptr != nullptr) {
                std::error_code ec;
                std::filesystem::remove_all(*static_cast<std::filesystem::path*>(ptr), ec);
            }
        });

    auto options = ToolOptionsFor(root, false);
    savor::debugtool::ReprojectUiReadPlan plan;
    std::string err;
    EXPECT_FALSE(savor::debugtool::BuildPlan(options, &plan, &err));
    EXPECT_NE(err.find("missing ui_read DB"), std::string::npos) << err;
}

TEST(SavorDbDebugTool, DryRunDoesNotMutateStaleUiReadRows) {
    SeededDebugDb seeded;
    SeedSecondTurnQueuedBattleJobs(&seeded);
    auto cleanup = std::unique_ptr<void, void (*)(void*)>(
        const_cast<std::filesystem::path*>(&seeded.root),
        [](void* ptr) {
            std::error_code ec;
            std::filesystem::remove_all(*static_cast<std::filesystem::path*>(ptr), ec);
        });

    SqliteHandle ui_before;
    ASSERT_TRUE(OpenSqlite(seeded.paths.ui_read_db_path, &ui_before));
    EXPECT_EQ(
        ReadInt64(ui_before.db, ("SELECT COUNT(*) FROM ui_battle_turn_job WHERE wave_id=" + std::to_string(seeded.wave_id) + " AND job_state='QUEUED';").c_str()),
        4);

    auto options = ToolOptionsFor(seeded.root, false);
    savor::debugtool::ReprojectUiReadPlan plan;
    std::string err;
    ASSERT_TRUE(savor::debugtool::BuildPlan(options, &plan, &err)) << err;
    ASSERT_EQ(plan.streams.size(), 5u);

    savor::debugtool::ReprojectUiReadResult result;
    ASSERT_TRUE(savor::debugtool::ExecutePlan(plan, &result, &err)) << err;
    EXPECT_FALSE(result.applied);
    EXPECT_TRUE(result.backup_path.empty());

    SqliteHandle ui_after;
    ASSERT_TRUE(OpenSqlite(seeded.paths.ui_read_db_path, &ui_after));
    EXPECT_EQ(
        ReadInt64(ui_after.db, ("SELECT COUNT(*) FROM ui_battle_turn_job WHERE wave_id=" + std::to_string(seeded.wave_id) + " AND job_state='QUEUED';").c_str()),
        4);
    EXPECT_EQ(ReadInt64(ui_after.db, "SELECT COUNT(*) FROM ui_projection_dirty_entity;"), 0);
}

TEST(SavorDbDebugTool, BackfillAnalysisBattleDryRunDoesNotMutateAnalysisRows) {
    BackfillSeededDebugDb seeded;
    SeedAnalysisBattleBackfillDb(&seeded);
    auto cleanup = std::unique_ptr<void, void (*)(void*)>(
        const_cast<std::filesystem::path*>(&seeded.root),
        [](void* ptr) {
            std::error_code ec;
            std::filesystem::remove_all(*static_cast<std::filesystem::path*>(ptr), ec);
        });

    SqliteHandle analysis_before;
    ASSERT_TRUE(OpenSqlite(seeded.paths.analysis_db_path, &analysis_before));
    EXPECT_EQ(
        ReadText(analysis_before.db, ("SELECT resolved_turn_variant_key FROM ab_turn_job WHERE turn_job_id=" + std::to_string(seeded.turn2_job_id) + ";").c_str()),
        "");

    auto options = BackfillOptionsFor(seeded.root, false);
    savor::debugtool::BackfillAnalysisBattlePlan plan;
    std::string err;
    ASSERT_TRUE(savor::debugtool::BuildBackfillAnalysisBattlePlan(options, &plan, &err)) << err;

    savor::debugtool::BackfillAnalysisBattleResult result;
    ASSERT_TRUE(savor::debugtool::ExecuteBackfillAnalysisBattlePlan(plan, &result, &err)) << err;
    EXPECT_FALSE(result.applied);
    EXPECT_EQ(result.candidate_rows, 6);
    EXPECT_EQ(result.rows_updated, 4);
    EXPECT_EQ(result.rows_already_complete, 1);
    EXPECT_EQ(result.rows_missing_exec_job, 1);
    EXPECT_EQ(result.rows_missing_or_invalid_ini, 1);
    EXPECT_EQ(result.rows_invalid_command_blob, 1);

    SqliteHandle analysis_after;
    ASSERT_TRUE(OpenSqlite(seeded.paths.analysis_db_path, &analysis_after));
    EXPECT_EQ(
        ReadText(analysis_after.db, ("SELECT resolved_turn_variant_key FROM ab_turn_job WHERE turn_job_id=" + std::to_string(seeded.turn2_job_id) + ";").c_str()),
        "");
}

TEST(SavorDbDebugTool, BackfillAnalysisBattleApplyRepairsMissingFactsFromJobIni) {
    BackfillSeededDebugDb seeded;
    SeedAnalysisBattleBackfillDb(&seeded);
    auto cleanup = std::unique_ptr<void, void (*)(void*)>(
        const_cast<std::filesystem::path*>(&seeded.root),
        [](void* ptr) {
            std::error_code ec;
            std::filesystem::remove_all(*static_cast<std::filesystem::path*>(ptr), ec);
        });

    auto options = BackfillOptionsFor(seeded.root, true);
    savor::debugtool::BackfillAnalysisBattlePlan plan;
    std::string err;
    ASSERT_TRUE(savor::debugtool::BuildBackfillAnalysisBattlePlan(options, &plan, &err)) << err;

    savor::debugtool::BackfillAnalysisBattleResult result;
    ASSERT_TRUE(savor::debugtool::ExecuteBackfillAnalysisBattlePlan(plan, &result, &err)) << err;
    EXPECT_TRUE(result.applied);
    EXPECT_FALSE(result.backup_path.empty());
    EXPECT_TRUE(std::filesystem::exists(result.backup_path));
    EXPECT_EQ(result.candidate_rows, 6);
    EXPECT_EQ(result.rows_updated, 4);
    EXPECT_EQ(result.rows_already_complete, 1);
    EXPECT_EQ(result.rows_missing_exec_job, 1);
    EXPECT_EQ(result.rows_missing_or_invalid_ini, 1);
    EXPECT_EQ(result.rows_invalid_command_blob, 1);

    SqliteHandle analysis;
    ASSERT_TRUE(OpenSqlite(seeded.paths.analysis_db_path, &analysis));
    EXPECT_EQ(ReadInt64(analysis.db, ("SELECT source_savestate_id FROM ab_turn_job WHERE turn_job_id=" + std::to_string(seeded.turn1_job_id) + ";").c_str()), 1111);
    EXPECT_EQ(ReadText(analysis.db, ("SELECT resolved_turn_variant_key FROM ab_turn_job WHERE turn_job_id=" + std::to_string(seeded.turn1_job_id) + ";").c_str()), "turn1-old");
    EXPECT_EQ(ReadInt64(analysis.db, ("SELECT source_savestate_id FROM ab_turn_job WHERE turn_job_id=" + std::to_string(seeded.turn2_job_id) + ";").c_str()), 2222);
    EXPECT_EQ(ReadText(analysis.db, ("SELECT resolved_turn_variant_key FROM ab_turn_job WHERE turn_job_id=" + std::to_string(seeded.turn2_job_id) + ";").c_str()), "turn2-new");
    EXPECT_EQ(ReadInt64(analysis.db, "SELECT COUNT(1) FROM ab_turn_job WHERE source_savestate_id=9999 AND seed_candidate_id=9998 AND authored_plan_id=9997 AND authored_turn_index=9 AND resolved_turn_variant_key='preserve-variant';"), 1);
    EXPECT_EQ(ReadInt64(analysis.db, "SELECT COUNT(1) FROM ab_turn_job WHERE source_savestate_id=6666 AND seed_candidate_id IS NOT NULL AND authored_plan_id=9107 AND authored_turn_index=2 AND resolved_turn_variant_key IS NOT NULL;"), 1);
    EXPECT_EQ(ReadInt64(analysis.db, "SELECT COUNT(1) FROM ab_turn_job WHERE source_savestate_id=4444 AND resolved_turn_commands_blob IS NULL AND resolved_turn_variant_key='invalid-command-variant';"), 1);
}

TEST(SavorDbDebugTool, BackfillAnalysisBattleThenReprojectUiReadRepairsReplicationRows) {
    BackfillSeededDebugDb seeded;
    SeedAnalysisBattleBackfillDb(&seeded);
    auto cleanup = std::unique_ptr<void, void (*)(void*)>(
        const_cast<std::filesystem::path*>(&seeded.root),
        [](void* ptr) {
            std::error_code ec;
            std::filesystem::remove_all(*static_cast<std::filesystem::path*>(ptr), ec);
        });

    auto backfill_options = BackfillOptionsFor(seeded.root, true);
    savor::debugtool::BackfillAnalysisBattlePlan backfill_plan;
    std::string err;
    ASSERT_TRUE(savor::debugtool::BuildBackfillAnalysisBattlePlan(backfill_options, &backfill_plan, &err)) << err;
    savor::debugtool::BackfillAnalysisBattleResult backfill_result;
    ASSERT_TRUE(savor::debugtool::ExecuteBackfillAnalysisBattlePlan(backfill_plan, &backfill_result, &err)) << err;

    auto reproject_options = ToolOptionsFor(seeded.root, true);
    reproject_options.stream_ids = { "analysis-battle" };
    savor::debugtool::ReprojectUiReadPlan reproject_plan;
    ASSERT_TRUE(savor::debugtool::BuildPlan(reproject_options, &reproject_plan, &err)) << err;
    savor::debugtool::ReprojectUiReadResult reproject_result;
    ASSERT_TRUE(savor::debugtool::ExecutePlan(reproject_plan, &reproject_result, &err)) << err;

    SqliteHandle ui;
    ASSERT_TRUE(OpenSqlite(seeded.paths.ui_read_db_path, &ui));
    EXPECT_EQ(ReadInt64(ui.db, ("SELECT source_savestate_id FROM ui_battle_turn_job_replication WHERE turn_job_id=" + std::to_string(seeded.turn1_job_id) + ";").c_str()), 1111);
    EXPECT_EQ(ReadText(ui.db, ("SELECT resolved_turn_variant_key FROM ui_battle_turn_job_replication WHERE turn_job_id=" + std::to_string(seeded.turn2_job_id) + ";").c_str()), "turn2-new");
    EXPECT_EQ(ReadInt64(ui.db, ("SELECT parent_turn_job_id FROM ui_battle_turn_job_replication WHERE turn_job_id=" + std::to_string(seeded.turn2_job_id) + ";").c_str()), seeded.turn1_job_id);
    EXPECT_EQ(ReadText(ui.db, ("SELECT resolved_turn_commands_blob FROM ui_battle_turn_job_replication WHERE turn_job_id=" + std::to_string(seeded.turn2_job_id) + ";").c_str()), "01000000000004ffff");
}

TEST(SavorDbDebugTool, ApplyRepairsAllStreamsAndObservedSecondTurnBattleRows) {
    SeededDebugDb seeded;
    SeedSecondTurnQueuedBattleJobs(&seeded);
    auto cleanup = std::unique_ptr<void, void (*)(void*)>(
        const_cast<std::filesystem::path*>(&seeded.root),
        [](void* ptr) {
            std::error_code ec;
            std::filesystem::remove_all(*static_cast<std::filesystem::path*>(ptr), ec);
        });

    auto options = ToolOptionsFor(seeded.root, true);
    savor::debugtool::ReprojectUiReadPlan plan;
    std::string err;
    ASSERT_TRUE(savor::debugtool::BuildPlan(options, &plan, &err)) << err;

    const auto battle_stream = std::find_if(plan.streams.begin(), plan.streams.end(), [](const auto& stream) {
        return stream.stream_id == "analysis-battle";
    });
    ASSERT_NE(battle_stream, plan.streams.end());
    EXPECT_EQ(battle_stream->expected_dirty_rows, 1);

    savor::debugtool::ReprojectUiReadResult result;
    ASSERT_TRUE(savor::debugtool::ExecutePlan(plan, &result, &err)) << err;
    EXPECT_TRUE(result.applied);
    EXPECT_FALSE(result.backup_path.empty());
    EXPECT_TRUE(std::filesystem::exists(result.backup_path));

    SqliteHandle ui;
    ASSERT_TRUE(OpenSqlite(seeded.paths.ui_read_db_path, &ui));
    EXPECT_EQ(
        ReadInt64(ui.db, ("SELECT COUNT(*) FROM ui_battle_turn_job WHERE wave_id=" + std::to_string(seeded.wave_id) + " AND job_state='SUCCEEDED';").c_str()),
        4);
    EXPECT_EQ(
        ReadInt64(ui.db, ("SELECT COUNT(*) FROM ui_battle_turn_job WHERE wave_id=" + std::to_string(seeded.wave_id) + " AND job_state='QUEUED';").c_str()),
        0);
    EXPECT_EQ(ReadText(ui.db, ("SELECT status FROM ui_battle_wave WHERE wave_id=" + std::to_string(seeded.wave_id) + ";").c_str()), "COMPLETED");
    EXPECT_EQ(ReadText(ui.db, ("SELECT status FROM ui_battle_group WHERE battle_set_id=" + std::to_string(seeded.battle_set_id) + ";").c_str()), "VICTORY");
    EXPECT_EQ(ReadInt64(ui.db, "SELECT COUNT(*) FROM ui_projection_dirty_entity;"), 0);
    EXPECT_EQ(ReadInt64(ui.db, "SELECT COUNT(*) FROM ui_projection_subscription;"), 5);
}
