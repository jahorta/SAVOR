#include "DebugSessionsRepo.h"

#include <sqlite3.h>

namespace simcore::db {
    namespace {
        static inline std::optional<std::string> col_text_opt(sqlite3_stmt* st, int i) {
            if (sqlite3_column_type(st, i) == SQLITE_NULL) return std::nullopt;
            const unsigned char* t = sqlite3_column_text(st, i);
            return t ? std::make_optional<std::string>(reinterpret_cast<const char*>(t)) : std::nullopt;
        }
        static inline std::optional<int64_t> col_i64_opt(sqlite3_stmt* st, int i) {
            if (sqlite3_column_type(st, i) == SQLITE_NULL) return std::nullopt;
            return sqlite3_column_int64(st, i);
        }

        static bool is_terminal_state(const std::string& s) {
            return s == "SUCCEEDED" || s == "FAILED" || s == "CANCELED" || s == "SUPERSEDED" || s == "SUCCEEDED_WINNER" || s == "SUCCEEDED_DUPLICATE";
        }

        static DbResult<std::optional<DebugSessionRow>> get_by_id_impl(DbEnv& env, int64_t session_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            const char* sql =
                "SELECT id,job_id,state,created_at,updated_at,started_by,worker_id,slot_id,session_token,vm_endpoint,dolphin_endpoint,lock_acquired_at,lock_released_at,failure_code,failure_detail "
                "FROM debug_sessions WHERE id=?1;";
            int rc = sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::optional<DebugSessionRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, session_id);
            rc = sqlite3_step(st);
            if (rc == SQLITE_DONE) {
                sqlite3_finalize(st);
                return DbResult<std::optional<DebugSessionRow>>::Ok(std::nullopt);
            }
            if (rc != SQLITE_ROW) {
                sqlite3_finalize(st);
                return DbResult<std::optional<DebugSessionRow>>::Err({ map_sqlite_err(rc), rc, "select" });
            }

            DebugSessionRow r{};
            r.id = sqlite3_column_int64(st, 0);
            r.job_id = sqlite3_column_int64(st, 1);
            r.state = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
            r.created_at = sqlite3_column_int64(st, 3);
            r.updated_at = sqlite3_column_int64(st, 4);
            r.started_by = col_text_opt(st, 5);
            r.worker_id = col_i64_opt(st, 6);
            r.slot_id = col_i64_opt(st, 7);
            r.session_token = col_text_opt(st, 8);
            r.vm_endpoint = col_text_opt(st, 9);
            r.dolphin_endpoint = col_text_opt(st, 10);
            r.lock_acquired_at = col_i64_opt(st, 11);
            r.lock_released_at = col_i64_opt(st, 12);
            r.failure_code = col_text_opt(st, 13);
            r.failure_detail = col_text_opt(st, 14);
            sqlite3_finalize(st);
            return DbResult<std::optional<DebugSessionRow>>::Ok(r);
        }
    }

    bool DebugSessionsRepo::IsActiveState(const std::string& state) {
        return state == "starting" || state == "launching_worker" || state == "attach_ready" || state == "active" || state == "stopping";
    }

    std::future<DbResult<StartDebugAdmissionResult>> DebugSessionsRepo::StartDebugAsync(int64_t job_id, std::string started_by, int64_t slot_id, RetryPolicy rp) {
        return DBService::instance().submit_res<StartDebugAdmissionResult>(OpType::Write, Priority::High, rp,
            [=](DbEnv& env) -> DbResult<StartDebugAdmissionResult> {
                sqlite3* db = env.handle();
                sqlite3_stmt* st{};
                int rc = sqlite3_exec(db, "SAVEPOINT start_debug;", nullptr, nullptr, nullptr);
                if (rc != SQLITE_OK) return DbResult<StartDebugAdmissionResult>::Err({ map_sqlite_err(rc), rc, "begin" });

                rc = sqlite3_prepare_v2(db, "SELECT state FROM jobs WHERE job_id=?1;", -1, &st, nullptr);
                if (rc != SQLITE_OK) {
                    sqlite3_exec(db, "ROLLBACK TO start_debug;", nullptr, nullptr, nullptr);
                    sqlite3_exec(db, "RELEASE start_debug;", nullptr, nullptr, nullptr);
                    return DbResult<StartDebugAdmissionResult>::Err({ map_sqlite_err(rc), rc, "prepare job lookup" });
                }
                sqlite3_bind_int64(st, 1, job_id);
                rc = sqlite3_step(st);
                if (rc == SQLITE_DONE) {
                    sqlite3_finalize(st);
                    sqlite3_exec(db, "ROLLBACK TO start_debug;", nullptr, nullptr, nullptr);
                    sqlite3_exec(db, "RELEASE start_debug;", nullptr, nullptr, nullptr);
                    return DbResult<StartDebugAdmissionResult>::Err({ DbErrorKind::NotFound, SQLITE_NOTFOUND, "JobNotFound" });
                }
                if (rc != SQLITE_ROW) {
                    sqlite3_finalize(st);
                    sqlite3_exec(db, "ROLLBACK TO start_debug;", nullptr, nullptr, nullptr);
                    sqlite3_exec(db, "RELEASE start_debug;", nullptr, nullptr, nullptr);
                    return DbResult<StartDebugAdmissionResult>::Err({ map_sqlite_err(rc), rc, "select job" });
                }
                const unsigned char* state_text = sqlite3_column_text(st, 0);
                const std::string job_state = state_text ? reinterpret_cast<const char*>(state_text) : std::string();
                sqlite3_finalize(st);
                if (!is_terminal_state(job_state)) {
                    sqlite3_exec(db, "ROLLBACK TO start_debug;", nullptr, nullptr, nullptr);
                    sqlite3_exec(db, "RELEASE start_debug;", nullptr, nullptr, nullptr);
                    return DbResult<StartDebugAdmissionResult>::Err({ DbErrorKind::InvalidArgument, SQLITE_CONSTRAINT, "JobNotTerminal" });
                }

                rc = sqlite3_prepare_v2(db,
                    "SELECT id FROM debug_sessions WHERE job_id=?1 AND state IN ('starting','launching_worker','attach_ready','active','stopping') LIMIT 1;",
                    -1, &st, nullptr);
                if (rc != SQLITE_OK) {
                    sqlite3_exec(db, "ROLLBACK TO start_debug;", nullptr, nullptr, nullptr);
                    sqlite3_exec(db, "RELEASE start_debug;", nullptr, nullptr, nullptr);
                    return DbResult<StartDebugAdmissionResult>::Err({ map_sqlite_err(rc), rc, "prepare active-by-job" });
                }
                sqlite3_bind_int64(st, 1, job_id);
                rc = sqlite3_step(st);
                bool job_busy = (rc == SQLITE_ROW);
                sqlite3_finalize(st);
                if (job_busy) {
                    sqlite3_exec(db, "ROLLBACK TO start_debug;", nullptr, nullptr, nullptr);
                    sqlite3_exec(db, "RELEASE start_debug;", nullptr, nullptr, nullptr);
                    return DbResult<StartDebugAdmissionResult>::Err({ DbErrorKind::Conflict, SQLITE_CONSTRAINT, "DebugSessionAlreadyActiveForJob" });
                }

                rc = sqlite3_prepare_v2(db,
                    "SELECT id FROM debug_sessions WHERE slot_id=?1 AND state IN ('starting','launching_worker','attach_ready','active','stopping') LIMIT 1;",
                    -1, &st, nullptr);
                if (rc != SQLITE_OK) {
                    sqlite3_exec(db, "ROLLBACK TO start_debug;", nullptr, nullptr, nullptr);
                    sqlite3_exec(db, "RELEASE start_debug;", nullptr, nullptr, nullptr);
                    return DbResult<StartDebugAdmissionResult>::Err({ map_sqlite_err(rc), rc, "prepare slot check" });
                }
                sqlite3_bind_int64(st, 1, slot_id);
                rc = sqlite3_step(st);
                bool slot_busy = (rc == SQLITE_ROW);
                sqlite3_finalize(st);
                if (slot_busy) {
                    sqlite3_exec(db, "ROLLBACK TO start_debug;", nullptr, nullptr, nullptr);
                    sqlite3_exec(db, "RELEASE start_debug;", nullptr, nullptr, nullptr);
                    return DbResult<StartDebugAdmissionResult>::Err({ DbErrorKind::Conflict, SQLITE_CONSTRAINT, "DebugSlotBusy" });
                }

                rc = sqlite3_prepare_v2(db,
                    "INSERT INTO debug_sessions(job_id,state,started_by,slot_id,lock_acquired_at,created_at,updated_at) VALUES(?1,'starting',?2,?3,strftime('%s','now'),strftime('%s','now'),strftime('%s','now'));",
                    -1, &st, nullptr);
                if (rc != SQLITE_OK) {
                    sqlite3_exec(db, "ROLLBACK TO start_debug;", nullptr, nullptr, nullptr);
                    sqlite3_exec(db, "RELEASE start_debug;", nullptr, nullptr, nullptr);
                    return DbResult<StartDebugAdmissionResult>::Err({ map_sqlite_err(rc), rc, "prepare insert" });
                }
                sqlite3_bind_int64(st, 1, job_id);
                sqlite3_bind_text(st, 2, started_by.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(st, 3, slot_id);
                rc = sqlite3_step(st);
                sqlite3_finalize(st);
                if (rc != SQLITE_DONE) {
                    sqlite3_exec(db, "ROLLBACK TO start_debug;", nullptr, nullptr, nullptr);
                    sqlite3_exec(db, "RELEASE start_debug;", nullptr, nullptr, nullptr);
                    return DbResult<StartDebugAdmissionResult>::Err({ map_sqlite_err(rc), rc, "insert" });
                }

                rc = sqlite3_exec(db, "RELEASE start_debug;", nullptr, nullptr, nullptr);
                if (rc != SQLITE_OK) return DbResult<StartDebugAdmissionResult>::Err({ map_sqlite_err(rc), rc, "commit" });

                StartDebugAdmissionResult out{};
                out.request_id = sqlite3_last_insert_rowid(db);
                out.initial_status = "QueuedStartup";
                return DbResult<StartDebugAdmissionResult>::Ok(out);
            });
    }


    std::future<DbResult<void>> DebugSessionsRepo::MarkLaunchingAsync(int64_t session_id, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& env) -> DbResult<void> {
                sqlite3* db = env.handle();
                sqlite3_stmt* st{};
                int rc = sqlite3_prepare_v2(db,
                    "UPDATE debug_sessions SET state='launching_worker',updated_at=strftime('%s','now') WHERE id=?1 AND state='starting';",
                    -1, &st, nullptr);
                if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
                sqlite3_bind_int64(st, 1, session_id);
                rc = sqlite3_step(st);
                const int rows = sqlite3_changes(db);
                sqlite3_finalize(st);
                if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
                if (rows <= 0) return DbResult<void>::Err({ DbErrorKind::Conflict, SQLITE_CONSTRAINT, "TooLate" });
                return DbResult<void>::Ok();
            });
    }

std::future<DbResult<void>> DebugSessionsRepo::MarkAttachReadyAsync(int64_t session_id, std::optional<int64_t> worker_id, std::string token, std::string vm_endpoint, std::string dolphin_endpoint, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& env) -> DbResult<void> {
                sqlite3* db = env.handle();
                sqlite3_stmt* st{};
                int rc = sqlite3_prepare_v2(db,
                    "UPDATE debug_sessions SET state='attach_ready',worker_id=?1,session_token=?2,vm_endpoint=?3,dolphin_endpoint=?4,updated_at=strftime('%s','now') WHERE id=?5 AND state='launching_worker';",
                    -1, &st, nullptr);
                if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
                if (worker_id) sqlite3_bind_int64(st, 1, *worker_id); else sqlite3_bind_null(st, 1);
                sqlite3_bind_text(st, 2, token.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 3, vm_endpoint.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 4, dolphin_endpoint.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(st, 5, session_id);
                rc = sqlite3_step(st);
                const int rows = sqlite3_changes(db);
                sqlite3_finalize(st);
                if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
                if (rows <= 0) return DbResult<void>::Err({ DbErrorKind::Conflict, SQLITE_CONSTRAINT, "TooLate" });
                return DbResult<void>::Ok();
            });
    }

    std::future<DbResult<void>> DebugSessionsRepo::MarkActiveAsync(int64_t session_id, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& env) -> DbResult<void> {
                sqlite3* db = env.handle();
                sqlite3_stmt* st{};
                int rc = sqlite3_prepare_v2(db, "UPDATE debug_sessions SET state='active',updated_at=strftime('%s','now') WHERE id=?1 AND state='attach_ready';", -1, &st, nullptr);
                if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
                sqlite3_bind_int64(st, 1, session_id);
                rc = sqlite3_step(st);
                const int rows = sqlite3_changes(db);
                sqlite3_finalize(st);
                if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
                if (rows <= 0) return DbResult<void>::Err({ DbErrorKind::Conflict, SQLITE_CONSTRAINT, "TooLate" });
                return DbResult<void>::Ok();
            });
    }

    std::future<DbResult<void>> DebugSessionsRepo::MarkStoppedAsync(int64_t session_id, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& env) -> DbResult<void> {
                sqlite3* db = env.handle();
                sqlite3_stmt* st{};
                int rc = sqlite3_prepare_v2(db,
                    "UPDATE debug_sessions SET state='stopped',lock_released_at=strftime('%s','now'),updated_at=strftime('%s','now') WHERE id=?1;",
                    -1, &st, nullptr);
                if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
                sqlite3_bind_int64(st, 1, session_id);
                rc = sqlite3_step(st); sqlite3_finalize(st);
                if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
                return DbResult<void>::Ok();
            });
    }

    std::future<DbResult<void>> DebugSessionsRepo::MarkFailedAsync(int64_t session_id, std::string code, std::string detail, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& env) -> DbResult<void> {
                sqlite3* db = env.handle();
                sqlite3_stmt* st{};
                int rc = sqlite3_prepare_v2(db,
                    "UPDATE debug_sessions SET state='failed',failure_code=?1,failure_detail=?2,lock_released_at=strftime('%s','now'),updated_at=strftime('%s','now') WHERE id=?3;",
                    -1, &st, nullptr);
                if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
                sqlite3_bind_text(st, 1, code.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, detail.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(st, 3, session_id);
                rc = sqlite3_step(st); sqlite3_finalize(st);
                if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
                return DbResult<void>::Ok();
            });
    }


    std::future<DbResult<int>> DebugSessionsRepo::CleanupOrphanedActiveSessionsAsync(int64_t max_age_seconds, RetryPolicy rp) {
        return DBService::instance().submit_res<int>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& env) -> DbResult<int> {
                sqlite3* db = env.handle();
                sqlite3_stmt* st{};
                int rc = sqlite3_prepare_v2(db,
                    "UPDATE debug_sessions "
                    "SET state='failed', failure_code='CoordinatorRestartCleanup', failure_detail='Recovered stale debug session', lock_released_at=strftime('%s','now'), updated_at=strftime('%s','now') "
                    "WHERE state IN ('starting','launching_worker','attach_ready','active','stopping') "
                    "AND updated_at <= (strftime('%s','now') - ?1);",
                    -1, &st, nullptr);
                if (rc != SQLITE_OK) return DbResult<int>::Err({ map_sqlite_err(rc), rc, "prepare" });
                sqlite3_bind_int64(st, 1, max_age_seconds);
                rc = sqlite3_step(st);
                const int rows = sqlite3_changes(db);
                sqlite3_finalize(st);
                if (rc != SQLITE_DONE) return DbResult<int>::Err({ map_sqlite_err(rc), rc, "update" });
                return DbResult<int>::Ok(rows);
            });
    }

std::future<DbResult<std::optional<DebugSessionRow>>> DebugSessionsRepo::GetByIdAsync(int64_t session_id, RetryPolicy rp) {
        return DBService::instance().submit_res<std::optional<DebugSessionRow>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& env) { return get_by_id_impl(env, session_id); });
    }

    std::future<DbResult<std::optional<DebugSessionRow>>> DebugSessionsRepo::GetActiveByJobIdAsync(int64_t job_id, RetryPolicy rp) {
        return DBService::instance().submit_res<std::optional<DebugSessionRow>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& env) -> DbResult<std::optional<DebugSessionRow>> {
                sqlite3* db = env.handle();
                sqlite3_stmt* st{};
                int rc = sqlite3_prepare_v2(db,
                    "SELECT id FROM debug_sessions WHERE job_id=?1 AND state IN ('starting','launching_worker','attach_ready','active','stopping') ORDER BY id DESC LIMIT 1;",
                    -1, &st, nullptr);
                if (rc != SQLITE_OK) return DbResult<std::optional<DebugSessionRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });
                sqlite3_bind_int64(st, 1, job_id);
                rc = sqlite3_step(st);
                if (rc == SQLITE_DONE) {
                    sqlite3_finalize(st);
                    return DbResult<std::optional<DebugSessionRow>>::Ok(std::nullopt);
                }
                if (rc != SQLITE_ROW) {
                    sqlite3_finalize(st);
                    return DbResult<std::optional<DebugSessionRow>>::Err({ map_sqlite_err(rc), rc, "select" });
                }
                const int64_t id = sqlite3_column_int64(st, 0);
                sqlite3_finalize(st);
                return get_by_id_impl(env, id);
            });
    }

    std::future<DbResult<std::vector<DebugSessionRow>>> DebugSessionsRepo::ListRecentAsync(int limit, RetryPolicy rp) {
        return DBService::instance().submit_res<std::vector<DebugSessionRow>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& env) -> DbResult<std::vector<DebugSessionRow>> {
                sqlite3* db = env.handle();
                sqlite3_stmt* st{};
                int rc = sqlite3_prepare_v2(db,
                    "SELECT id,job_id,state,created_at,updated_at,started_by,worker_id,slot_id,session_token,vm_endpoint,dolphin_endpoint,lock_acquired_at,lock_released_at,failure_code,failure_detail "
                    "FROM debug_sessions ORDER BY id DESC LIMIT ?1;",
                    -1, &st, nullptr);
                if (rc != SQLITE_OK) return DbResult<std::vector<DebugSessionRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });
                sqlite3_bind_int(st, 1, limit);
                std::vector<DebugSessionRow> out;
                while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                    DebugSessionRow r{};
                    r.id = sqlite3_column_int64(st, 0);
                    r.job_id = sqlite3_column_int64(st, 1);
                    r.state = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
                    r.created_at = sqlite3_column_int64(st, 3);
                    r.updated_at = sqlite3_column_int64(st, 4);
                    r.started_by = col_text_opt(st, 5);
                    r.worker_id = col_i64_opt(st, 6);
                    r.slot_id = col_i64_opt(st, 7);
                    r.session_token = col_text_opt(st, 8);
                    r.vm_endpoint = col_text_opt(st, 9);
                    r.dolphin_endpoint = col_text_opt(st, 10);
                    r.lock_acquired_at = col_i64_opt(st, 11);
                    r.lock_released_at = col_i64_opt(st, 12);
                    r.failure_code = col_text_opt(st, 13);
                    r.failure_detail = col_text_opt(st, 14);
                    out.push_back(std::move(r));
                }
                sqlite3_finalize(st);
                if (rc != SQLITE_DONE) return DbResult<std::vector<DebugSessionRow>>::Err({ map_sqlite_err(rc), rc, "select" });
                return DbResult<std::vector<DebugSessionRow>>::Ok(std::move(out));
            });
    }
}
