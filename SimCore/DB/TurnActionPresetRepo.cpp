#include "TurnActionPresetRepo.h"
#include <sqlite3.h>

namespace simcore::db {

    static inline DbResult<int64_t> Impl_Insert(DbEnv& env, const TurnActionPresetRow& r) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        const char* sql =
            "INSERT INTO turn_action_presets(name,macro,target_kind,item_id,mask_bits,single_slot,same_as_pc,flags,target_expr_ini,created_at,updated_at) "
            "VALUES(?,?,?,?,?,?,?,?,?,strftime('%s','now'),strftime('%s','now')) RETURNING id;";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
            return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });

        sqlite3_bind_text(st, 1, r.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, r.macro);
        sqlite3_bind_int(st, 3, r.target_kind);
        if (r.item_id >= 0) sqlite3_bind_int(st, 4, r.item_id); else sqlite3_bind_null(st, 4);
        if (r.mask_bits != 0) sqlite3_bind_int(st, 5, r.mask_bits); else sqlite3_bind_null(st, 5);
        if (r.single_slot >= 0) sqlite3_bind_int(st, 6, r.single_slot); else sqlite3_bind_null(st, 6);
        if (r.same_as_pc >= 0) sqlite3_bind_int(st, 7, r.same_as_pc); else sqlite3_bind_null(st, 7);
        sqlite3_bind_int(st, 8, r.flags);
        if (!r.target_expr_ini.empty()) sqlite3_bind_text(st, 9, r.target_expr_ini.c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 9);

        int rc = sqlite3_step(st);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            sqlite3_finalize(st);
            return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "insert preset" });
        }
        int64_t id = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return DbResult<int64_t>::Ok(id);
    }

    static inline DbResult<void> Impl_Update(DbEnv& env, const TurnActionPresetRow& r) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        const char* sql =
            "UPDATE turn_action_presets SET name=?,macro=?,target_kind=?,item_id=?,mask_bits=?,single_slot=?,same_as_pc=?,flags=?,target_expr_ini=?,updated_at=strftime('%s','now') "
            "WHERE id=?;";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
            return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });

        sqlite3_bind_text(st, 1, r.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, r.macro);
        sqlite3_bind_int(st, 3, r.target_kind);
        if (r.item_id >= 0) sqlite3_bind_int(st, 4, r.item_id); else sqlite3_bind_null(st, 4);
        if (r.mask_bits != 0) sqlite3_bind_int(st, 5, r.mask_bits); else sqlite3_bind_null(st, 5);
        if (r.single_slot >= 0) sqlite3_bind_int(st, 6, r.single_slot); else sqlite3_bind_null(st, 6);
        if (r.same_as_pc >= 0) sqlite3_bind_int(st, 7, r.same_as_pc); else sqlite3_bind_null(st, 7);
        sqlite3_bind_int(st, 8, r.flags);
        if (!r.target_expr_ini.empty()) sqlite3_bind_text(st, 9, r.target_expr_ini.c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 9);
        sqlite3_bind_int64(st, 10, r.id);

        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update preset" });
        return DbResult<void>::Ok();
    }

    static inline DbResult<void> Impl_Delete(DbEnv& env, int64_t id) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        const char* sql = "DELETE FROM turn_action_presets WHERE id=?;";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
            return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        sqlite3_bind_int64(st, 1, id);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "delete preset" });
        return DbResult<void>::Ok();
    }

    static inline DbResult<TurnActionPresetRow> Impl_Get(DbEnv& env, int64_t id) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        const char* sql =
            "SELECT id,name,macro,target_kind,item_id,mask_bits,single_slot,same_as_pc,flags,target_expr_ini,created_at,updated_at "
            "FROM turn_action_presets WHERE id=?;";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
            return DbResult<TurnActionPresetRow>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        sqlite3_bind_int64(st, 1, id);
        int rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) { sqlite3_finalize(st); return DbResult<TurnActionPresetRow>::Err({ DbErrorKind::NotFound, 0, "not found" }); }
        TurnActionPresetRow r{};
        r.id = sqlite3_column_int64(st, 0);
        r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        r.macro = sqlite3_column_int(st, 2);
        r.target_kind = sqlite3_column_int(st, 3);
        r.item_id = sqlite3_column_type(st, 4) == SQLITE_NULL ? -1 : sqlite3_column_int(st, 4);
        r.mask_bits = sqlite3_column_type(st, 5) == SQLITE_NULL ? 0 : sqlite3_column_int(st, 5);
        r.single_slot = sqlite3_column_type(st, 6) == SQLITE_NULL ? -1 : sqlite3_column_int(st, 6);
        r.same_as_pc = sqlite3_column_type(st, 7) == SQLITE_NULL ? -1 : sqlite3_column_int(st, 7);
        r.flags = sqlite3_column_int(st, 8);
        if (sqlite3_column_type(st, 9) != SQLITE_NULL) r.target_expr_ini = reinterpret_cast<const char*>(sqlite3_column_text(st, 9));
        r.created_at = sqlite3_column_int64(st, 10);
        r.updated_at = sqlite3_column_int64(st, 11);
        sqlite3_finalize(st);
        return DbResult<TurnActionPresetRow>::Ok(std::move(r));
    }

    static inline DbResult<std::optional<TurnActionPresetRow>> Impl_FindByName(DbEnv& env, const std::string& name) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        const char* sql =
            "SELECT id,name,macro,target_kind,item_id,mask_bits,single_slot,same_as_pc,flags,target_expr_ini,created_at,updated_at "
            "FROM turn_action_presets WHERE LOWER(name)=LOWER(?) LIMIT 1;";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
            return DbResult<std::optional<TurnActionPresetRow>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) { sqlite3_finalize(st); return DbResult<std::optional<TurnActionPresetRow>>::Ok(std::nullopt); }
        TurnActionPresetRow r{};
        r.id = sqlite3_column_int64(st, 0);
        r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        r.macro = sqlite3_column_int(st, 2);
        r.target_kind = sqlite3_column_int(st, 3);
        r.item_id = sqlite3_column_type(st, 4) == SQLITE_NULL ? -1 : sqlite3_column_int(st, 4);
        r.mask_bits = sqlite3_column_type(st, 5) == SQLITE_NULL ? 0 : sqlite3_column_int(st, 5);
        r.single_slot = sqlite3_column_type(st, 6) == SQLITE_NULL ? -1 : sqlite3_column_int(st, 6);
        r.same_as_pc = sqlite3_column_type(st, 7) == SQLITE_NULL ? -1 : sqlite3_column_int(st, 7);
        r.flags = sqlite3_column_int(st, 8);
        if (sqlite3_column_type(st, 9) != SQLITE_NULL) r.target_expr_ini = reinterpret_cast<const char*>(sqlite3_column_text(st, 9));
        r.created_at = sqlite3_column_int64(st, 10);
        r.updated_at = sqlite3_column_int64(st, 11);
        sqlite3_finalize(st);
        return DbResult<std::optional<TurnActionPresetRow>>::Ok(std::move(r));
    }

    static inline DbResult<std::vector<TurnActionPresetLite>> Impl_ListLite(DbEnv& env, const std::string& search, int32_t limit) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        const char* sql =
            "SELECT id,name,macro,target_kind FROM turn_action_presets "
            "WHERE LOWER(name) LIKE LOWER(?) ESCAPE '\\' "
            "ORDER BY name ASC LIMIT ?;";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
            return DbResult<std::vector<TurnActionPresetLite>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });

        std::string pattern = "%";
        for (char c : search) { if (c == '%' || c == '_' || c == '\\') pattern.push_back('\\'); pattern.push_back(c); }
        pattern.push_back('%');
        sqlite3_bind_text(st, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, limit <= 0 ? 50 : limit);

        std::vector<TurnActionPresetLite> out;
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            TurnActionPresetLite r{};
            r.id = sqlite3_column_int64(st, 0);
            r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
            r.macro = sqlite3_column_int(st, 2);
            r.target_kind = sqlite3_column_int(st, 3);
            out.emplace_back(std::move(r));
        }
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<std::vector<TurnActionPresetLite>>::Err({ map_sqlite_err(rc), rc, "list presets" });
        return DbResult<std::vector<TurnActionPresetLite>>::Ok(std::move(out));
    }

    std::future<DbResult<int64_t>> TurnActionPresetRepo::InsertAsync(const TurnActionPresetRow& r, RetryPolicy rp) {
        return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp, [r](DbEnv& env) { return Impl_Insert(env, r); });
    }
    std::future<DbResult<void>> TurnActionPresetRepo::UpdateAsync(const TurnActionPresetRow& r, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp, [r](DbEnv& env) { return Impl_Update(env, r); });
    }
    std::future<DbResult<void>> TurnActionPresetRepo::DeleteAsync(int64_t id, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp, [=](DbEnv& env) { return Impl_Delete(env, id); });
    }
    std::future<DbResult<TurnActionPresetRow>> TurnActionPresetRepo::GetAsync(int64_t id, RetryPolicy rp) {
        return DBService::instance().submit_res<TurnActionPresetRow>(OpType::Read, Priority::Normal, rp, [=](DbEnv& env) { return Impl_Get(env, id); });
    }
    std::future<DbResult<std::optional<TurnActionPresetRow>>> TurnActionPresetRepo::FindByNameAsync(const std::string& name, RetryPolicy rp) {
        return DBService::instance().submit_res<std::optional<TurnActionPresetRow>>(OpType::Read, Priority::Normal, rp, [name](DbEnv& env) { return Impl_FindByName(env, name); });
    }
    std::future<DbResult<std::vector<TurnActionPresetLite>>> TurnActionPresetRepo::ListLiteAsync(const std::string& search, int32_t limit, RetryPolicy rp) {
        return DBService::instance().submit_res<std::vector<TurnActionPresetLite>>(OpType::Read, Priority::Normal, rp, [=](DbEnv& env) { return Impl_ListLite(env, search, limit); });
    }

} // namespace simcore::db
