#include "SeedProbeRepo.h"
#include <sqlite3.h>

namespace simcore {
    namespace db {

        static inline DbResult<int64_t> Impl_Create(DbEnv& env, int64_t savestate_id, int64_t codec_version) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "INSERT INTO seed_probe(savestate_id, codec_version, status, complete) VALUES(?, ?, 'planned', 0);",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, savestate_id);
            sqlite3_bind_int64(st, 2, codec_version);
            rc = sqlite3_step(st);
            if (rc != SQLITE_DONE) { sqlite3_finalize(st); std::string errmsg = "insert: " + std::string(sqlite3_errmsg(db)); return DbResult<int64_t>::Err({map_sqlite_err(rc), rc, errmsg.c_str()}); }
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
                "SELECT id, savestate_id, codec_version, neutral_seed, status, complete FROM seed_probe WHERE id=? LIMIT 1;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<SeedProbeRow>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, probe_id);
            rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DbResult<SeedProbeRow>::Err({ DbErrorKind::NotFound, SQLITE_DONE, "not found" }); }
            SeedProbeRow r{};
            r.id = sqlite3_column_int64(st, 0);
            r.savestate_id = sqlite3_column_int64(st, 1);
            r.codec_version = sqlite3_column_int64(st, 2);
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
                "SELECT id, savestate_id, codec_version, neutral_seed, status, complete FROM seed_probe WHERE savestate_id=? AND status IN('planned','running') ORDER BY id ASC;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::vector<SeedProbeRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, savestate_id);
            std::vector<SeedProbeRow> out;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                SeedProbeRow r{};
                r.id = sqlite3_column_int64(st, 0);
                r.savestate_id = sqlite3_column_int64(st, 1);
                r.codec_version = sqlite3_column_int64(st, 2);
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
                "SELECT id, savestate_id, codec_version, neutral_seed, status, complete FROM seed_probe WHERE status='planned' ORDER BY id ASC;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::vector<SeedProbeRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });
            std::vector<SeedProbeRow> out;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                SeedProbeRow r{};
                r.id = sqlite3_column_int64(st, 0);
                r.savestate_id = sqlite3_column_int64(st, 1);
                r.codec_version = sqlite3_column_int64(st, 2);
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

        std::future<DbResult<int64_t>> SeedProbeRepo::CreateAsync(int64_t savestate_id, int64_t codec_version, RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Create(e, savestate_id, codec_version); });
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

        static inline void bind_like(sqlite3_stmt* st, int idx, const std::string& s) {
            std::string pat = "%" + s + "%";
            sqlite3_bind_text(st, idx, pat.c_str(), -1, SQLITE_TRANSIENT);
        }

        static inline DbResult<Page<SeedProbeLite>> Impl_ListSeedProbe(DbEnv& env, const PagedQuery<>& q, const std::string& search, bool only_done, std::optional<int64_t> filter_sid) {
            sqlite3* db = env.handle();
            std::string sql =
                "SELECT p.id,p.savestate_id,p.neutral_seed,p.status,"
                "COALESCE(("
                "  WITH RECURSIVE chain(job_set_id,parent_job_set_id,purpose) AS ("
                "    SELECT js.job_set_id, js.parent_job_set_id, COALESCE(js.purpose,'') "
                "    FROM job_sets js "
                "    JOIN ("
                "      SELECT j.job_set_id "
                "      FROM jobs j "
                "      WHERE j.program_kind=1 AND j.program_ref_id=p.id "
                "      ORDER BY j.queued_at DESC, j.job_id DESC "
                "      LIMIT 1"
                "    ) latest ON latest.job_set_id = js.job_set_id "
                "    UNION ALL "
                "    SELECT parent.job_set_id, parent.parent_job_set_id, COALESCE(parent.purpose,'') "
                "    FROM job_sets parent "
                "    JOIN chain c ON c.parent_job_set_id = parent.job_set_id"
                "  ) "
                "  SELECT c.purpose FROM chain c "
                "  WHERE c.parent_job_set_id IS NULL "
                "     OR NOT EXISTS (SELECT 1 FROM job_sets parent WHERE parent.job_set_id = c.parent_job_set_id) "
                "  LIMIT 1"
                "),''),"
                "p.complete,"
                "EXISTS(SELECT 1 FROM battle_contexts bc WHERE bc.savestate_id = p.savestate_id) "
                "FROM seed_probe p ";
            std::string where;
            if (only_done) { where += (where.empty() ? "WHERE " : " AND "); where += "p.status='done'"; }
            bool s_is_num = false; int64_t s_num = 0;
            try { if (!search.empty()) { s_num = std::stoll(search); s_is_num = true; } }
            catch (...) {}
            if (!search.empty()) { where += (where.empty() ? "WHERE " : " AND "); where += "(p.status LIKE ? OR COALESCE(("
                "  WITH RECURSIVE chain(job_set_id,parent_job_set_id,purpose) AS ("
                "    SELECT js.job_set_id, js.parent_job_set_id, COALESCE(js.purpose,'') "
                "    FROM job_sets js "
                "    JOIN ("
                "      SELECT j.job_set_id FROM jobs j WHERE j.program_kind=1 AND j.program_ref_id=p.id ORDER BY j.queued_at DESC, j.job_id DESC LIMIT 1"
                "    ) latest ON latest.job_set_id = js.job_set_id "
                "    UNION ALL "
                "    SELECT parent.job_set_id, parent.parent_job_set_id, COALESCE(parent.purpose,'') FROM job_sets parent JOIN chain c ON c.parent_job_set_id = parent.job_set_id"
                "  ) SELECT c.purpose FROM chain c WHERE c.parent_job_set_id IS NULL OR NOT EXISTS (SELECT 1 FROM job_sets parent WHERE parent.job_set_id = c.parent_job_set_id) LIMIT 1"
                "),'') LIKE ?";
                if (s_is_num) where += " OR p.savestate_id=?";
                where += ")"; }
            if (filter_sid) { where += (where.empty() ? "WHERE " : " AND "); where += "p.savestate_id=?"; }
            bool has_cursor = false; KeysetCursor cur{};
            if (q.before) { has_cursor = true; cur = *q.before; where += (where.empty() ? "WHERE p.id< ?" : " AND p.id< ?"); }
            if (q.after) { has_cursor = true; cur = *q.after;  where += (where.empty() ? "WHERE p.id> ?" : " AND p.id> ?"); }
            std::string order = q.after ? " ORDER BY p.id ASC " : " ORDER BY p.id DESC ";
            sqlite3_stmt* st{};
            std::string final = sql + where + order + " LIMIT ?;";
            if (sqlite3_prepare_v2(db, final.c_str(), -1, &st, nullptr) != SQLITE_OK)
                return DbResult<Page<SeedProbeLite>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
            int b = 1;
            if (!search.empty()) { bind_like(st, b++, search); bind_like(st, b++, search); if (s_is_num) sqlite3_bind_int64(st, b++, s_num); }
            if (filter_sid) sqlite3_bind_int64(st, b++, *filter_sid);
            if (has_cursor) sqlite3_bind_int64(st, b++, q.after ? cur.secondary : cur.secondary ? cur.secondary : cur.primary);
            sqlite3_bind_int(st, b++, q.limit);
            Page<SeedProbeLite> page{};
            while (sqlite3_step(st) == SQLITE_ROW) {
                SeedProbeLite r{};
                r.id = sqlite3_column_int64(st, 0);
                r.savestate_id = sqlite3_column_int64(st, 1);
                if (sqlite3_column_type(st, 2) != SQLITE_NULL) r.neutral_seed = sqlite3_column_int64(st, 2);
                r.status = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
                r.purpose = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
                r.complete = sqlite3_column_int(st, 5);
                r.has_battle_context = sqlite3_column_int(st, 6) != 0;
                page.items.push_back(std::move(r));
            }
            sqlite3_finalize(st);
            if (q.after && !page.items.empty()) std::reverse(page.items.begin(), page.items.end());
            if (!page.items.empty()) {
                page.prev = KeysetCursor{ page.items.front().id, page.items.front().id };
                page.next = KeysetCursor{ page.items.back().id,  page.items.back().id };
            }
            return DbResult<Page<SeedProbeLite>>::Ok(std::move(page));
        }
        std::future<DbResult<Page<SeedProbeLite>>> SeedProbeRepo::ListPagedAsync(const PagedQuery<>& q, const std::string& search, bool only_done, std::optional<int64_t> filter_sid, RetryPolicy rp) {
            return DBService::instance().submit_res<Page<SeedProbeLite>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ListSeedProbe(e, q, search, only_done, filter_sid); });
        }

    } // db
} // simcore
