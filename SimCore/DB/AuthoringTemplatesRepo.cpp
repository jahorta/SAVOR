#include "AuthoringTemplatesRepo.h"
#include <sqlite3.h>

namespace simcore::db {

    static inline DbResult<int64_t> Impl_Insert(DbEnv& env, const AuthoringTemplateRow& r) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        const char* sql =
            "INSERT INTO authoring_templates(name,description,seed_probe_id,ui_config_ini,predicate_specs_ini,last_materialized_settings_id,last_codec_version_seen,created_at,updated_at) "
            "VALUES(?,?,?,?,?,?,?,strftime('%s','now'),strftime('%s','now')) RETURNING id;";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
            return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });

        sqlite3_bind_text(st, 1, r.name.c_str(), -1, SQLITE_TRANSIENT);
        if (!r.description.empty()) sqlite3_bind_text(st, 2, r.description.c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 2);
        if (r.seed_probe_id) sqlite3_bind_int64(st, 3, *r.seed_probe_id); else sqlite3_bind_null(st, 3);
        sqlite3_bind_text(st, 4, r.ui_config_ini.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, r.predicate_specs_ini.c_str(), -1, SQLITE_TRANSIENT);
        if (r.last_materialized_settings_id) sqlite3_bind_int64(st, 6, *r.last_materialized_settings_id); else sqlite3_bind_null(st, 6);
        if (r.last_codec_version_seen) sqlite3_bind_int(st, 7, *r.last_codec_version_seen); else sqlite3_bind_null(st, 7);

        int rc = sqlite3_step(st);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            sqlite3_finalize(st);
            return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "insert authoring template" });
        }
        int64_t id = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return DbResult<int64_t>::Ok(id);
    }

    static inline DbResult<void> Impl_Update(DbEnv& env, const AuthoringTemplateRow& r) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        const char* sql =
            "UPDATE authoring_templates SET name=?,description=?,seed_probe_id=?,ui_config_ini=?,predicate_specs_ini=?,last_materialized_settings_id=?,last_codec_version_seen=?,updated_at=strftime('%s','now') "
            "WHERE id=?;";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
            return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });

        sqlite3_bind_text(st, 1, r.name.c_str(), -1, SQLITE_TRANSIENT);
        if (!r.description.empty()) sqlite3_bind_text(st, 2, r.description.c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 2);
        if (r.seed_probe_id) sqlite3_bind_int64(st, 3, *r.seed_probe_id); else sqlite3_bind_null(st, 3);
        sqlite3_bind_text(st, 4, r.ui_config_ini.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, r.predicate_specs_ini.c_str(), -1, SQLITE_TRANSIENT);
        if (r.last_materialized_settings_id) sqlite3_bind_int64(st, 6, *r.last_materialized_settings_id); else sqlite3_bind_null(st, 6);
        if (r.last_codec_version_seen) sqlite3_bind_int(st, 7, *r.last_codec_version_seen); else sqlite3_bind_null(st, 7);
        sqlite3_bind_int64(st, 8, r.id);

        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update authoring template" });
        return DbResult<void>::Ok();
    }

    static inline DbResult<void> Impl_Delete(DbEnv& env, int64_t id) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        const char* sql = "DELETE FROM authoring_templates WHERE id=?;";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
            return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        sqlite3_bind_int64(st, 1, id);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "delete authoring template" });
        return DbResult<void>::Ok();
    }

    static inline DbResult<AuthoringTemplateRow> Impl_Get(DbEnv& env, int64_t id) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        const char* sql =
            "SELECT id,name,description,seed_probe_id,ui_config_ini,predicate_specs_ini,last_materialized_settings_id,last_codec_version_seen,created_at,updated_at "
            "FROM authoring_templates WHERE id=?;";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
            return DbResult<AuthoringTemplateRow>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        sqlite3_bind_int64(st, 1, id);
        int rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) { sqlite3_finalize(st); return DbResult<AuthoringTemplateRow>::Err({ DbErrorKind::NotFound, 0, "not found" }); }
        AuthoringTemplateRow r{};
        r.id = sqlite3_column_int64(st, 0);
        r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        if (sqlite3_column_type(st, 2) != SQLITE_NULL) r.description = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        if (sqlite3_column_type(st, 3) != SQLITE_NULL) r.seed_probe_id = sqlite3_column_int64(st, 3);
        r.ui_config_ini = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
        r.predicate_specs_ini = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
        if (sqlite3_column_type(st, 6) != SQLITE_NULL) r.last_materialized_settings_id = sqlite3_column_int64(st, 6);
        if (sqlite3_column_type(st, 7) != SQLITE_NULL) r.last_codec_version_seen = sqlite3_column_int(st, 7);
        r.created_at = sqlite3_column_int64(st, 8);
        r.updated_at = sqlite3_column_int64(st, 9);
        sqlite3_finalize(st);
        return DbResult<AuthoringTemplateRow>::Ok(std::move(r));
    }

    static inline DbResult<std::optional<AuthoringTemplateRow>> Impl_FindByName(DbEnv& env, const std::string& name) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        const char* sql =
            "SELECT id,name,description,seed_probe_id,ui_config_ini,predicate_specs_ini,last_materialized_settings_id,last_codec_version_seen,created_at,updated_at "
            "FROM authoring_templates WHERE LOWER(name)=LOWER(?) LIMIT 1;";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
            return DbResult<std::optional<AuthoringTemplateRow>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) { sqlite3_finalize(st); return DbResult<std::optional<AuthoringTemplateRow>>::Ok(std::nullopt); }
        AuthoringTemplateRow r{};
        r.id = sqlite3_column_int64(st, 0);
        r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        if (sqlite3_column_type(st, 2) != SQLITE_NULL) r.description = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        if (sqlite3_column_type(st, 3) != SQLITE_NULL) r.seed_probe_id = sqlite3_column_int64(st, 3);
        r.ui_config_ini = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
        r.predicate_specs_ini = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
        if (sqlite3_column_type(st, 6) != SQLITE_NULL) r.last_materialized_settings_id = sqlite3_column_int64(st, 6);
        if (sqlite3_column_type(st, 7) != SQLITE_NULL) r.last_codec_version_seen = sqlite3_column_int(st, 7);
        r.created_at = sqlite3_column_int64(st, 8);
        r.updated_at = sqlite3_column_int64(st, 9);
        sqlite3_finalize(st);
        return DbResult<std::optional<AuthoringTemplateRow>>::Ok(std::move(r));
    }

    static inline DbResult<std::vector<AuthoringTemplateLite>> Impl_ListLite(DbEnv& env, const std::string& search, int32_t limit) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        const char* sql =
            "SELECT id,name FROM authoring_templates WHERE LOWER(name) LIKE LOWER(?) ESCAPE '\\' ORDER BY name ASC LIMIT ?;";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
            return DbResult<std::vector<AuthoringTemplateLite>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });

        std::string pattern = "%";
        for (char c : search) { if (c == '%' || c == '_' || c == '\\') pattern.push_back('\\'); pattern.push_back(c); }
        pattern.push_back('%');
        sqlite3_bind_text(st, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, limit <= 0 ? 50 : limit);

        std::vector<AuthoringTemplateLite> out;
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            AuthoringTemplateLite r{};
            r.id = sqlite3_column_int64(st, 0);
            r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
            out.emplace_back(std::move(r));
        }
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<std::vector<AuthoringTemplateLite>>::Err({ map_sqlite_err(rc), rc, "list authoring templates" });
        return DbResult<std::vector<AuthoringTemplateLite>>::Ok(std::move(out));
    }

    std::future<DbResult<int64_t>> AuthoringTemplatesRepo::InsertAsync(const AuthoringTemplateRow& r, RetryPolicy rp) {
        return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp, [r](DbEnv& env) { return Impl_Insert(env, r); });
    }
    std::future<DbResult<void>> AuthoringTemplatesRepo::UpdateAsync(const AuthoringTemplateRow& r, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp, [r](DbEnv& env) { return Impl_Update(env, r); });
    }
    std::future<DbResult<void>> AuthoringTemplatesRepo::DeleteAsync(int64_t id, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp, [=](DbEnv& env) { return Impl_Delete(env, id); });
    }
    std::future<DbResult<AuthoringTemplateRow>> AuthoringTemplatesRepo::GetAsync(int64_t id, RetryPolicy rp) {
        return DBService::instance().submit_res<AuthoringTemplateRow>(OpType::Read, Priority::Normal, rp, [=](DbEnv& env) { return Impl_Get(env, id); });
    }
    std::future<DbResult<std::optional<AuthoringTemplateRow>>> AuthoringTemplatesRepo::FindByNameAsync(const std::string& name, RetryPolicy rp) {
        return DBService::instance().submit_res<std::optional<AuthoringTemplateRow>>(OpType::Read, Priority::Normal, rp, [name](DbEnv& env) { return Impl_FindByName(env, name); });
    }
    std::future<DbResult<std::vector<AuthoringTemplateLite>>> AuthoringTemplatesRepo::ListLiteAsync(const std::string& search, int32_t limit, RetryPolicy rp) {
        return DBService::instance().submit_res<std::vector<AuthoringTemplateLite>>(OpType::Read, Priority::Normal, rp, [=](DbEnv& env) { return Impl_ListLite(env, search, limit); });
    }

} // namespace simcore::db
