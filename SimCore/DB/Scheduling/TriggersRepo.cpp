#include "TriggersRepo.h"
#include <sqlite3.h>

namespace simcore::db {

    static DbResult<std::vector<TriggerRow>> impl_list(DbEnv& env, const char* scope, int64_t scope_id) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT trigger_id,scope,scope_id,action_kind,condition,action_args,active "
            "FROM triggers WHERE active=1 AND scope=? AND scope_id=?";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<std::vector<TriggerRow>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "prepare triggers list" });
        }
        sqlite3_bind_text(st, 1, scope, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, scope_id);

        std::vector<TriggerRow> out;
        while (sqlite3_step(st) == SQLITE_ROW) {
            TriggerRow r{};
            r.trigger_id = sqlite3_column_int64(st, 0);
            r.scope = (const char*)sqlite3_column_text(st, 1);
            r.scope_id = sqlite3_column_int64(st, 2);
            r.action_kind = sqlite3_column_int(st, 3);
            if (sqlite3_column_type(st, 4) != SQLITE_NULL) r.condition = (const char*)sqlite3_column_text(st, 4);
            if (sqlite3_column_type(st, 5) != SQLITE_NULL) r.action_args = (const char*)sqlite3_column_text(st, 5);
            r.active = sqlite3_column_int(st, 6);
            out.push_back(std::move(r));
        }
        sqlite3_finalize(st);
        return DbResult<std::vector<TriggerRow>>::Ok(std::move(out));
    }

    std::future<DbResult<std::vector<TriggerRow>>> TriggersRepo::ListActiveByJobAsync(int64_t job_id, RetryPolicy rp) {
        return DBService::instance().submit_res<std::vector<TriggerRow>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_list(e, "job", job_id); });
    }
    std::future<DbResult<std::vector<TriggerRow>>> TriggersRepo::ListActiveByJobSetAsync(int64_t job_set_id, RetryPolicy rp) {
        return DBService::instance().submit_res<std::vector<TriggerRow>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_list(e, "job_set", job_set_id); });
    }

    static DbResult<bool> impl_deactivate(DbEnv& env, int64_t trigger_id) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st = nullptr;
        const char* sql = "UPDATE triggers SET active=0 WHERE trigger_id=? AND active=1";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<bool>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "prepare triggers deactivate" });
        }
        sqlite3_bind_int64(st, 1, trigger_id);
        int rc = sqlite3_step(st);
        int changes = sqlite3_changes(db);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            return DbResult<bool>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "exec deactivate" });
        }
        return DbResult<bool>::Ok(changes > 0);
    }

    std::future<DbResult<bool>> TriggersRepo::TryDeactivateAsync(int64_t trigger_id, RetryPolicy rp) {
        return DBService::instance().submit_res<bool>(OpType::Write, Priority::High, rp,
            [=](DbEnv& e) { return impl_deactivate(e, trigger_id); });
    }

    // Helper to validate scope string
    static inline bool is_valid_scope(const std::string& s) {
        return (s == "job" || s == "job_set");
    }

    static DbResult<int64_t> impl_add(DbEnv& env,
        const std::string& scope,
        int64_t scope_id,
        int action_kind,
        const std::optional<std::string>& condition_ini,
        const std::optional<std::string>& action_args_ini,
        int active)
    {
        if (!is_valid_scope(scope)) {
            return DbResult<int64_t>::Err({ DbErrorKind::InvalidArgument, 0, "invalid scope (expected 'job' or 'job_set')" });
        }

        sqlite3* db = env.handle();
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "INSERT INTO triggers (scope, scope_id, action_kind, condition, action_args, active) "
            "VALUES (?, ?, ?, ?, ?, ?);";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "prepare insert trigger" });
        }

        int rc = SQLITE_OK;
        rc = sqlite3_bind_text(st, 1, scope.c_str(), -1, SQLITE_TRANSIENT); if (rc != SQLITE_OK) goto bind_fail;
        rc = sqlite3_bind_int64(st, 2, scope_id);                            if (rc != SQLITE_OK) goto bind_fail;
        rc = sqlite3_bind_int(st, 3, action_kind);                         if (rc != SQLITE_OK) goto bind_fail;

        if (condition_ini && !condition_ini->empty())
            rc = sqlite3_bind_text(st, 4, condition_ini->c_str(), -1, SQLITE_TRANSIENT);
        else
            rc = sqlite3_bind_null(st, 4);
        if (rc != SQLITE_OK) goto bind_fail;

        if (action_args_ini && !action_args_ini->empty())
            rc = sqlite3_bind_text(st, 5, action_args_ini->c_str(), -1, SQLITE_TRANSIENT);
        else
            rc = sqlite3_bind_null(st, 5);
        if (rc != SQLITE_OK) goto bind_fail;

        rc = sqlite3_bind_int(st, 6, active ? 1 : 0);                        if (rc != SQLITE_OK) goto bind_fail;

        rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) {
            int ec = sqlite3_errcode(db);
            sqlite3_finalize(st);
            return DbResult<int64_t>::Err({ map_sqlite_err(ec), ec, "exec insert trigger" });
        }
        {
            int64_t id = sqlite3_last_insert_rowid(db);
            sqlite3_finalize(st);
            return DbResult<int64_t>::Ok(id);
        }

    bind_fail:
        {
            int ec = sqlite3_errcode(db);
            sqlite3_finalize(st);
            return DbResult<int64_t>::Err({ map_sqlite_err(ec), ec, "bind insert trigger" });
        }
    }

    std::future<DbResult<int64_t>> TriggersRepo::AddAsync(
        const std::string& scope,
        int64_t scope_id,
        int action_kind,
        std::optional<std::string> condition_ini,
        std::optional<std::string> action_args_ini,
        int active,
        RetryPolicy rp)
    {
        return DBService::instance().submit_res<int64_t>(
            OpType::Write, Priority::High, rp,
            [=](DbEnv& e) {
                return impl_add(e, scope, scope_id, action_kind, condition_ini, action_args_ini, active);
            });
    }

    std::future<DbResult<int64_t>> TriggersRepo::AddForJobAsync(
        int64_t job_id,
        int action_kind,
        std::optional<std::string> condition_ini,
        std::optional<std::string> action_args_ini,
        int active,
        RetryPolicy rp)
    {
        return AddAsync(std::string("job"), job_id, action_kind,
            std::move(condition_ini), std::move(action_args_ini), active, rp);
    }

    std::future<DbResult<int64_t>> TriggersRepo::AddForJobSetAsync(
        int64_t job_set_id,
        int action_kind,
        std::optional<std::string> condition_ini,
        std::optional<std::string> action_args_ini,
        int active,
        RetryPolicy rp)
    {
        return AddAsync(std::string("job_set"), job_set_id, action_kind,
            std::move(condition_ini), std::move(action_args_ini), active, rp);
    }


} // namespace simcore::db
