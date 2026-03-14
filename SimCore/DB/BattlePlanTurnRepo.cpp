#include "BattlePlanTurnRepo.h"
#include <sqlite3.h>

namespace simcore {
    namespace db {

        static inline DbResult<void> Impl_UpsertTurnByPlan(DbEnv& env, int64_t plan_id, int32_t turn_index) {
            auto* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "INSERT INTO battle_plan_turn(plan_id,turn_index) VALUES(?,?) "
                "ON CONFLICT(plan_id,turn_index) DO NOTHING;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
            sqlite3_bind_int64(st, 1, plan_id);
            sqlite3_bind_int(st, 2, turn_index);
            rc = sqlite3_step(st); sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
            return DbResult<void>::Ok();
        }

        static inline DbResult<void> Impl_UpsertTurnActorByPlan(DbEnv& env, int64_t plan_id, int32_t turn_index, int32_t actor_index, int64_t atom_id) {
            auto* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "INSERT INTO battle_plan_turn_actor(plan_id,turn_index,actor_index,atom_id) VALUES(?,?,?,?) "
                "ON CONFLICT(plan_id,turn_index,actor_index) DO UPDATE SET atom_id=excluded.atom_id;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
            sqlite3_bind_int64(st, 1, plan_id);
            sqlite3_bind_int(st, 2, turn_index);
            sqlite3_bind_int(st, 3, actor_index);
            sqlite3_bind_int64(st, 4, atom_id);
            rc = sqlite3_step(st); sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
            return DbResult<void>::Ok();
        }

        static inline DbResult<void> Impl_ReplaceTurnByPlan(DbEnv& env, int64_t plan_id, int32_t turn_index, const std::vector<TurnActorBindingByPlan>& actors) {
            auto a = Impl_UpsertTurnByPlan(env, plan_id, turn_index);
            if (!a.ok) return a;
            for (auto& b : actors) {
                auto r = Impl_UpsertTurnActorByPlan(env, plan_id, turn_index, b.actor_index, b.atom_id);
                if (!r.ok) return r;
            }
            return DbResult<void>::Ok();
        }

        static inline DbResult<std::vector<BattlePlanTurnByPlanRow>> Impl_LoadTurnsByPlan(DbEnv& env, int64_t plan_id) {
            auto* db = env.handle();
            sqlite3_stmt* ts{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT turn_index FROM battle_plan_turn WHERE plan_id=? ORDER BY turn_index;", -1, &ts, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::vector<BattlePlanTurnByPlanRow>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
            sqlite3_bind_int64(ts, 1, plan_id);
            std::vector<BattlePlanTurnByPlanRow> rows;
            while ((rc = sqlite3_step(ts)) == SQLITE_ROW) {
                BattlePlanTurnByPlanRow r;
                r.plan_id = plan_id;
                r.turn_index = sqlite3_column_int(ts, 0);
                rows.push_back(std::move(r));
            }
            sqlite3_finalize(ts);
            if (rc != SQLITE_DONE) return DbResult<std::vector<BattlePlanTurnByPlanRow>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
            return DbResult<std::vector<BattlePlanTurnByPlanRow>>::Ok(std::move(rows));
        }

        static inline DbResult<std::vector<TurnActorBindingByPlan>> Impl_ListActorsByPlan(DbEnv& env, int64_t plan_id, int32_t turn_index) {
            auto* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT actor_index,atom_id FROM battle_plan_turn_actor WHERE plan_id=? AND turn_index=? ORDER BY actor_index;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::vector<TurnActorBindingByPlan>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
            sqlite3_bind_int64(st, 1, plan_id);
            sqlite3_bind_int(st, 2, turn_index);
            std::vector<TurnActorBindingByPlan> v;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                TurnActorBindingByPlan r;
                r.actor_index = sqlite3_column_int(st, 0);
                r.atom_id = sqlite3_column_int64(st, 1);
                v.push_back(std::move(r));
            }
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<std::vector<TurnActorBindingByPlan>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
            return DbResult<std::vector<TurnActorBindingByPlan>>::Ok(std::move(v));
        }

        std::future<DbResult<void>> BattlePlanTurnRepo::UpsertTurnByPlanAsync(int64_t plan_id, int32_t turn_index, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_UpsertTurnByPlan(e, plan_id, turn_index); });
        }
        std::future<DbResult<void>> BattlePlanTurnRepo::UpsertTurnActorByPlanAsync(int64_t plan_id, int32_t turn_index, int32_t actor_index, int64_t atom_id, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_UpsertTurnActorByPlan(e, plan_id, turn_index, actor_index, atom_id); });
        }
        std::future<DbResult<void>> BattlePlanTurnRepo::ReplaceTurnByPlanAsync(int64_t plan_id, int32_t turn_index, std::vector<TurnActorBindingByPlan> actors, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ReplaceTurnByPlan(e, plan_id, turn_index, actors); });
        }
        std::future<DbResult<std::vector<BattlePlanTurnByPlanRow>>> BattlePlanTurnRepo::LoadTurnsByPlanAsync(int64_t plan_id, RetryPolicy rp) {
            return DBService::instance().submit_res<std::vector<BattlePlanTurnByPlanRow>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_LoadTurnsByPlan(e, plan_id); });
        }
        std::future<DbResult<std::vector<TurnActorBindingByPlan>>> BattlePlanTurnRepo::ListActorsByPlanAsync(int64_t plan_id, int32_t turn_index, RetryPolicy rp) {
            return DBService::instance().submit_res<std::vector<TurnActorBindingByPlan>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ListActorsByPlan(e, plan_id, turn_index); });
        }

    } // namespace db
} // namespace simcore