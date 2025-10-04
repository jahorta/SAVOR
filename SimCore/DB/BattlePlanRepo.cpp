// SimCore/DB/BattlePlanRepo.cpp
#include "BattlePlanRepo.h"
#include <sqlite3.h>

namespace simcore::db {

    static DbResult<std::optional<int64_t>> impl_find_by_fp(DbEnv& env, const std::string& fp) {
        auto* db = env.handle();
        sqlite3_stmt* st{};
        if (sqlite3_prepare_v2(db, "SELECT plan_id FROM battle_plan WHERE fingerprint=? LIMIT 1;", -1, &st, nullptr) != SQLITE_OK)
            return DbResult<std::optional<int64_t>>::Err({map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        sqlite3_bind_text(st, 1, fp.c_str(), -1, SQLITE_TRANSIENT);
        std::optional<int64_t> out{};
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) out = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return DbResult<std::optional<int64_t>>::Ok(out);
    }

    static DbResult<int64_t> impl_ensure(DbEnv& env, const std::string& name, const std::string& fingerprint, int32_t num_turns) {
        auto f = impl_find_by_fp(env, fingerprint);
        if (!f.ok) return DbResult<int64_t>::Err(f.error);
        if (f.value) return DbResult<int64_t>::Ok(*f.value);

        auto* db = env.handle();
        sqlite3_stmt* st{};
        if (sqlite3_prepare_v2(db,
            "INSERT INTO battle_plan(name,fingerprint,num_turns,created_at) "
            "VALUES(?,?,?,strftime('%s','now')) RETURNING plan_id;", -1, &st, nullptr) != SQLITE_OK)
            return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });

        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, num_turns);
        int64_t id{};
        if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        if (!id) return DbResult<int64_t>::Err({ DbErrorKind::IO, sqlite3_errcode(db), "insert battle_plan" });
        return DbResult<int64_t>::Ok(id);
    }

    static DbResult<BattlePlanRow> impl_get(DbEnv& env, int64_t plan_id) {
        auto* db = env.handle();
        sqlite3_stmt* st{};
        if (sqlite3_prepare_v2(db,
            "SELECT plan_id,settings_id,name,fingerprint,num_turns,created_at FROM battle_plan WHERE plan_id=?;", -1, &st, nullptr) != SQLITE_OK)
            return DbResult<BattlePlanRow>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        sqlite3_bind_int64(st, 1, plan_id);
        BattlePlanRow r;
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            r.plan_id = sqlite3_column_int64(st, 0);
            if (sqlite3_column_type(st, 1) != SQLITE_NULL) r.name = (const char*)sqlite3_column_text(st, 1);
            if (sqlite3_column_type(st, 2) != SQLITE_NULL) r.fingerprint = (const char*)sqlite3_column_text(st, 2);
            r.num_turns = sqlite3_column_int(st, 3);
            r.created_at = sqlite3_column_int64(st, 4);
            sqlite3_finalize(st);
            return DbResult<BattlePlanRow>::Ok(std::move(r));
        }
        sqlite3_finalize(st);
        return DbResult<BattlePlanRow>::Err({ DbErrorKind::NotFound, sqlite3_errcode(db), "plan not found" });
    }

    static DbResult<void> impl_delete(DbEnv& env, int64_t plan_id) {
        auto* db = env.handle();
        sqlite3_stmt* st{};
        if (sqlite3_prepare_v2(db, "DELETE FROM battle_plan WHERE plan_id=?;", -1, &st, nullptr) != SQLITE_OK)
            return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        sqlite3_bind_int64(st, 1, plan_id);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        return DbResult<void>::Ok();
    }

    std::future<DbResult<int64_t>> BattlePlanRepo::EnsureAsync(std::string name, std::string fingerprint, int32_t num_turns, RetryPolicy rp) {
        return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_ensure(e, name, fingerprint, num_turns); });
    }
    std::future<DbResult<std::optional<int64_t>>> BattlePlanRepo::FindByFingerprintAsync(const std::string& fingerprint, RetryPolicy rp) {
        return DBService::instance().submit_res<std::optional<int64_t>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_find_by_fp(e, fingerprint); });
    }
    std::future<DbResult<BattlePlanRow>> BattlePlanRepo::GetAsync(int64_t plan_id, RetryPolicy rp) {
        return DBService::instance().submit_res<BattlePlanRow>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_get(e, plan_id); });
    }
    std::future<DbResult<void>> BattlePlanRepo::DeleteAsync(int64_t plan_id, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_delete(e, plan_id); });
    }

} // namespace simcore::db
