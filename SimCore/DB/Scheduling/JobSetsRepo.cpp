// SimCore/DB/JobSetsRepo.cpp
#include "JobSetsRepo.h"
#include <sqlite3.h>

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
            return DbResult<int64_t>::Err(sqlite3_errmsg(db));
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
        if (!id) return DbResult<int64_t>::Err("failed to insert job_set");
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
            return DbResult<JobSetRow>::Err(sqlite3_errmsg(db));
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
        return DbResult<JobSetRow>::Err("job_set not found");
    }

    std::future<DbResult<JobSetRow>> JobSetsRepo::GetAsync(int64_t job_set_id, RetryPolicy rp) {
        return DBService::instance().submit_res<JobSetRow>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_get(e, job_set_id); });
    }

} // namespace simcore::db
