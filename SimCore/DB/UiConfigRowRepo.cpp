#include "UiConfigRowRepo.h"
#include <sqlite3.h>
#include <unordered_map>

namespace simcore::db {

    static inline DbResult<void> begin_immediate(sqlite3* db) {
        sqlite3_stmt* st{};
        int rc = sqlite3_prepare_v2(db, "BEGIN IMMEDIATE;", -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), rc, sqlite3_errmsg(db) });
        rc = sqlite3_step(st); sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "BEGIN IMMEDIATE" });
        return DbResult<void>::Ok();
    }
    static inline DbResult<void> commit(sqlite3* db) {
        sqlite3_stmt* st{};
        int rc = sqlite3_prepare_v2(db, "COMMIT;", -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), rc, sqlite3_errmsg(db) });
        rc = sqlite3_step(st); sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "COMMIT" });
        return DbResult<void>::Ok();
    }
    static inline void rollback(sqlite3* db) {
        sqlite3_stmt* st{};
        if (sqlite3_prepare_v2(db, "ROLLBACK;", -1, &st, nullptr) == SQLITE_OK) { sqlite3_step(st); sqlite3_finalize(st); }
    }

    DbResult<std::vector<int64_t>> impl_insert_many(
        DbEnv& env,
        const std::vector<UiConfigRow>& rows) 
    {
        if (rows.empty()) return DbResult<std::vector<int64_t>>::Ok({});
        sqlite3* db = env.handle();

        sqlite3_stmt* st{};
        const char* sql =
            "INSERT INTO ui_config_rows(preset_id,turn_index,actor_slot,created_at) "
            "VALUES(?,?,?,strftime('%s','now')) RETURNING id;";
        int rc = sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
        if (rc != SQLITE_OK) { rollback(db); return DbResult<std::vector<int64_t>>::Err({ map_sqlite_err(sqlite3_errcode(db)), rc, sqlite3_errmsg(db) }); }

        std::vector<int64_t> out; out.reserve(rows.size());
        for (const auto& r : rows) {
            sqlite3_reset(st);
            sqlite3_clear_bindings(st);
            sqlite3_bind_int64(st, 1, r.preset_id);
            sqlite3_bind_int(st, 2, r.turn_index);
            sqlite3_bind_int(st, 3, r.actor_slot);
            rc = sqlite3_step(st);
            if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
                sqlite3_finalize(st); rollback(db);
                return DbResult<std::vector<int64_t>>::Err({ map_sqlite_err(rc), rc, "insert ui_config_row" });
            }
            int64_t id = (rc == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : sqlite3_last_insert_rowid(db);
            out.push_back(id);
        }
        sqlite3_finalize(st);
        return DbResult<std::vector<int64_t>>::Ok(std::move(out));
    }

    std::future<DbResult<std::vector<int64_t>>> UiConfigRowRepo::InsertManyAsync(const std::vector<UiConfigRow>& rows, RetryPolicy rp) {
        return DBService::instance().submit_res<std::vector<int64_t>>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_insert_many(e, rows); });
    }

    DbResult<std::vector<UiConfigRow>> impl_get_by_ids(
        DbEnv& env, 
        const std::vector<int64_t>& ids)
    {
        if (ids.empty()) return DbResult<std::vector<UiConfigRow>>::Ok({});
        sqlite3* db = env.handle();

        // Build IN clause
        std::string sql = "SELECT id,preset_id,turn_index,actor_slot,created_at FROM ui_config_rows WHERE id IN (";
        for (size_t i = 0; i < ids.size(); ++i) { sql += (i ? "," : ""); sql += "?"; }
        sql += ");";

        sqlite3_stmt* st{};
        int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<std::vector<UiConfigRow>>::Err({ map_sqlite_err(sqlite3_errcode(db)), rc, sqlite3_errmsg(db) });
        for (size_t i = 0; i < ids.size(); ++i) sqlite3_bind_int64(st, (int)i + 1, ids[i]);

        std::unordered_map<int64_t, UiConfigRow> map;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            UiConfigRow r{};
            r.id = sqlite3_column_int64(st, 0);
            r.preset_id = sqlite3_column_int64(st, 1);
            r.turn_index = sqlite3_column_int(st, 2);
            r.actor_slot = sqlite3_column_int(st, 3);
            r.created_at = sqlite3_column_int64(st, 4);
            map.emplace(r.id, r);
        }
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<std::vector<UiConfigRow>>::Err({ map_sqlite_err(rc), rc, "select ui_config_rows" });

        // Reorder to match input vector order; omit missing ids
        std::vector<UiConfigRow> out; out.reserve(ids.size());
        for (auto id : ids) {
            auto it = map.find(id);
            if (it != map.end()) out.push_back(it->second);
        }
        return DbResult<std::vector<UiConfigRow>>::Ok(std::move(out));
    }

    std::future<DbResult<std::vector<UiConfigRow>>> UiConfigRowRepo::GetByIdsAsync(const std::vector<int64_t>& ids, RetryPolicy rp) {
        return DBService::instance().submit_res<std::vector<UiConfigRow>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_get_by_ids(e, ids); });
    }

} // namespace simcore::db
