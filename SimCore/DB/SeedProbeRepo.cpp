#include "SeedProbeRepo.h"
#include <sqlite3.h>

namespace simcore {
    namespace db {

        static inline DbResult<int64_t> Impl_Create(DbEnv& env, int64_t savestate_id, int64_t version_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "INSERT INTO seed_probe(savestate_id, version_id, status, complete) VALUES(?, ?, 'planned', 0);",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, savestate_id);
            sqlite3_bind_int64(st, 2, version_id);
            rc = sqlite3_step(st);
            if (rc != SQLITE_DONE) { sqlite3_finalize(st); return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "insert" }); }
            int64_t id = sqlite3_last_insert_rowid(db);
            sqlite3_finalize(st);
            return DbResult<int64_t>::Ok(id);
        }

        static inline DbResult<void> Impl_MarkRunning(DbEnv& env, int64_t probe_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db, "UPDATE seed_probe SET status='running' WHERE id=? AND status='planned';", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, probe_id);
            rc = sqlite3_step(st); sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
            return DbResult<void>{ true };
        }

        static inline DbResult<void> Impl_SetNeutralSeed(DbEnv& env, int64_t probe_id, int64_t neutral_seed) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db, "UPDATE seed_probe SET neutral_seed=? WHERE id=?;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, neutral_seed);
            sqlite3_bind_int64(st, 2, probe_id);
            rc = sqlite3_step(st); sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
            return DbResult<void>{ true };
        }

        static inline DbResult<void> Impl_MarkDone(DbEnv& env, int64_t probe_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db, "UPDATE seed_probe SET status='done', complete=1 WHERE id=? AND status='running';", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, probe_id);
            rc = sqlite3_step(st); sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
            return DbResult<void>{ true };
        }

        static inline DbResult<SeedProbeRow> Impl_Get(DbEnv& env, int64_t probe_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id, savestate_id, version_id, neutral_seed, status, complete FROM seed_probe WHERE id=? LIMIT 1;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<SeedProbeRow>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, probe_id);
            rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DbResult<SeedProbeRow>::Err({ DbErrorKind::NotFound, SQLITE_DONE, "not found" }); }
            SeedProbeRow r{};
            r.id = sqlite3_column_int64(st, 0);
            r.savestate_id = sqlite3_column_int64(st, 1);
            r.version_id = sqlite3_column_int64(st, 2);
            r.neutral_seed = sqlite3_column_int64(st, 3);
            r.status = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
            r.complete = static_cast<int32_t>(sqlite3_column_int64(st, 5));
            sqlite3_finalize(st);
            return DbResult<SeedProbeRow>::Ok(r);
        }

        static inline DbResult<std::vector<SeedProbeRow>> Impl_ListActiveForSavestate(DbEnv& env, int64_t savestate_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id, savestate_id, version_id, neutral_seed, status, complete FROM seed_probe WHERE savestate_id=? AND status IN('planned','running') ORDER BY id ASC;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::vector<SeedProbeRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, savestate_id);
            std::vector<SeedProbeRow> out;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                SeedProbeRow r{};
                r.id = sqlite3_column_int64(st, 0);
                r.savestate_id = sqlite3_column_int64(st, 1);
                r.version_id = sqlite3_column_int64(st, 2);
                r.neutral_seed = sqlite3_column_int64(st, 3);
                r.status = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
                r.complete = static_cast<int32_t>(sqlite3_column_int64(st, 5));
                out.push_back(r);
            }
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<std::vector<SeedProbeRow>>::Err({ map_sqlite_err(rc), rc, "select" });
            return DbResult<std::vector<SeedProbeRow>>::Ok(std::move(out));
        }

        static inline DbResult<std::vector<SeedProbeRow>> Impl_ListPlanned(DbEnv& env) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id, savestate_id, version_id, neutral_seed, status, complete FROM seed_probe WHERE status='planned' ORDER BY id ASC;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::vector<SeedProbeRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });
            std::vector<SeedProbeRow> out;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                SeedProbeRow r{};
                r.id = sqlite3_column_int64(st, 0);
                r.savestate_id = sqlite3_column_int64(st, 1);
                r.version_id = sqlite3_column_int64(st, 2);
                r.neutral_seed = sqlite3_column_int64(st, 3);
                r.status = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
                r.complete = static_cast<int32_t>(sqlite3_column_int64(st, 5));
                out.push_back(r);
            }
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<std::vector<SeedProbeRow>>::Err({ map_sqlite_err(rc), rc, "select" });
            return DbResult<std::vector<SeedProbeRow>>::Ok(std::move(out));
        }

        // Async via DBService

        std::future<DbResult<int64_t>> SeedProbeRepo::CreateAsync(int64_t savestate_id, int64_t version_id, RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Create(e, savestate_id, version_id); });
        }
        std::future<DbResult<void>> SeedProbeRepo::MarkRunningAsync(int64_t probe_id, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::High, rp,
                [=](DbEnv& e) { return Impl_MarkRunning(e, probe_id); });
        }
        std::future<DbResult<void>> SeedProbeRepo::SetNeutralSeedAsync(int64_t probe_id, int64_t neutral_seed, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_SetNeutralSeed(e, probe_id, neutral_seed); });
        }
        std::future<DbResult<void>> SeedProbeRepo::MarkDoneAsync(int64_t probe_id, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::High, rp,
                [=](DbEnv& e) { return Impl_MarkDone(e, probe_id); });
        }
        std::future<DbResult<SeedProbeRow>> SeedProbeRepo::GetAsync(int64_t probe_id, RetryPolicy rp) {
            return DBService::instance().submit_res<SeedProbeRow>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Get(e, probe_id); });
        }
        std::future<DbResult<std::vector<SeedProbeRow>>> SeedProbeRepo::ListActiveForSavestateAsync(int64_t savestate_id, RetryPolicy rp) {
            return DBService::instance().submit_res<std::vector<SeedProbeRow>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ListActiveForSavestate(e, savestate_id); });
        }
        std::future<DbResult<std::vector<SeedProbeRow>>> SeedProbeRepo::ListPlannedAsync(RetryPolicy rp) {
            return DBService::instance().submit_res<std::vector<SeedProbeRow>>(OpType::Read, Priority::Normal, rp,
                [](DbEnv& e) { return Impl_ListPlanned(e); });
        }

    } // db
} // simcore
