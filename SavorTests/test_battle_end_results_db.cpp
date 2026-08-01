#include <string>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "Analysis/SqliteAnalysisDb.h"
#include "State/SqliteStateDb.h"

namespace {

class ScopedSqliteDb {
public:
    ScopedSqliteDb() {
        EXPECT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
    }

    ~ScopedSqliteDb() {
        if (db_ != nullptr) {
            sqlite3_close(db_);
        }
    }

    sqlite3* get() const {
        return db_;
    }

    void Exec(const char* sql) const {
        char* error = nullptr;
        const auto rc = sqlite3_exec(
            db_,
            sql,
            nullptr,
            nullptr,
            &error);
        const std::string message =
            error == nullptr ? "" : error;
        if (error != nullptr) {
            sqlite3_free(error);
        }
        ASSERT_EQ(rc, SQLITE_OK) << message;
    }

private:
    sqlite3* db_ = nullptr;
};

TEST(
    BattleEndResultsDb,
    AnalysisLookupReturnsEveryJobWithTheChosenOutputSavestate) {
    ScopedSqliteDb db;
    db.Exec(
        "CREATE TABLE ab_turn_job("
        "turn_job_id INTEGER,wave_id INTEGER,exec_job_id INTEGER,"
        "plan_id INTEGER,source_savestate_id INTEGER,"
        "seed_candidate_id INTEGER,authored_plan_id INTEGER,"
        "authored_turn_index INTEGER,resolved_turn_commands_blob TEXT,"
        "resolved_turn_variant_key TEXT,fake_attacks_this_turn INTEGER,"
        "fake_attacks_used_before INTEGER,job_state TEXT,"
        "started_at_utc INTEGER,ended_at_utc INTEGER,"
        "has_results INTEGER,vi_start INTEGER,vi_end INTEGER,"
        "delta_vi INTEGER,rng_seed INTEGER,battle_outcome INTEGER,"
        "plan_materialize_err INTEGER,pred_passed INTEGER,"
        "pred_total INTEGER,pred_abort_run INTEGER,"
        "output_savestate_id INTEGER,applied_input_artifact_id INTEGER,"
        "input_trace_artifact_id INTEGER,"
        "result_context_blob_base64 TEXT,"
        "result_context_version INTEGER,recorded_at_utc INTEGER);"
        "INSERT INTO ab_turn_job VALUES"
        "(1,10,100,20,30,40,50,1,'commands','variant',"
        "0,0,'SUCCEEDED',1,2,1,3,4,1,5,2,0,1,1,0,77,"
        "NULL,NULL,NULL,NULL,2),"
        "(2,11,101,21,31,41,51,1,'commands2','variant2',"
        "0,0,'SUCCEEDED',1,2,1,3,4,1,5,2,0,1,1,0,77,"
        "NULL,NULL,NULL,NULL,2),"
        "(3,12,102,22,32,42,52,1,'commands3','variant3',"
        "0,0,'SUCCEEDED',1,2,1,3,4,1,5,2,0,1,1,0,88,"
        "NULL,NULL,NULL,NULL,2);");

    savor::db::analysis::SqliteAnalysisDb analysis(db.get());
    const auto matches =
        analysis.ListBattleTurnJobsByOutputSavestateId(77);
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0].turn_job_id, 1);
    EXPECT_EQ(matches[1].turn_job_id, 2);
    EXPECT_EQ(
        matches[0].job_state,
        savor::db::BattleTurnJobState::Succeeded);
    EXPECT_EQ(matches[0].output_savestate_id, 77);
    EXPECT_TRUE(
        analysis.ListBattleTurnJobsByOutputSavestateId(0).empty());
}

TEST(
    BattleEndResultsDb,
    StateLookupIncludesCompletionAndFrozenArtifactIdentity) {
    ScopedSqliteDb db;
    db.Exec(
        "CREATE TABLE state_artifact("
        "artifact_id INTEGER,sha256 TEXT,size_bytes INTEGER,"
        "filename TEXT,file_ext TEXT,artifact_kind TEXT);"
        "CREATE TABLE state_savestate("
        "savestate_id INTEGER,artifact_id INTEGER,"
        "savestate_type TEXT,note TEXT,is_complete INTEGER,"
        "created_at_utc INTEGER);"
        "INSERT INTO state_artifact VALUES("
        "5,'abc123',1234,'C:/state/victory.sav','.sav','SAV');"
        "INSERT INTO state_savestate VALUES("
        "9,5,'BATTLE','Victory',1,1000);");

    savor::db::state::SqliteStateDb state(db.get());
    const auto row = state.GetSavestate(9);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->savestate_id, 9);
    EXPECT_EQ(row->artifact_id, 5);
    EXPECT_TRUE(row->is_complete);
    EXPECT_EQ(row->artifact_sha256, "abc123");
    EXPECT_EQ(row->artifact_size_bytes, 1234);
    EXPECT_EQ(row->artifact_filename, "C:/state/victory.sav");
    EXPECT_EQ(row->artifact_kind, "SAV");
    EXPECT_FALSE(state.GetSavestate(10).has_value());
}

} // namespace
