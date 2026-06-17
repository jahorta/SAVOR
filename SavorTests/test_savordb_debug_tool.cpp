#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "ReprojectUiRead.h"

#include "Analysis/IAnalysisDb.h"
#include "Common/DbService.h"
#include "Common/Migrations/MigrationRunner.h"
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
            .battle_run_spec_id = 222,
            .explorer_settings_id = 333,
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
