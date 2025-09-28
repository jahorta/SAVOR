#include "Query.h"
#include <sqlite3.h>

namespace simcore {
    namespace db {

        DbResult<SeedProbeRow> QueryRepo::DequeueProbe(DbEnv& env) {
            sqlite3* db = env.handle();
            char* err = nullptr;
            int rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, &err);
            if (rc != SQLITE_OK) { std::string m = err ? err : "begin"; sqlite3_free(err); return DbResult<SeedProbeRow>::Err({ map_sqlite_err(rc), rc, m }); }

            sqlite3_stmt* sel{};
            rc = sqlite3_prepare_v2(db, "SELECT id FROM seed_probe WHERE status='planned' ORDER BY id LIMIT 1;", -1, &sel, nullptr);
            if (rc != SQLITE_OK) { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); return DbResult<SeedProbeRow>::Err({ map_sqlite_err(rc), rc, "prepare sel" }); }
            rc = sqlite3_step(sel);
            if (rc != SQLITE_ROW) { sqlite3_finalize(sel); sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); return DbResult<SeedProbeRow>::Err({ DbErrorKind::Unknown, rc, "empty" }); }
            int64_t id = sqlite3_column_int64(sel, 0);
            sqlite3_finalize(sel);

            sqlite3_stmt* upd{};
            rc = sqlite3_prepare_v2(db, "UPDATE seed_probe SET status='running' WHERE id=? AND status='planned';", -1, &upd, nullptr);
            if (rc != SQLITE_OK) { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); return DbResult<SeedProbeRow>::Err({ map_sqlite_err(rc), rc, "prepare upd" }); }
            sqlite3_bind_int64(upd, 1, id);
            rc = sqlite3_step(upd);
            sqlite3_finalize(upd);
            if (rc != SQLITE_DONE) { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); return DbResult<SeedProbeRow>::Err({ map_sqlite_err(rc), rc, "claim" }); }

            rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &err);
            if (rc != SQLITE_OK) { std::string m = err ? err : "commit"; sqlite3_free(err); return DbResult<SeedProbeRow>::Err({ map_sqlite_err(rc), rc, m }); }

            return SeedProbeRepo::Get(id);
        }

        DbResult<ExplorerRunRow> QueryRepo::DequeueRun(DbEnv& env) {
            sqlite3* db = env.handle();
            char* err = nullptr;
            int rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, &err);
            if (rc != SQLITE_OK) { std::string m = err ? err : "begin"; sqlite3_free(err); return DbResult<ExplorerRunRow>::Err({ map_sqlite_err(rc), rc, m }); }

            sqlite3_stmt* sel{};
            rc = sqlite3_prepare_v2(db, "SELECT id FROM explorer_run WHERE status='planned' ORDER BY id LIMIT 1;", -1, &sel, nullptr);
            if (rc != SQLITE_OK) { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); return DbResult<ExplorerRunRow>::Err({ map_sqlite_err(rc), rc, "prepare sel" }); }
            rc = sqlite3_step(sel);
            if (rc != SQLITE_ROW) { sqlite3_finalize(sel); sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); return DbResult<ExplorerRunRow>::Err({ DbErrorKind::Unknown, rc, "empty" }); }
            int64_t id = sqlite3_column_int64(sel, 0);
            sqlite3_finalize(sel);

            sqlite3_stmt* upd{};
            rc = sqlite3_prepare_v2(db, "UPDATE explorer_run SET status='running' WHERE id=? AND status='planned';", -1, &upd, nullptr);
            if (rc != SQLITE_OK) { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); return DbResult<ExplorerRunRow>::Err({ map_sqlite_err(rc), rc, "prepare upd" }); }
            sqlite3_bind_int64(upd, 1, id);
            rc = sqlite3_step(upd);
            sqlite3_finalize(upd);
            if (rc != SQLITE_DONE) { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); return DbResult<ExplorerRunRow>::Err({ map_sqlite_err(rc), rc, "claim" }); }

            rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &err);
            if (rc != SQLITE_OK) { std::string m = err ? err : "commit"; sqlite3_free(err); return DbResult<ExplorerRunRow>::Err({ map_sqlite_err(rc), rc, m }); }

            return ExplorerRunRepo::Get(id);
        }

        DbResult<QueueDepths> QueryRepo::QueueDepthsNow(DbEnv& env) {
            sqlite3* db = env.handle();
            QueueDepths q{};

            sqlite3_stmt* st1{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT "
                "SUM(CASE WHEN status='planned' THEN 1 ELSE 0 END),"
                "SUM(CASE WHEN status='running' THEN 1 ELSE 0 END) "
                "FROM seed_probe;", -1, &st1, nullptr);
            if (rc != SQLITE_OK) return DbResult<QueueDepths>::Err({ map_sqlite_err(rc), rc, "prepare q1" });
            rc = sqlite3_step(st1);
            if (rc == SQLITE_ROW) {
                q.probes_planned = sqlite3_column_int64(st1, 0);
                q.probes_running = sqlite3_column_int64(st1, 1);
            }
            sqlite3_finalize(st1);

            sqlite3_stmt* st2{};
            rc = sqlite3_prepare_v2(db,
                "SELECT "
                "SUM(CASE WHEN status='planned' THEN 1 ELSE 0 END),"
                "SUM(CASE WHEN status='running' THEN 1 ELSE 0 END) "
                "FROM explorer_run;", -1, &st2, nullptr);
            if (rc != SQLITE_OK) return DbResult<QueueDepths>::Err({ map_sqlite_err(rc), rc, "prepare q2" });
            rc = sqlite3_step(st2);
            if (rc == SQLITE_ROW) {
                q.runs_planned = sqlite3_column_int64(st2, 0);
                q.runs_running = sqlite3_column_int64(st2, 1);
            }
            sqlite3_finalize(st2);

            return DbResult<QueueDepths>::Ok(q);
        }

        DbResult<LineageRow> QueryRepo::GetLineage(DbEnv& env, int64_t run_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT run_id, probe_id, settings_id, savestate_id FROM v_lineage WHERE run_id=?;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<LineageRow>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, run_id);
            rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DbResult<LineageRow>::Err({ DbErrorKind::Unknown, rc, "not found" }); }
            LineageRow row;
            row.run_id = sqlite3_column_int64(st, 0);
            row.probe_id = sqlite3_column_int64(st, 1);
            row.settings_id = sqlite3_column_int64(st, 2);
            row.savestate_id = sqlite3_column_int64(st, 3);
            sqlite3_finalize(st);
            return DbResult<LineageRow>::Ok(row);
        }

        DbResult<TasMovieRow> QueryRepo::DequeueTasMovie(DbEnv& env) {
            sqlite3* db = env.handle();
            char* err = nullptr;
            int rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, &err);
            if (rc != SQLITE_OK) { std::string m = err ? err : "begin"; sqlite3_free(err); return DbResult<TasMovieRow>::Err({ map_sqlite_err(rc), rc, m }); }

            sqlite3_stmt* sel{};
            rc = sqlite3_prepare_v2(db,
                "SELECT id FROM tas_movie WHERE status='planned' ORDER BY priority DESC, id LIMIT 1;",
                -1, &sel, nullptr);
            if (rc != SQLITE_OK) { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); return DbResult<TasMovieRow>::Err({ map_sqlite_err(rc), rc, "prepare sel" }); }
            rc = sqlite3_step(sel);
            if (rc != SQLITE_ROW) { sqlite3_finalize(sel); sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); return DbResult<TasMovieRow>::Err({ DbErrorKind::Unknown, rc, "empty" }); }
            int64_t id = sqlite3_column_int64(sel, 0);
            sqlite3_finalize(sel);

            sqlite3_stmt* upd{};
            rc = sqlite3_prepare_v2(db,
                "UPDATE tas_movie SET status='running', started_at=strftime('%s','now'), attempt_count=attempt_count+1 "
                "WHERE id=? AND status='planned';", -1, &upd, nullptr);
            if (rc != SQLITE_OK) { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); return DbResult<TasMovieRow>::Err({ map_sqlite_err(rc), rc, "prepare upd" }); }
            sqlite3_bind_int64(upd, 1, id);
            rc = sqlite3_step(upd);
            sqlite3_finalize(upd);
            if (rc != SQLITE_DONE) { sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); return DbResult<TasMovieRow>::Err({ map_sqlite_err(rc), rc, "claim" }); }

            rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &err);
            if (rc != SQLITE_OK) { std::string m = err ? err : "commit"; sqlite3_free(err); return DbResult<TasMovieRow>::Err({ map_sqlite_err(rc), rc, m }); }

            return TasMovieRepo::Get(id);
        }

        DbResult<TasMovieQueueDepths> QueryRepo::TasMovieQueueDepthsNow(DbEnv& env) {
            sqlite3* db = env.handle();
            TasMovieQueueDepths q{};
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT "
                "SUM(CASE WHEN status='planned' THEN 1 ELSE 0 END), "
                "SUM(CASE WHEN status='running' THEN 1 ELSE 0 END) "
                "FROM tas_movie;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<TasMovieQueueDepths>::Err({ map_sqlite_err(rc), rc, "prepare" });
            rc = sqlite3_step(st);
            if (rc == SQLITE_ROW) {
                q.planned = sqlite3_column_int64(st, 0);
                q.running = sqlite3_column_int64(st, 1);
            }
            sqlite3_finalize(st);
            return DbResult<TasMovieQueueDepths>::Ok(q);
        }
    }
} // namespace simcore::db
