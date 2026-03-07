// SimCore/DB/JobsRepo.cpp
#include "JobsRepo.h"
#include <sqlite3.h>
#include <sstream>

namespace simcore::db {

    static DbResult<int64_t> impl_create_or_get(DbEnv& env,
            int64_t job_set_id, int program_kind, int program_version,
            int64_t program_ref_id, const std::string& fingerprint, int priority,
            const std::optional<std::string>& vm_kv,
            const std::optional<int64_t>& savestate_id)
            {
                auto* db = env.handle();
                sqlite3_stmt* st = nullptr;

                const char* sql =
                    "INSERT INTO jobs(job_set_id,program_kind,program_version,program_ref_id,"
                    "fingerprint,priority,state,attempts,max_attempts,queued_at,vm_kv,savestate_id) "
                    "VALUES(?,?,?,?,?,?, 'QUEUED',0,5,strftime('%s','now'),?,?) "
                    "ON CONFLICT(fingerprint) DO UPDATE SET fingerprint=fingerprint "
                    "RETURNING job_id";

                if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
                    return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
                }

                sqlite3_bind_int64(st, 1, job_set_id);
                sqlite3_bind_int(st, 2, program_kind);
                sqlite3_bind_int(st, 3, program_version);
                sqlite3_bind_int64(st, 4, program_ref_id);
                sqlite3_bind_text(st, 5, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(st, 6, priority);
                if (vm_kv && !vm_kv->empty()) sqlite3_bind_text(st, 7, vm_kv->c_str(), -1, SQLITE_TRANSIENT);
                else                          sqlite3_bind_null(st, 7);
                if (savestate_id)             sqlite3_bind_int64(st, 8, *savestate_id);
                else                          sqlite3_bind_null(st, 8);

                int64_t out_id = 0;
                if (sqlite3_step(st) == SQLITE_ROW) out_id = sqlite3_column_int64(st, 0);
                sqlite3_finalize(st);
                if (!out_id) return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "failed to upsert job" });
                return DbResult<int64_t>::Ok(out_id);
    }

    std::future<DbResult<int64_t>> JobsRepo::CreateOrGetByFingerprintAsync(
        int64_t job_set_id, int program_kind, int program_version,
        int64_t program_ref_id, std::string fingerprint, int priority,
        std::optional<std::string> vm_kv, std::optional<int64_t> savestate_id, RetryPolicy rp) {

        return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& e) {
                return impl_create_or_get(e, job_set_id, program_kind, program_version, program_ref_id, fingerprint, priority, vm_kv, savestate_id);
            });
    }

    static DbResult<JobRow> impl_get(DbEnv& env, int64_t job_id) {
        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT job_id,job_set_id,program_kind,program_version,program_ref_id,fingerprint,priority,"
            "state,attempts,max_attempts,claimed_by_token,lease_expires_at,queued_at,vm_kv,savestate_id "
            "FROM jobs WHERE job_id=?", -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<JobRow>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
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
            if (sqlite3_column_type(st, 14) != SQLITE_NULL) r.savestate_id = sqlite3_column_int64(st, 14);
            sqlite3_finalize(st);
            return DbResult<JobRow>::Ok(std::move(r));
        }
        sqlite3_finalize(st);
        return DbResult<JobRow>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "job not found" });
    }

    std::future<DbResult<JobRow>> JobsRepo::GetAsync(int64_t job_id, RetryPolicy rp) {
        return DBService::instance().submit_res<JobRow>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_get(e, job_id); });
    }

    static DbResult<void> impl_set_state(DbEnv& env, int64_t job_id, const std::string& s) {
        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "UPDATE jobs SET state=? WHERE job_id=?", -1, &st, nullptr) != SQLITE_OK)
            return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
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
            return DbResult<void>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
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

    static inline DbResult<std::optional<JobRow>> impl_claim_next_ready(DbEnv& env, const std::string& claim_token, int lease_seconds, double aging_factor, std::optional<int64_t> preferred_savestate_id) {
        sqlite3* db = env.handle();
        int rc = sqlite3_exec(db, "SAVEPOINT claim_job;", nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK) return DbResult<std::optional<JobRow>>::Err({ map_sqlite_err(rc), rc, "begin" });

        int64_t cand_id = 0;

        const char* sel =
            "SELECT j.job_id "
            "FROM jobs j "
            "JOIN program_kinds pk ON pk.kind_id = j.program_kind "
            "WHERE j.state='QUEUED' "
            "ORDER BY "
            "  CASE WHEN ?1 IS NULL "
            "       THEN CASE WHEN j.savestate_id IS NULL THEN 1 ELSE 0 END "
            "       ELSE CASE WHEN j.savestate_id = ?1   THEN 1 ELSE 0 END "
            "  END DESC, "
            "  (pk.base_priority + j.priority + ((strftime('%s','now') - j.queued_at) * ?2)) DESC, "
            "  j.queued_at ASC "
            "LIMIT 1;";

        sqlite3_stmt* st = nullptr;
        rc = sqlite3_prepare_v2(db, sel, -1, &st, nullptr);
        if (rc != SQLITE_OK) 
        { 
            sqlite3_exec(db, "ROLLBACK TO claim_job;", nullptr, nullptr, nullptr); 
            sqlite3_exec(db, "RELEASE claim_job;", nullptr, nullptr, nullptr); 
            return DbResult<std::optional<JobRow>>::Err({ map_sqlite_err(rc), rc, "prepare sel" }); 
        }

        if (preferred_savestate_id) sqlite3_bind_int64(st, 1, *preferred_savestate_id);
        else                        sqlite3_bind_null(st, 1);
        sqlite3_bind_double(st, 2, aging_factor);

        if (sqlite3_step(st) == SQLITE_ROW) cand_id = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);

        if (cand_id == 0) {
            sqlite3_exec(db, "RELEASE claim_job;", nullptr, nullptr, nullptr);
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
        if (rc != SQLITE_DONE) {
            sqlite3_exec(db, "ROLLBACK TO claim_job;", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "RELEASE claim_job;", nullptr, nullptr, nullptr);
            return DbResult<std::optional<JobRow>>::Err({ map_sqlite_err(rc), rc, "update" }); 
        }

        auto row = impl_get(env, cand_id);
        if (!row.ok) {
            sqlite3_exec(db, "ROLLBACK TO claim_job;", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "RELEASE claim_job;", nullptr, nullptr, nullptr); 
            return DbResult<std::optional<JobRow>>::Err(row.error); 
        }

        sqlite3_exec(db, "RELEASE claim_job;", nullptr, nullptr, nullptr);
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

    std::future<DbResult<std::optional<JobRow>>> JobsRepo::ClaimNextReadyAsync(
        std::string claim_token, int lease_seconds, double aging_factor,
        std::optional<int64_t> savestate_id, RetryPolicy rp) {
        return DBService::instance().submit_res<std::optional<JobRow>>(OpType::Write, Priority::High, rp,
            [=](DbEnv& e) { return impl_claim_next_ready(e, claim_token, lease_seconds, aging_factor, savestate_id); });
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

    static DbResult<std::vector<JobRow>> impl_list_by_job_set(DbEnv& env, int64_t job_set_id, bool queued_only) {
        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;

        const char* sql_all =
            "SELECT job_id,job_set_id,program_kind,program_version,program_ref_id,"
            "fingerprint,priority,state,attempts,max_attempts,claimed_by_token,"
            "lease_expires_at,queued_at,vm_kv,savestate_id "
            "FROM jobs WHERE job_set_id=? ORDER BY queued_at ASC";

        const char* sql_queued =
            "SELECT job_id,job_set_id,program_kind,program_version,program_ref_id,"
            "fingerprint,priority,state,attempts,max_attempts,claimed_by_token,"
            "lease_expires_at,queued_at,vm_kv,savestate_id "
            "FROM jobs WHERE job_set_id=? AND state='QUEUED' ORDER BY queued_at ASC";

        if (sqlite3_prepare_v2(db, queued_only ? sql_queued : sql_all, -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<std::vector<JobRow>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "prepare failed" });
        }
        if (sqlite3_bind_int64(st, 1, job_set_id) != SQLITE_OK) {
            sqlite3_finalize(st);
            return DbResult<std::vector<JobRow>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "bind failed" });
        }

        std::vector<JobRow> out;
        for (;;) {
            int rc = sqlite3_step(st);
            if (rc == SQLITE_ROW) {
                JobRow r{};
                r.job_id = sqlite3_column_int64(st, 0);
                r.job_set_id = sqlite3_column_int64(st, 1);
                r.program_kind = sqlite3_column_int(st, 2);
                r.program_version = sqlite3_column_int(st, 3);
                r.program_ref_id = sqlite3_column_int64(st, 4);
                r.fingerprint = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
                r.priority = sqlite3_column_int(st, 6);
                r.state = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
                r.attempts = sqlite3_column_int(st, 8);
                r.max_attempts = sqlite3_column_int(st, 9);

                if (sqlite3_column_type(st, 10) != SQLITE_NULL)
                    r.claimed_by_token = std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 10)));
                else
                    r.claimed_by_token.reset();

                if (sqlite3_column_type(st, 11) != SQLITE_NULL)
                    r.lease_expires_at = sqlite3_column_int64(st, 11);
                else
                    r.lease_expires_at.reset();

                r.queued_at = sqlite3_column_int64(st, 12);

                if (sqlite3_column_type(st, 13) != SQLITE_NULL)
                    r.vm_kv = std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 13)));
                else
                    r.vm_kv.reset();

                if (sqlite3_column_type(st, 14) != SQLITE_NULL)
                    r.savestate_id = sqlite3_column_int64(st, 14);
                else
                    r.savestate_id.reset();

                out.push_back(std::move(r));
                continue;
            }
            if (rc == SQLITE_DONE) break;

            auto err = DbResult<std::vector<JobRow>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "step failed" });
            sqlite3_finalize(st);
            return err;
        }

        sqlite3_finalize(st);
        return DbResult<std::vector<JobRow>>::Ok(std::move(out));
    }

    std::future<DbResult<std::vector<JobRow>>> JobsRepo::GetByJobSetAsync(int64_t job_set_id, RetryPolicy rp) {
        return DBService::instance().submit_res<std::vector<JobRow>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_list_by_job_set(e, job_set_id, false); });
    }

    std::future<DbResult<std::vector<JobRow>>> JobsRepo::GetQueuedByJobSetAsync(int64_t job_set_id, RetryPolicy rp) {
        return DBService::instance().submit_res<std::vector<JobRow>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_list_by_job_set(e, job_set_id, true); });
    }

    static DbResult<Page<JobLite>> impl_list_recent_jobs(
        DbEnv& env,
        const JobsListScope& scope,
        const std::optional<KeysetCursor>& before,
        int limit)
    {
        auto* db = env.handle();
        std::ostringstream sql;
        sql << "SELECT job_id, job_set_id, program_kind, state, priority, queued_at, savestate_id "
            "FROM jobs ";

        // WHERE
        bool hasWhere = false;
        auto add_and = [&](bool cond) { if (cond) { sql << (hasWhere ? " AND " : " WHERE "); hasWhere = true; } };

        if (scope.job_set_id) { add_and(true); sql << "job_set_id=?"; }
        if (scope.program_kind) { add_and(true); sql << "program_kind=?"; }
        if (!scope.states.empty()) {
            add_and(true);
            sql << "state IN (";
            for (size_t i = 0; i < scope.states.size(); ++i) {
                if (i) sql << ',';
                sql << '?';
            }
            sql << ")";
        }
        if (scope.since_queued_at) { add_and(true); sql << "queued_at >= ?"; }
        if (before) {
            add_and(true);
            sql << "(queued_at < ? OR (queued_at = ? AND job_id < ?))";
        }

        sql << " ORDER BY queued_at DESC, job_id DESC LIMIT ?";

        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql.str().c_str(), -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<Page<JobLite>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        }

        int bi = 1;
        if (scope.job_set_id) sqlite3_bind_int64(st, bi++, *scope.job_set_id);
        if (scope.program_kind) sqlite3_bind_int(st, bi++, *scope.program_kind);
        for (auto const& s : scope.states) sqlite3_bind_text(st, bi++, s.c_str(), -1, SQLITE_TRANSIENT);
        if (scope.since_queued_at) sqlite3_bind_int64(st, bi++, *scope.since_queued_at);
        if (before) {
            sqlite3_bind_int64(st, bi++, before->primary);   // queued_at
            sqlite3_bind_int64(st, bi++, before->primary);   // queued_at (tie)
            sqlite3_bind_int64(st, bi++, before->secondary); // job_id
        }
        sqlite3_bind_int(st, bi++, limit);

        Page<JobLite> page{};
        page.items.reserve(static_cast<size_t>(limit));
        while (true) {
            int rc = sqlite3_step(st);
            if (rc == SQLITE_ROW) {
                JobLite r{};
                r.job_id = sqlite3_column_int64(st, 0);
                r.job_set_id = sqlite3_column_int64(st, 1);
                r.program_kind = sqlite3_column_int(st, 2);
                r.state = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
                r.priority = sqlite3_column_int(st, 4);
                r.queued_at = sqlite3_column_int64(st, 5);
                if (sqlite3_column_type(st, 6) != SQLITE_NULL) r.savestate_id = sqlite3_column_int64(st, 6);
                page.items.push_back(std::move(r));
            }
            else if (rc == SQLITE_DONE) {
                break;
            }
            else {
                auto err = DbResult<Page<JobLite>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "step failed" });
                sqlite3_finalize(st);
                return err;
            }
        }

        sqlite3_finalize(st);

        if ((int)page.items.size() == limit) {
            const auto& last = page.items.back();
            page.next = KeysetCursor{ last.queued_at, last.job_id };
        }
        // prev not computed for DESC scans; UI can pass `before` from the first item of previous page if needed.

        return DbResult<Page<JobLite>>::Ok(std::move(page));
    }

    std::future<DbResult<Page<JobLite>>> JobsRepo::ListRecentAsync(
        const JobsListScope& scope,
        std::optional<KeysetCursor> before,
        int limit,
        RetryPolicy rp)
    {
        if (limit <= 0) limit = 50;
        return DBService::instance().submit_res<Page<JobLite>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_list_recent_jobs(e, scope, before, limit); });
    }
    static DbResult<Page<JobLite>> impl_list_recent_jobs_after(
        DbEnv& env,
        const JobsListScope& scope,
        const std::optional<KeysetCursor>& after,
        int limit)
    {
        auto* db = env.handle();
        std::ostringstream sql;
        sql << "SELECT job_id, job_set_id, program_kind, state, priority, queued_at, savestate_id FROM jobs ";

        bool hasWhere = false;
        auto add_and = [&](bool cond) { if (cond) { sql << (hasWhere ? " AND " : " WHERE "); hasWhere = true; } };

        if (scope.job_set_id) { add_and(true); sql << "job_set_id=?"; }
        if (scope.program_kind) { add_and(true); sql << "program_kind=?"; }
        if (!scope.states.empty()) {
            add_and(true);
            sql << "state IN(";
            for (size_t i = 0; i < scope.states.size(); ++i) { if (i) sql << ','; sql << '?'; }
            sql << ")";
        }
        if (scope.since_queued_at) { add_and(true); sql << "queued_at >= ?"; }
        if (after) {
            add_and(true);
            sql << "(queued_at > ? OR (queued_at = ? AND job_id > ?))";
        }

        sql << " ORDER BY queued_at DESC, job_id DESC LIMIT ?";

        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql.str().c_str(), -1, &st, nullptr) != SQLITE_OK) {
            int rc = sqlite3_errcode(db);
            return DbResult<Page<JobLite>>::Err({ map_sqlite_err(rc), rc, sqlite3_errmsg(db) });
        }

        int bind = 1;
        if (scope.job_set_id) sqlite3_bind_int64(st, bind++, *scope.job_set_id);
        if (scope.program_kind) sqlite3_bind_int(st, bind++, *scope.program_kind);
        for (auto& s : scope.states) sqlite3_bind_text(st, bind++, s.c_str(), -1, SQLITE_TRANSIENT);
        if (scope.since_queued_at) sqlite3_bind_int64(st, bind++, *scope.since_queued_at);
        if (after) {
            sqlite3_bind_int64(st, bind++, after->primary);
            sqlite3_bind_int64(st, bind++, after->primary);
            sqlite3_bind_int64(st, bind++, after->secondary);
        }
        sqlite3_bind_int(st, bind++, limit);

        Page<JobLite> page{};
        for (;;) {
            int rc = sqlite3_step(st);
            if (rc == SQLITE_ROW) {
                JobLite r{};
                r.job_id = sqlite3_column_int64(st, 0);
                r.job_set_id = sqlite3_column_int64(st, 1);
                r.program_kind = sqlite3_column_int(st, 2);
                r.state = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
                r.priority = sqlite3_column_int(st, 4);
                r.queued_at = sqlite3_column_int64(st, 5);                
                if (sqlite3_column_type(st, 6) != SQLITE_NULL) r.savestate_id = sqlite3_column_int64(st, 6);
                page.items.push_back(std::move(r));
            }
            else if (rc == SQLITE_DONE) {
                break;
            }
            else {
                int ec = sqlite3_errcode(db);
                sqlite3_finalize(st);
                return DbResult<Page<JobLite>>::Err({ map_sqlite_err(ec), ec, sqlite3_errmsg(db) });
            }
        }
        sqlite3_finalize(st);

        if (!page.items.empty()) {
            const auto& first = page.items.front();
            const auto& last = page.items.back();
            page.prev = KeysetCursor{ first.queued_at, first.job_id };
            page.next = KeysetCursor{ last.queued_at,  last.job_id };
        }
        return DbResult<Page<JobLite>>::Ok(std::move(page));
    }

    std::future<DbResult<Page<JobLite>>> JobsRepo::ListRecentAfterAsync(
        const JobsListScope& scope,
        std::optional<KeysetCursor> after,
        int limit,
        RetryPolicy rp)
    {
        if (limit <= 0) limit = 50;
        return DBService::instance().submit_res<Page<JobLite>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_list_recent_jobs_after(e, scope, after, limit); });
    }

    static inline DbResult<void> impl_requeue(DbEnv& env, int64_t job_id) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st = nullptr;
        int rc = sqlite3_prepare_v2(db, "UPDATE jobs SET state='QUEUED', claimed_by_token=NULL, lease_expires_at=NULL WHERE job_id=?1;", -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare requeue" });
        sqlite3_bind_int64(st, 1, job_id);
        rc = sqlite3_step(st); sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "exec requeue" });
        return DbResult<void>::Ok();
    }
    std::future<DbResult<void>> JobsRepo::RequeueAsync(int64_t job_id, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_requeue(e, job_id); });
    }

    static inline DbResult<void> impl_cancel_if_not_running(DbEnv& env, int64_t job_id) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st = nullptr;
        int rc = sqlite3_prepare_v2(db, "UPDATE jobs SET state='CANCELED', claimed_by_token=NULL, lease_expires_at=NULL WHERE job_id=?1 AND state IN('QUEUED','CLAIMED');", -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare cancel" });
        sqlite3_bind_int64(st, 1, job_id);
        rc = sqlite3_step(st);
        int changes = sqlite3_changes(db);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "exec cancel" });
        if (changes == 0) return DbResult<void>::Err({ DbErrorKind::InvalidState, 0, "cannot cancel a running or terminal job" });
        return DbResult<void>::Ok();
    }
    std::future<DbResult<void>> JobsRepo::CancelIfNotRunningAsync(int64_t job_id, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_cancel_if_not_running(e, job_id); });
    }

    static inline DbResult<void> impl_bump_priority(DbEnv& env, int64_t job_id, int delta) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st = nullptr;
        int rc = sqlite3_prepare_v2(db, "UPDATE jobs SET priority=priority+?2 WHERE job_id=?1;", -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare bump" });
        sqlite3_bind_int64(st, 1, job_id);
        sqlite3_bind_int(st, 2, delta);
        rc = sqlite3_step(st); sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "exec bump" });
        return DbResult<void>::Ok();
    }
    std::future<DbResult<void>> JobsRepo::BumpPriorityAsync(int64_t job_id, int delta, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_bump_priority(e, job_id, delta); });
    }

} // namespace simcore::db
