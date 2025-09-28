#include "BattlePlanAtomRepo.h"
#include <sqlite3.h>

namespace simcore {
    namespace db {

        static inline DbResult<int64_t> Impl_Ensure(DbEnv& env,
            int32_t action_type,
            int32_t actor_slot,
            int32_t param_item_id,
            int32_t target_slot) {
            sqlite3* db = env.handle();

            {
                sqlite3_stmt* st = nullptr;
                const char* sql = "SELECT id FROM battle_plan_atom WHERE action_type=? AND actor_slot=? AND param_item_id=? AND target_slot=?;";
                if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
                    return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
                sqlite3_bind_int(st, 1, action_type);
                sqlite3_bind_int(st, 2, actor_slot);
                sqlite3_bind_int(st, 3, param_item_id);
                sqlite3_bind_int(st, 4, target_slot);
                int step = sqlite3_step(st);
                if (step == SQLITE_ROW) {
                    int64_t id = sqlite3_column_int64(st, 0);
                    sqlite3_finalize(st);
                    return DbResult<int64_t>::Ok(id);
                }
                sqlite3_finalize(st);
                if (step != SQLITE_DONE) return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "SELECT failed" });
            }

            {
                sqlite3_stmt* st = nullptr;
                const char* sql = "INSERT INTO battle_plan_atom(action_type,actor_slot,param_item_id,target_slot) VALUES (?,?,?,?);";
                if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
                    return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
                sqlite3_bind_int(st, 1, action_type);
                sqlite3_bind_int(st, 2, actor_slot);
                sqlite3_bind_int(st, 3, param_item_id);
                sqlite3_bind_int(st, 4, target_slot);
                if (sqlite3_step(st) != SQLITE_DONE) {
                    const char* emsg = sqlite3_errmsg(db);
                    sqlite3_finalize(st);
                    return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), emsg });
                }
                sqlite3_finalize(st);
                return DbResult<int64_t>::Ok(sqlite3_last_insert_rowid(db));
            }
        }

        static inline DbResult<BattlePlanAtomRow> Impl_Get(DbEnv& env, int64_t id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id, action_type, actor_slot, param_item_id, target_slot "
                "FROM battle_plan_atom WHERE id=?;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<BattlePlanAtomRow>::Err({ map_sqlite_err(rc), rc, "prepare" });

            sqlite3_bind_int64(st, 1, id);
            rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DbResult<BattlePlanAtomRow>::Err({ DbErrorKind::NotFound, rc, "not found" }); }

            BattlePlanAtomRow r;
            r.id = sqlite3_column_int64(st, 0);
            r.action_type = sqlite3_column_int(st, 1);
            r.actor_slot = sqlite3_column_int(st, 2);
            r.param_item_id = sqlite3_column_int(st, 3);
            r.target_slot = sqlite3_column_int(st, 4);
            sqlite3_finalize(st);

            return DbResult<BattlePlanAtomRow>::Ok(std::move(r));
        }

        // Async

        std::future<DbResult<int64_t>> BattlePlanAtomRepo::EnsureAsync(
            int32_t action_type,
            int32_t actor_slot,
            int32_t param_item_id,
            int32_t target_slot,
            RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) {
                    return Impl_Ensure(e, action_type, actor_slot, param_item_id, target_slot);
                });
        }

        std::future<DbResult<BattlePlanAtomRow>> BattlePlanAtomRepo::GetAsync(int64_t id, RetryPolicy rp) {
            return DBService::instance().submit_res<BattlePlanAtomRow>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Get(e, id); });
        }

    } // namespace db
} // namespace simcore
