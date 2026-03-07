// SimCore/DB/JobSetsRepo.cpp
#include "JobSetsRepo.h"
#include <sqlite3.h>
#include <sstream>

namespace simcore::db {

    static DbResult<int64_t> impl_create(DbEnv& env,
        const std::optional<std::string>& purpose, int program_kind,
        const std::optional<std::string>& created_by,
        const std::optional<std::string>& domain_ref_kind,
        const std::optional<int64_t>& domain_ref_id,
        const std::optional<std::string>& meta_text,
        const std::optional<int64_t>& expected_total) {

        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO job_sets(purpose,program_kind,created_by,created_at,domain_ref_kind,domain_ref_id,meta_text,expected_total)"
            " VALUES(?,?,?,strftime('%s','now'),?,?,?,?) RETURNING job_set_id", -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        }

        if (purpose && !purpose->empty()) sqlite3_bind_text(st, 1, purpose->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 1);
        sqlite3_bind_int(st, 2, program_kind);
        if (created_by && !created_by->empty()) sqlite3_bind_text(st, 3, created_by->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 3);
        if (domain_ref_kind && !domain_ref_kind->empty()) sqlite3_bind_text(st, 4, domain_ref_kind->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 4);
        if (domain_ref_id) sqlite3_bind_int64(st, 5, *domain_ref_id); else sqlite3_bind_null(st, 5);
        if (meta_text && !meta_text->empty()) sqlite3_bind_text(st, 6, meta_text->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 6);
        if (expected_total) sqlite3_bind_int64(st, 7, *expected_total); else sqlite3_bind_null(st, 7);

        int64_t id = 0;
        if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        if (!id) return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "failed to insert job_set" });
        return DbResult<int64_t>::Ok(id);
    }

    std::future<DbResult<int64_t>> JobSetsRepo::CreateAsync(
        std::optional<std::string> purpose, int program_kind,
        std::optional<std::string> created_by,
        std::optional<std::string> domain_ref_kind,
        std::optional<int64_t> domain_ref_id,
        std::optional<std::string> meta_text,
        std::optional<int64_t> expected_total,
        RetryPolicy rp) {

        return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_create(e, purpose, program_kind, created_by, domain_ref_kind, domain_ref_id, meta_text, expected_total); });
    }

    static DbResult<JobSetRow> impl_get(DbEnv& env, int64_t job_set_id) {
        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT job_set_id,purpose,program_kind,created_by,created_at,domain_ref_kind,domain_ref_id,meta_text,expected_total"
            " FROM job_sets WHERE job_set_id=?", -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<JobSetRow>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        }
        sqlite3_bind_int64(st, 1, job_set_id);
        JobSetRow r{};
        if (sqlite3_step(st) == SQLITE_ROW) {
            r.job_set_id = sqlite3_column_int64(st, 0);
            if (sqlite3_column_type(st, 1) != SQLITE_NULL) r.purpose = std::string((const char*)sqlite3_column_text(st, 1));
            r.program_kind = sqlite3_column_int(st, 2);
            if (sqlite3_column_type(st, 3) != SQLITE_NULL) r.created_by = std::string((const char*)sqlite3_column_text(st, 3));
            r.created_at = sqlite3_column_int64(st, 4);
            if (sqlite3_column_type(st, 5) != SQLITE_NULL) r.domain_ref_kind = std::string((const char*)sqlite3_column_text(st, 5));
            if (sqlite3_column_type(st, 6) != SQLITE_NULL) r.domain_ref_id = sqlite3_column_int64(st, 6);
            if (sqlite3_column_type(st, 7) != SQLITE_NULL) r.meta_text = std::string((const char*)sqlite3_column_text(st, 7));
            if (sqlite3_column_type(st, 8) != SQLITE_NULL) r.expected_total = sqlite3_column_int64(st, 8);
            sqlite3_finalize(st);
            return DbResult<JobSetRow>::Ok(std::move(r));
        }
        sqlite3_finalize(st);
        return DbResult<JobSetRow>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "job_set not found" });
    }

    std::future<DbResult<JobSetRow>> JobSetsRepo::GetAsync(int64_t job_set_id, RetryPolicy rp) {
        return DBService::instance().submit_res<JobSetRow>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_get(e, job_set_id); });
    }

    static DbResult<void> impl_set_meta_text(DbEnv& env, int64_t job_set_id, const std::optional<std::string>& meta_text) {
        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;

        if (sqlite3_prepare_v2(db, "UPDATE job_sets SET meta_text=? WHERE job_set_id=?", -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "prepare failed" });
        }

        int rc = (meta_text ? sqlite3_bind_text(st, 1, meta_text->c_str(), -1, SQLITE_TRANSIENT)
            : sqlite3_bind_null(st, 1));
        if (rc != SQLITE_OK || sqlite3_bind_int64(st, 2, job_set_id) != SQLITE_OK) {
            sqlite3_finalize(st);
            return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "bind failed" });
        }

        rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) {
            auto err = DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "step failed" });
            sqlite3_finalize(st);
            return err;
        }

        sqlite3_finalize(st);
        return DbResult<void>::Ok();
    }

    static DbResult<void> impl_set_expected_total(DbEnv& env, int64_t job_set_id, const std::optional<int64_t>& expected_total) {
        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;

        if (sqlite3_prepare_v2(db, "UPDATE job_sets SET expected_total=? WHERE job_set_id=?", -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "prepare failed" });
        }

        int rc = (expected_total ? sqlite3_bind_int64(st, 1, *expected_total)
            : sqlite3_bind_null(st, 1));
        if (rc != SQLITE_OK || sqlite3_bind_int64(st, 2, job_set_id) != SQLITE_OK) {
            sqlite3_finalize(st);
            return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "bind failed" });
        }

        rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) {
            auto err = DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "step failed" });
            sqlite3_finalize(st);
            return err;
        }

        sqlite3_finalize(st);
        return DbResult<void>::Ok();
    }

    std::future<DbResult<void>> JobSetsRepo::SetMetaTextAsync(int64_t job_set_id, std::optional<std::string> meta_text, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_set_meta_text(e, job_set_id, meta_text); });
    }

    std::future<DbResult<void>> JobSetsRepo::SetExpectedTotalAsync(int64_t job_set_id, std::optional<int64_t> expected_total, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_set_expected_total(e, job_set_id, expected_total); });
    }

    static DbResult<Page<JobSetLite>> impl_list_recent_job_sets(
        DbEnv& env,
        const JobSetsListScope& scope,
        const std::optional<KeysetCursor>& before,
        int limit)
    {
        auto* db = env.handle();
        std::ostringstream sql;
        sql << "SELECT job_set_id, program_kind, "
            "CASE WHEN purpose IS NULL THEN '' ELSE purpose END AS purpose, "
            "CASE WHEN created_at IS NULL THEN 0  ELSE created_at END AS created_at "
            "FROM job_sets ";

        bool hasWhere = false;
        auto add_and = [&](bool cond) { if (cond) { sql << (hasWhere ? " AND " : " WHERE "); hasWhere = true; } };

        if (scope.program_kind) { add_and(true); sql << "program_kind=?"; }
        if (scope.min_job_set_id) { add_and(true); sql << "job_set_id >= ?"; }
        if (before) {
            add_and(true);
            sql << "(created_at < ? OR (created_at = ? AND job_set_id < ?))";
        }

        sql << " ORDER BY created_at DESC, job_set_id DESC LIMIT ?";

        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql.str().c_str(), -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<Page<JobSetLite>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        }

        int bi = 1;
        if (scope.program_kind) sqlite3_bind_int(st, bi++, *scope.program_kind);
        if (scope.min_job_set_id) sqlite3_bind_int64(st, bi++, *scope.min_job_set_id);
        if (before) {
            sqlite3_bind_int64(st, bi++, before->primary);   // created_at
            sqlite3_bind_int64(st, bi++, before->primary);   // created_at (tie)
            sqlite3_bind_int64(st, bi++, before->secondary); // job_set_id
        }
        sqlite3_bind_int(st, bi++, limit);

        Page<JobSetLite> page{};
        page.items.reserve(static_cast<size_t>(limit));
        while (true) {
            int rc = sqlite3_step(st);
            if (rc == SQLITE_ROW) {
                JobSetLite r{};
                r.job_set_id = sqlite3_column_int64(st, 0);
                r.program_kind = sqlite3_column_int(st, 1);
                if (sqlite3_column_type(st, 2) != SQLITE_NULL)
                    r.purpose = std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 2)));
                else
                    r.purpose.clear();
                r.created_at = sqlite3_column_int64(st, 3);
                page.items.push_back(std::move(r));
            }
            else if (rc == SQLITE_DONE) {
                break;
            }
            else {
                auto err = DbResult<Page<JobSetLite>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "step failed" });
                sqlite3_finalize(st);
                return err;
            }
        }

        sqlite3_finalize(st);

        if ((int)page.items.size() == limit) {
            const auto& last = page.items.back();
            page.next = KeysetCursor{ last.created_at, last.job_set_id };
        }
        return DbResult<Page<JobSetLite>>::Ok(std::move(page));
    }

    std::future<DbResult<Page<JobSetLite>>> JobSetsRepo::ListRecentAsync(
        const JobSetsListScope& scope,
        std::optional<KeysetCursor> before,
        int limit,
        RetryPolicy rp)
    {
        if (limit <= 0) limit = 50;
        return DBService::instance().submit_res<Page<JobSetLite>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_list_recent_job_sets(e, scope, before, limit); });
    }

    static DbResult<int64_t> impl_create_child(DbEnv& env,
        int64_t parent_job_set_id,
        const std::optional<std::string>& purpose, int program_kind,
        const std::optional<std::string>& created_by,
        const std::optional<std::string>& domain_ref_kind,
        const std::optional<int64_t>& domain_ref_id,
        const std::optional<std::string>& meta_text,
        const std::optional<int64_t>& expected_total)
    {
        sqlite3* db = env.handle();
        const char* sql =
            "INSERT INTO job_sets(purpose,program_kind,created_by,domain_ref_kind,domain_ref_id,meta_text,expected_total,parent_job_set_id) "
            "VALUES(?,?,?,?,?,?,?,?);";
        sqlite3_stmt* st = nullptr;
        int rc = sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
        if (rc != SQLITE_OK) 
        {
            const std::string err = "prepare job_sets insert child" + std::string(sqlite3_errmsg(db));
            return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, err.c_str()});
        }

        int idx = 1;
        if (purpose) sqlite3_bind_text(st, idx++, purpose->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, idx++);
        sqlite3_bind_int(st, idx++, program_kind);
        if (created_by) sqlite3_bind_text(st, idx++, created_by->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, idx++);
        if (domain_ref_kind) sqlite3_bind_text(st, idx++, domain_ref_kind->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, idx++);
        if (domain_ref_id) sqlite3_bind_int64(st, idx++, *domain_ref_id); else sqlite3_bind_null(st, idx++);
        if (meta_text) sqlite3_bind_text(st, idx++, meta_text->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, idx++);
        if (expected_total) sqlite3_bind_int64(st, idx++, *expected_total); else sqlite3_bind_null(st, idx++);
        sqlite3_bind_int64(st, idx++, parent_job_set_id);

        rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(st);
            return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "insert child job_set" });
        }
        sqlite3_finalize(st);
        return DbResult<int64_t>::Ok(sqlite3_last_insert_rowid(db));
    }

    // Public async wrappers
    std::future<DbResult<int64_t>> JobSetsRepo::CreateChildAsync(
        int64_t parent_job_set_id,
        std::optional<std::string> purpose,
        int program_kind,
        std::optional<std::string> created_by,
        std::optional<std::string> domain_ref_kind,
        std::optional<int64_t> domain_ref_id,
        std::optional<std::string> meta_text,
        std::optional<int64_t> expected_total,
        RetryPolicy rp)
    {
        return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::High, rp,
            [=](DbEnv& e) { return impl_create_child(e, parent_job_set_id, purpose, program_kind, created_by, domain_ref_kind, domain_ref_id, meta_text, expected_total); });
    }

    static DbResult<std::optional<int64_t>> impl_get_parent(DbEnv& env, int64_t job_set_id) {
        sqlite3* db = env.handle();
        const char* sql = "SELECT parent_job_set_id FROM job_sets WHERE job_set_id=?";
        sqlite3_stmt* st = nullptr;
        int rc = sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<std::optional<int64_t>>::Err({ map_sqlite_err(rc), rc, "prepare GetParent" });
        sqlite3_bind_int64(st, 1, job_set_id);
        std::optional<int64_t> out;
        if (sqlite3_step(st) == SQLITE_ROW) {
            if (sqlite3_column_type(st, 0) != SQLITE_NULL) out = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
        return DbResult<std::optional<int64_t>>::Ok(out);
    }

    std::future<DbResult<std::optional<int64_t>>> JobSetsRepo::GetParentAsync(int64_t job_set_id)
    {
        return DBService::instance().submit_res<std::optional<int64_t>>(OpType::Read, Priority::Normal, {},
            [=](DbEnv& env) { return impl_get_parent(env, job_set_id); });
    }

} // namespace simcore::db
