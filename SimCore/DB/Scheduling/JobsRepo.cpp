// SimCore/DB/JobsRepo.cpp
#include "JobsRepo.h"
#include <sqlite3.h>

namespace simcore::db {

    static DbResult<int64_t> impl_create_or_get(DbEnv& env,
        int64_t job_set_id, int program_kind, int program_version,
        int64_t program_ref_id, const std::string& fingerprint, int priority,
        const std::optional<std::string>& vm_kv) {

        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;

        if (sqlite3_prepare_v2(db,
            "INSERT INTO jobs(job_set_id,program_kind,program_version,program_ref_id,fingerprint,priority,state,attempts,max_attempts,queued_at,vm_kv)"
            " VALUES(?,?,?,?,?,?, 'QUEUED',0,5,strftime('%s','now'),?)"
            " ON CONFLICT(fingerprint) DO UPDATE SET fingerprint=fingerprint"
            " RETURNING job_id", -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<int64_t>::Err(sqlite3_errmsg(db));
        }

        sqlite3_bind_int64(st, 1, job_set_id);
        sqlite3_bind_int(st, 2, program_kind);
        sqlite3_bind_int(st, 3, program_version);
        sqlite3_bind_int64(st, 4, program_ref_id);
        sqlite3_bind_text(st, 5, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 6, priority);
        if (vm_kv && !vm_kv->empty())
            sqlite3_bind_text(st, 7, vm_kv->c_str(), -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(st, 7);

        int64_t out_id = 0;
        if (sqlite3_step(st) == SQLITE_ROW) out_id = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        if (!out_id) return DbResult<int64_t>::Err("failed to upsert job");
        return DbResult<int64_t>::Ok(out_id);
    }

    std::future<DbResult<int64_t>> JobsRepo::CreateOrGetByFingerprintAsync(
        int64_t job_set_id, int program_kind, int program_version,
        int64_t program_ref_id, std::string fingerprint, int priority,
        std::optional<std::string> vm_kv, RetryPolicy rp) {

        return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& e) {
                return impl_create_or_get(e, job_set_id, program_kind, program_version, program_ref_id, fingerprint, priority, vm_kv);
            });
    }

    static DbResult<JobRow> impl_get(DbEnv& env, int64_t job_id) {
        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT job_id,job_set_id,program_kind,program_version,program_ref_id,fingerprint,priority,state,attempts,max_attempts,claimed_by_token,lease_expires_at,queued_at,vm_kv"
            " FROM jobs WHERE job_id=?", -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<JobRow>::Err(sqlite3_errmsg(db));
        }
        sqlite3_bind_int64(st, 1, job_id);
        JobRow r{};
        if (sqlite3_step(st) == SQLITE_ROW) {
            r.job_id = sqlite3_column_int64(st, 0);
            r.job_set_id = sqlite3_column_int64(st, 1);
            r.program_kind = sqlite3_column_int(st, 2);
            r.program_version = sqlite3_column_int(st, 3);
            r.program_ref_id = sqlite3_column_int64(st, 4);
            r.fingerprint = (const char*)sqlite3_column_text(st, 5);
            r.priority = sqlite3_column_int(st, 6);
            r.state = (const char*)sqlite3_column_text(st, 7);
            r.attempts = sqlite3_column_int(st, 8);
            r.max_attempts = sqlite3_column_int(st, 9);
            if (sqlite3_column_type(st, 10) != SQLITE_NULL) r.claimed_by_token = std::string((const char*)sqlite3_column_text(st, 10));
            if (sqlite3_column_type(st, 11) != SQLITE_NULL) r.lease_expires_at = sqlite3_column_int64(st, 11);
            r.queued_at = sqlite3_column_int64(st, 12);
            if (sqlite3_column_type(st, 13) != SQLITE_NULL) r.vm_kv = std::string((const char*)sqlite3_column_text(st, 13));
            sqlite3_finalize(st);
            return DbResult<JobRow>::Ok(std::move(r));
        }
        sqlite3_finalize(st);
        return DbResult<JobRow>::Err("job not found");
    }

    std::future<DbResult<JobRow>> JobsRepo::GetAsync(int64_t job_id, RetryPolicy rp) {
        return DBService::instance().submit_res<JobRow>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_get(e, job_id); });
    }

    static DbResult<void> impl_set_state(DbEnv& env, int64_t job_id, const std::string& s) {
        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "UPDATE jobs SET state=? WHERE job_id=?", -1, &st, nullptr) != SQLITE_OK)
            return DbResult<void>::Err(sqlite3_errmsg(db));
        sqlite3_bind_text(st, 1, s.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, job_id);
        sqlite3_step(st);
        sqlite3_finalize(st);
        return DbResult<void>::Ok();
    }

    std::future<DbResult<void>> JobsRepo::SetStateAsync(int64_t job_id, std::string new_state, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_set_state(e, job_id, new_state); });
    }

    static DbResult<void> impl_set_vm_kv(DbEnv& env, int64_t job_id, const std::optional<std::string>& kv) {
        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "UPDATE jobs SET vm_kv=? WHERE job_id=?", -1, &st, nullptr) != SQLITE_OK)
            return DbResult<void>::Err(sqlite3_errmsg(db));
        if (kv && !kv->empty())
            sqlite3_bind_text(st, 1, kv->c_str(), -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(st, 1);
        sqlite3_bind_int64(st, 2, job_id);
        sqlite3_step(st);
        sqlite3_finalize(st);
        return DbResult<void>::Ok();
    }

    std::future<DbResult<void>> JobsRepo::SetVmKvAsync(int64_t job_id, std::optional<std::string> vm_kv, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_set_vm_kv(e, job_id, vm_kv); });
    }

    static DbResult<int64_t> impl_create_or_get(DbEnv& env,
        int64_t job_set_id, int program_kind, int program_version,
        int64_t program_ref_id, const std::string& fingerprint, int priority,
        const std::optional<std::string>& vm_kv);

    static DbResult<JobRow> impl_get(DbEnv& env, int64_t job_id);
    static DbResult<void> impl_set_state(DbEnv& env, int64_t job_id, const std::string& new_state);
    static DbResult<void> impl_set_vm_kv(DbEnv& env, int64_t job_id, const std::optional<std::string>& vm_kv);

    static inline DbResult<std::optional<JobRow>> impl_claim_next_ready(DbEnv& env, const std::string& claim_token, int lease_seconds, double aging_factor) {
        sqlite3* db = env.handle();
        int rc = 0;
        sqlite3_stmt* st = nullptr;

        rc = sqlite3_exec(db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK) return DbResult<std::optional<JobRow>>::Err({ map_sqlite_err(rc), rc, "begin" });

        int64_t cand_id = 0;

        const char* sel =
            "SELECT j.job_id "
            "FROM jobs j "
            "JOIN program_kinds pk ON pk.kind_id = j.program_kind "
            "WHERE j.state='QUEUED' "
            "ORDER BY (pk.base_priority + j.priority + ((strftime('%s','now') - j.queued_at) * ?1)) DESC, j.queued_at ASC "
            "LIMIT 1;";
        rc = sqlite3_prepare_v2(db, sel, -1, &st, nullptr);
        if (rc != SQLITE_OK) { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); return DbResult<std::optional<JobRow>>::Err({ map_sqlite_err(rc), rc, "prepare sel" }); }
        sqlite3_bind_double(st, 1, aging_factor);
        if (sqlite3_step(st) == SQLITE_ROW) cand_id = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);

        if (cand_id == 0) {
            sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
            return DbResult<std::optional<JobRow>>::Ok(std::nullopt);
        }

        const char* upd =
            "UPDATE jobs SET state='CLAIMED', claimed_by_token=?1, lease_expires_at=(strftime('%s','now') + ?2) "
            "WHERE job_id=?3 AND state='QUEUED';";
        rc = sqlite3_prepare_v2(db, upd, -1, &st, nullptr);
        if (rc != SQLITE_OK) { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); return DbResult<std::optional<JobRow>>::Err({ map_sqlite_err(rc), rc, "prepare upd" }); }
        sqlite3_bind_text(st, 1, claim_token.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, lease_seconds);
        sqlite3_bind_int64(st, 3, cand_id);
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); return DbResult<std::optional<JobRow>>::Err({ map_sqlite_err(rc), rc, "update" }); }

        auto row = impl_get(env, cand_id);
        if (!row.ok) { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); return DbResult<std::optional<JobRow>>::Err(row.error); }

        rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK) return DbResult<std::optional<JobRow>>::Err({ map_sqlite_err(rc), rc, "commit" });
        return DbResult<std::optional<JobRow>>::Ok(std::optional<JobRow>(row.value));
    }

    static inline DbResult<void> impl_mark_running(DbEnv& env, int64_t job_id) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st = nullptr;
        int rc = sqlite3_prepare_v2(db,
            "UPDATE jobs SET state='RUNNING', attempts=attempts+1 WHERE job_id=?1;", -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare running" });
        sqlite3_bind_int64(st, 1, job_id);
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "exec running" });
        return DbResult<void>::Ok();
    }

    static inline DbResult<void> impl_renew_lease(DbEnv& env, int64_t job_id, int lease_seconds) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st = nullptr;
        int rc = sqlite3_prepare_v2(db,
            "UPDATE jobs SET lease_expires_at=(strftime('%s','now') + ?1) WHERE job_id=?2;", -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare renew" });
        sqlite3_bind_int(st, 1, lease_seconds);
        sqlite3_bind_int64(st, 2, job_id);
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "exec renew" });
        return DbResult<void>::Ok();
    }

    static inline DbResult<void> impl_requeue_expired(DbEnv& env) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st = nullptr;
        int rc = sqlite3_prepare_v2(db,
            "UPDATE jobs SET state='QUEUED', claimed_by_token=NULL "
            "WHERE state IN ('CLAIMED','RUNNING') AND lease_expires_at IS NOT NULL AND lease_expires_at < strftime('%s','now');",
            -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare requeue" });
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "exec requeue" });
        return DbResult<void>::Ok();
    }

    std::future<DbResult<std::optional<JobRow>>> JobsRepo::ClaimNextReadyAsync(std::string claim_token, int lease_seconds, double aging_factor, RetryPolicy rp) {
        return DBService::instance().submit_res<std::optional<JobRow>>(OpType::Write, Priority::High, rp,
            [=](DbEnv& e) { return impl_claim_next_ready(e, claim_token, lease_seconds, aging_factor); });
    }

    std::future<DbResult<void>> JobsRepo::MarkRunningAsync(int64_t job_id, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::High, rp,
            [=](DbEnv& e) { return impl_mark_running(e, job_id); });
    }

    std::future<DbResult<void>> JobsRepo::RenewLeaseAsync(int64_t job_id, int lease_seconds, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_renew_lease(e, job_id, lease_seconds); });
    }

    std::future<DbResult<void>> JobsRepo::RequeueExpiredLeasesAsync(RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::High, rp,
            [=](DbEnv& e) { return impl_requeue_expired(e); });
    }

} // namespace simcore::db
