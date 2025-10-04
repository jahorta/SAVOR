#include "BattleContextRepo.h"
#include <sqlite3.h>

namespace simcore {
    namespace db {

        static DbResult<int64_t> Impl_Insert(DbEnv& env, int64_t job_set_id, int64_t job_id, int64_t artifact_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            const char* sql =
                "INSERT INTO battle_contexts(job_set_id,job_id,artifact_id,created_at)"
                " VALUES(?,?,?,strftime('%s','now')) RETURNING context_id;";
            if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
                return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
            sqlite3_bind_int64(st, 1, job_set_id);
            sqlite3_bind_int64(st, 2, job_id);
            sqlite3_bind_int64(st, 3, artifact_id);
            int rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), rc, "insert" }); }
            int64_t id = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
            return DbResult<int64_t>::Ok(id);
        }

        static DbResult<std::optional<BattleContextRow>> Impl_GetByJob(DbEnv& env, int64_t job_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            const char* sql =
                "SELECT context_id,job_set_id,job_id,artifact_id,created_at FROM battle_contexts WHERE job_id=? LIMIT 1;";
            if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
                return DbResult<std::optional<BattleContextRow>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
            sqlite3_bind_int64(st, 1, job_id);
            int rc = sqlite3_step(st);
            if (rc == SQLITE_DONE) { sqlite3_finalize(st); return DbResult<std::optional<BattleContextRow>>::Ok(std::nullopt); }
            if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DbResult<std::optional<BattleContextRow>>::Err({ map_sqlite_err(sqlite3_errcode(db)), rc, "select" }); }
            BattleContextRow row{};
            row.context_id = sqlite3_column_int64(st, 0);
            row.job_set_id = sqlite3_column_int64(st, 1);
            row.job_id = sqlite3_column_int64(st, 2);
            row.artifact_id = sqlite3_column_int64(st, 3);
            row.created_at = sqlite3_column_int64(st, 4);
            sqlite3_finalize(st);
            return DbResult<std::optional<BattleContextRow>>::Ok(row);
        }

        static DbResult<std::vector<BattleContextRow>> Impl_ListByJobSet(DbEnv& env, int64_t job_set_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            const char* sql =
                "SELECT context_id,job_set_id,job_id,artifact_id,created_at FROM battle_contexts WHERE job_set_id=? ORDER BY context_id;";
            if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
                return DbResult<std::vector<BattleContextRow>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
            sqlite3_bind_int64(st, 1, job_set_id);
            std::vector<BattleContextRow> out;
            int rc = SQLITE_ROW;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                BattleContextRow row{};
                row.context_id = sqlite3_column_int64(st, 0);
                row.job_set_id = sqlite3_column_int64(st, 1);
                row.job_id = sqlite3_column_int64(st, 2);
                row.artifact_id = sqlite3_column_int64(st, 3);
                row.created_at = sqlite3_column_int64(st, 4);
                out.push_back(row);
            }
            if (rc != SQLITE_DONE) { sqlite3_finalize(st); return DbResult<std::vector<BattleContextRow>>::Err({ map_sqlite_err(sqlite3_errcode(db)), rc, "select" }); }
            sqlite3_finalize(st);
            return DbResult<std::vector<BattleContextRow>>::Ok(std::move(out));
        }

        std::future<DbResult<int64_t>> BattleContextRepo::InsertAsync(int64_t job_set_id, int64_t job_id, int64_t artifact_id, RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp, 
                [=](DbEnv& env) { return Impl_Insert(env, job_set_id, job_id, artifact_id); });
        }
        std::future<DbResult<std::optional<BattleContextRow>>> BattleContextRepo::GetByJobAsync(int64_t job_id, RetryPolicy rp) {
            return DBService::instance().submit_res<std::optional<BattleContextRow>>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& env) { return Impl_GetByJob(env, job_id); });
        }
        std::future<DbResult<std::vector<BattleContextRow>>> BattleContextRepo::ListByJobSetAsync(int64_t job_set_id, RetryPolicy rp) {
            return DBService::instance().submit_res<std::vector<BattleContextRow>>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& env) { return Impl_ListByJobSet(env, job_set_id); });
        }

    }
} // namespace
