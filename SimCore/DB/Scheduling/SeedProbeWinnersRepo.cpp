// SimCore/DB/SeedProbeWinnersRepo.cpp
#include "SeedProbeWinnersRepo.h"
#include <sqlite3.h>

namespace simcore {
    namespace db {

        static inline DbResult<InsertWinnerResult> Impl_TryInsertWinner(
            DbEnv& env,
            int64_t job_set_id,
            int32_t result_delta,
            int64_t winner_job_id,
            std::optional<int32_t> expected_delta,
            std::optional<std::string> expected_tag,
            std::optional<std::string> stage,
            std::optional<std::string> frame_hex,
            std::optional<std::string> metrics_json
        ) {
            sqlite3* db = env.handle();

            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "INSERT INTO seed_probe_winners(job_set_id, result_delta, winner_job_id, expected_delta, expected_tag, stage, frame_hex, metrics_json) "
                "VALUES(?, ?, ?, ?, ?, ?, ?, ?);",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<InsertWinnerResult>::Err({ map_sqlite_err(rc), rc, "prepare insert" });

            sqlite3_bind_int64(st, 1, job_set_id);
            sqlite3_bind_int(st, 2, result_delta);
            sqlite3_bind_int64(st, 3, winner_job_id);
            if (expected_delta) sqlite3_bind_int(st, 4, *expected_delta); else sqlite3_bind_null(st, 4);
            if (expected_tag)   sqlite3_bind_text(st, 5, expected_tag->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 5);
            if (stage)          sqlite3_bind_text(st, 6, stage->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 6);
            if (frame_hex)      sqlite3_bind_text(st, 7, frame_hex->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 7);
            if (metrics_json)   sqlite3_bind_text(st, 8, metrics_json->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 8);

            rc = sqlite3_step(st);
            sqlite3_finalize(st);

            if (rc == SQLITE_DONE) {
                return DbResult<InsertWinnerResult>::Ok(InsertWinnerResult{ true, std::nullopt });
            }
            if (rc == SQLITE_CONSTRAINT || rc == SQLITE_CONSTRAINT_PRIMARYKEY || rc == SQLITE_CONSTRAINT_UNIQUE) {
                sqlite3_stmt* st2{};
                rc = sqlite3_prepare_v2(db,
                    "SELECT winner_job_id FROM seed_probe_winners WHERE job_set_id=? AND result_delta=?;",
                    -1, &st2, nullptr);
                if (rc != SQLITE_OK) return DbResult<InsertWinnerResult>::Err({ map_sqlite_err(rc), rc, "prepare select existing" });
                sqlite3_bind_int64(st2, 1, job_set_id);
                sqlite3_bind_int(st2, 2, result_delta);
                int rc2 = sqlite3_step(st2);
                if (rc2 != SQLITE_ROW) { sqlite3_finalize(st2); return DbResult<InsertWinnerResult>::Err({ map_sqlite_err(rc2), rc2, "select existing" }); }
                int64_t existing = sqlite3_column_int64(st2, 0);
                sqlite3_finalize(st2);
                return DbResult<InsertWinnerResult>::Ok(InsertWinnerResult{ false, existing });
            }
            return DbResult<InsertWinnerResult>::Err({ map_sqlite_err(rc), rc, "insert" });
        }

        static inline DbResult<bool> Impl_Exists(DbEnv& env, int64_t job_set_id, int32_t result_delta) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT 1 FROM seed_probe_winners WHERE job_set_id=? AND result_delta=? LIMIT 1;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<bool>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, job_set_id);
            sqlite3_bind_int(st, 2, result_delta);
            rc = sqlite3_step(st);
            bool exists = (rc == SQLITE_ROW);
            sqlite3_finalize(st);
            if (rc != SQLITE_ROW && rc != SQLITE_DONE) return DbResult<bool>::Err({ map_sqlite_err(rc), rc, "select" });
            return DbResult<bool>::Ok(exists);
        }

        static inline DbResult<int64_t> Impl_CountByJobSet(DbEnv& env, int64_t job_set_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT COUNT(*) FROM seed_probe_winners WHERE job_set_id=?;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, job_set_id);
            rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "select" }); }
            int64_t count = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
            return DbResult<int64_t>::Ok(count);
        }

        std::future<DbResult<InsertWinnerResult>> SeedProbeWinnersRepo::TryInsertWinnerAsync(
            int64_t job_set_id,
            int32_t result_delta,
            int64_t winner_job_id,
            std::optional<int32_t> expected_delta,
            std::optional<std::string> expected_tag,
            std::optional<std::string> stage,
            std::optional<std::string> frame_hex,
            std::optional<std::string> metrics_json,
            RetryPolicy rp
        ) {
            return DBService::instance().submit_res<InsertWinnerResult>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_TryInsertWinner(e, job_set_id, result_delta, winner_job_id, expected_delta, expected_tag, stage, frame_hex, metrics_json); });
        }

        std::future<DbResult<bool>> SeedProbeWinnersRepo::ExistsAsync(int64_t job_set_id, int32_t result_delta, RetryPolicy rp) {
            return DBService::instance().submit_res<bool>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Exists(e, job_set_id, result_delta); });
        }

        std::future<DbResult<int64_t>> SeedProbeWinnersRepo::CountByJobSetAsync(int64_t job_set_id, RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_CountByJobSet(e, job_set_id); });
        }

        static DbResult<int64_t> impl_delete_by_job_set(DbEnv& env, int64_t job_set_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st = nullptr;
            const char* sql = "DELETE FROM seed_probe_winners WHERE job_set_id=?";
            if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
                return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
            }
            sqlite3_bind_int64(st, 1, job_set_id);
            int rc = sqlite3_step(st);
            int changes = sqlite3_changes(db);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) {
                return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "delete seed_probe_winners failed" });
            }
            return DbResult<int64_t>::Ok(static_cast<int64_t>(changes));
        }

        std::future<DbResult<int64_t>> SeedProbeWinnersRepo::DeleteByJobSetAsync(int64_t job_set_id, RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return impl_delete_by_job_set(e, job_set_id); });
        }

    } // namespace db
} // namespace simcore
