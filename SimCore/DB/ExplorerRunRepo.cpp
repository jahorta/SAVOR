#include "ExplorerRunRepo.h"
#include <sqlite3.h>

namespace simcore {
    namespace db {

        static inline DbResult<int64_t> Impl_IdempotentCreate(DbEnv& env, int64_t probe_id, int64_t settings_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id FROM explorer_run WHERE probe_id=? AND settings_id=? AND status='planned' LIMIT 1;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, probe_id);
            sqlite3_bind_int64(st, 2, settings_id);
            rc = sqlite3_step(st);
            if (rc == SQLITE_ROW) { int64_t id = sqlite3_column_int64(st, 0); sqlite3_finalize(st); return DbResult<int64_t>::Ok(id); }
            sqlite3_finalize(st);

            rc = sqlite3_prepare_v2(db,
                "INSERT INTO explorer_run(probe_id, settings_id, status, progress_log, complete) VALUES(?, ?, 'planned', '', 0);",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, probe_id);
            sqlite3_bind_int64(st, 2, settings_id);
            rc = sqlite3_step(st);
            if (rc != SQLITE_DONE) { sqlite3_finalize(st); return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "insert" }); }
            int64_t id = sqlite3_last_insert_rowid(db);
            sqlite3_finalize(st);
            return DbResult<int64_t>::Ok(id);
        }

        static inline DbResult<void> Impl_AppendProgress(DbEnv& env, int64_t run_id, const std::string& text, int max_bytes) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "UPDATE explorer_run SET progress_log=substr(progress_log || ?, -?), complete=0 WHERE id=?;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_text(st, 1, text.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 2, max_bytes);
            sqlite3_bind_int64(st, 3, run_id);
            rc = sqlite3_step(st); sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
            return DbResult<void>{ true };
        }

        static inline DbResult<void> Impl_MarkRunning(DbEnv& env, int64_t run_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db, "UPDATE explorer_run SET status='running' WHERE id=? AND status='planned';", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, run_id);
            rc = sqlite3_step(st); sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
            return DbResult<void>{ true };
        }

        static inline DbResult<void> Impl_MarkDone(DbEnv& env, int64_t run_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db, "UPDATE explorer_run SET status='done', complete=1 WHERE id=?;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, run_id);
            rc = sqlite3_step(st); sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
            return DbResult<void>{ true };
        }

        static inline DbResult<ExplorerRunRow> Impl_Get(DbEnv& env, int64_t run_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id, probe_id, settings_id, status, progress_log, complete FROM explorer_run WHERE id=? LIMIT 1;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<ExplorerRunRow>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, run_id);
            rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DbResult<ExplorerRunRow>::Err({ DbErrorKind::NotFound, SQLITE_DONE, "not found" }); }
            ExplorerRunRow r{};
            r.id = sqlite3_column_int64(st, 0);
            r.probe_id = sqlite3_column_int64(st, 1);
            r.settings_id = sqlite3_column_int64(st, 2);
            r.status = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
            r.progress_log = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
            r.complete = static_cast<int32_t>(sqlite3_column_int64(st, 5));
            sqlite3_finalize(st);
            return DbResult<ExplorerRunRow>::Ok(r);
        }

        static inline DbResult<std::vector<ExplorerRunRow>> Impl_ListActiveForProbe(DbEnv& env, int64_t probe_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id, probe_id, settings_id, status, progress_log, complete FROM explorer_run WHERE probe_id=? AND status IN('planned','running') ORDER BY id ASC;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::vector<ExplorerRunRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, probe_id);
            std::vector<ExplorerRunRow> out;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                ExplorerRunRow r{};
                r.id = sqlite3_column_int64(st, 0);
                r.probe_id = sqlite3_column_int64(st, 1);
                r.settings_id = sqlite3_column_int64(st, 2);
                r.status = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
                r.progress_log = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
                r.complete = static_cast<int32_t>(sqlite3_column_int64(st, 5));
                out.push_back(r);
            }
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<std::vector<ExplorerRunRow>>::Err({ map_sqlite_err(rc), rc, "select" });
            return DbResult<std::vector<ExplorerRunRow>>::Ok(std::move(out));
        }

        // Async via DBService

        std::future<DbResult<int64_t>> ExplorerRunRepo::IdempotentCreateAsync(int64_t probe_id, int64_t settings_id, RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_IdempotentCreate(e, probe_id, settings_id); });
        }

        std::future<DbResult<void>> ExplorerRunRepo::AppendProgressAsync(int64_t run_id, std::string text, int max_bytes, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
                [run_id, text = std::move(text), max_bytes](DbEnv& e) { return Impl_AppendProgress(e, run_id, text, max_bytes); });
        }

        std::future<DbResult<void>> ExplorerRunRepo::MarkRunningAsync(int64_t run_id, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::High, rp,
                [=](DbEnv& e) { return Impl_MarkRunning(e, run_id); });
        }

        std::future<DbResult<void>> ExplorerRunRepo::MarkDoneAsync(int64_t run_id, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::High, rp,
                [=](DbEnv& e) { return Impl_MarkDone(e, run_id); });
        }

        std::future<DbResult<ExplorerRunRow>> ExplorerRunRepo::GetAsync(int64_t run_id, RetryPolicy rp) {
            return DBService::instance().submit_res<ExplorerRunRow>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Get(e, run_id); });
        }

        std::future<DbResult<std::vector<ExplorerRunRow>>> ExplorerRunRepo::ListActiveForProbeAsync(int64_t probe_id, RetryPolicy rp) {
            return DBService::instance().submit_res<std::vector<ExplorerRunRow>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ListActiveForProbe(e, probe_id); });
        }

        static inline DbResult<void> Impl_SetResultsIni(DbEnv& env, int64_t run_id, const std::string& ini_text) {
            auto* db = env.handle();
            sqlite3_stmt* st{};
            if (sqlite3_prepare_v2(db, "UPDATE explorer_run SET results_ini=? WHERE id=?;", -1, &st, nullptr) != SQLITE_OK)
                return DbResult<void>::Err(sqlite3_errmsg(db));
            sqlite3_bind_text(st, 1, ini_text.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, run_id);
            int rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err(sqlite3_errmsg(db));
            return DbResult<void>::Ok();
        }

        static inline DbResult<void> Impl_SetProgressLogArtifactId(DbEnv& env, int64_t run_id, int64_t artifact_id) {
            auto* db = env.handle();
            sqlite3_stmt* st{};
            if (sqlite3_prepare_v2(db, "UPDATE explorer_run SET progress_log_artifact_id=? WHERE id=?;", -1, &st, nullptr) != SQLITE_OK)
                return DbResult<void>::Err(sqlite3_errmsg(db));
            sqlite3_bind_int64(st, 1, artifact_id);
            sqlite3_bind_int64(st, 2, run_id);
            int rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err(sqlite3_errmsg(db));
            return DbResult<void>::Ok();
        }

        std::future<DbResult<void>> ExplorerRunRepo::SetResultsIniAsync(int64_t run_id, std::string ini_text, RetryPolicy rp) {
            return DBService::instance().submit_void(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_SetResultsIni(e, run_id, ini_text); });
        }
        std::future<DbResult<void>> ExplorerRunRepo::SetProgressLogArtifactIdAsync(int64_t run_id, int64_t artifact_id, RetryPolicy rp) {
            return DBService::instance().submit_void(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_SetProgressLogArtifactId(e, run_id, artifact_id); });
        }

    } // db
} // simcore
