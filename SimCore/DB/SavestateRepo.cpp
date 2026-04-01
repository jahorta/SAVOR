#include "SavestateRepo.h"
#include <sqlite3.h>

#include "DBCore/ObjectStore.h"

namespace simcore {
    namespace db {

        static inline DbResult<int64_t> Impl_Plan(DbEnv& env, int savestate_type, const std::string& note) {
            sqlite3* db = env.handle();
            sqlite3_stmt* stmt{};
            int rc = sqlite3_prepare_v2(db,
                "INSERT INTO savestate(savestate_type, note, complete) VALUES (?,?,0);",
                -1, &stmt, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int(stmt, 1, savestate_type);
            sqlite3_bind_text(stmt, 2, note.c_str(), -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "insert" });
            return DbResult<int64_t>::Ok(sqlite3_last_insert_rowid(db));
        }

        static inline DbResult<void> Impl_Finalize(DbEnv& env, int64_t id, int64_t object_ref_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* stmt{};
            int rc = sqlite3_prepare_v2(db,
                "UPDATE savestate SET object_ref_id=?, complete=1 WHERE id=?;",
                -1, &stmt, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(stmt, 1, object_ref_id);
            sqlite3_bind_int64(stmt, 2, id);
            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
            return DbResult<void>{ true };
        }


        static inline DbResult<void> Impl_Delete(DbEnv& env, int64_t id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* stmt{};
            int rc = sqlite3_prepare_v2(db,
                "DELETE FROM savestate WHERE id=?;",
                -1, &stmt, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(stmt, 1, id);
            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "delete" });
            return DbResult<void>::Ok();
        }

        static inline DbResult<std::optional<SavestateRow>> Impl_Get(DbEnv& env, int64_t id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* stmt{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id, savestate_type, note, object_ref_id, complete FROM savestate WHERE id=?;",
                -1, &stmt, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::optional<SavestateRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });

            sqlite3_bind_int64(stmt, 1, id);
            rc = sqlite3_step(stmt);
            if (rc != SQLITE_ROW) { sqlite3_finalize(stmt); return DbResult<std::optional<SavestateRow>>::Ok(std::optional<SavestateRow>{}); }

            SavestateRow row;
            row.id = sqlite3_column_int64(stmt, 0);
            row.savestate_type = (SavestateType)sqlite3_column_int(stmt, 1);
            row.note = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            row.object_ref_id = sqlite3_column_int64(stmt, 3);
            row.complete = sqlite3_column_int(stmt, 4) != 0;
            sqlite3_finalize(stmt);

            return DbResult<std::optional<SavestateRow>>::Ok(std::optional<SavestateRow>{ std::move(row) });
        }

        // Async

        std::future<DbResult<int64_t>> SavestateRepo::PlanAsync(int savestate_type, std::string note, RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
                [=, n = std::move(note)](DbEnv& e) { return Impl_Plan(e, savestate_type, n); });
        }

        std::future<DbResult<void>> SavestateRepo::FinalizeAsync(int64_t id, int64_t object_ref_id, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::High, rp,
                [=](DbEnv& e) { return Impl_Finalize(e, id, object_ref_id); });
        }

        std::future<DbResult<std::optional<SavestateRow>>> SavestateRepo::GetAsync(int64_t id, RetryPolicy rp) {
            return DBService::instance().submit_res<std::optional<SavestateRow>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Get(e, id); });
        }

        static inline DbResult<int64_t> Impl_Plan(DbEnv& env, int savestate_type, const std::string& note);
        static inline DbResult<void>    Impl_Finalize(DbEnv& env, int64_t id, int64_t object_ref_id);
        static inline DbResult<std::optional<SavestateRow>> Impl_Get(DbEnv& env, int64_t id);

        static inline DbResult<std::optional<SavestateRow>> Impl_GetByProbeId(DbEnv& env, int64_t probe_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* stmt{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT s.id, s.savestate_type, s.note, s.object_ref_id, s.complete "
                "FROM savestate s JOIN seed_probe p ON p.savestate_id = s.id WHERE p.id=? LIMIT 1;",
                -1, &stmt, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::optional<SavestateRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(stmt, 1, probe_id);
            rc = sqlite3_step(stmt);

            std::optional<SavestateRow> out{};
            if (rc == SQLITE_ROW) {
                SavestateRow r{};
                r.id = sqlite3_column_int64(stmt, 0);
                r.savestate_type = (SavestateType)sqlite3_column_int(stmt, 1);
                r.note = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                r.object_ref_id = sqlite3_column_int64(stmt, 3);
                r.complete = sqlite3_column_int(stmt, 4) != 0;
                out = r;
            }
            sqlite3_finalize(stmt);
            if (rc != SQLITE_ROW && rc != SQLITE_DONE) return DbResult<std::optional<SavestateRow>>::Err({ map_sqlite_err(rc), rc, "scan" });
            return DbResult<std::optional<SavestateRow>>::Ok(out);
        }

        std::future<DbResult<std::optional<SavestateRow>>> SavestateRepo::GetByProbeIdAsync(int64_t probe_id, RetryPolicy rp) {
            return DBService::instance().submit_res<std::optional<SavestateRow>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_GetByProbeId(e, probe_id); });
        }

        static inline void bind_like(sqlite3_stmt* st, int idx, const std::string& s) {
            std::string pat = "%" + s + "%";
            sqlite3_bind_text(st, idx, pat.c_str(), -1, SQLITE_TRANSIENT);
        }
        static inline DbResult<Page<SavestateLite>> Impl_ListSavestate(DbEnv& env, const PagedQuery<>& q, const std::string& search) {
            sqlite3* db = env.handle();
            std::string sql = "SELECT s.id,s.savestate_type,COALESCE(s.note,''),s.object_ref_id,COALESCE(o.filename,''),s.complete "
                "FROM savestate s LEFT JOIN object_ref o ON o.id = s.object_ref_id ";
            std::string where;
            bool has_num = false; int num = 0;
            try { num = std::stoi(search); has_num = true; }
            catch (...) {}
            if (!search.empty()) {
                where += "WHERE (s.note LIKE ? OR o.filename LIKE ?";
                if (has_num) {
                    where += " OR s.savestate_type = ?";
                }
                where += ")";
            }
            std::string keyset; KeysetCursor cur{}; bool has_cursor = false;
            if (q.before) { has_cursor = true; cur = *q.before; keyset = " AND s.id < ? "; }
            if (q.after) { has_cursor = true; cur = *q.after;  keyset = " AND s.id > ? "; }
            if (has_cursor) where += (where.empty() ? (q.after ? "WHERE s.id > ? " : "WHERE s.id < ? ") : keyset);
            std::string order = q.after ? " ORDER BY s.id ASC " : " ORDER BY s.id DESC ";
            sql += where + order + " LIMIT ?;";
            sqlite3_stmt* st{};
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
                return DbResult<Page<SavestateLite>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
            int b = 1;
            if (!search.empty()) { bind_like(st, b++, search); bind_like(st, b++, search); if (has_num) sqlite3_bind_int(st, b++, num); }
            if (has_cursor) sqlite3_bind_int64(st, b++, q.after ? cur.secondary : cur.secondary ? cur.secondary : cur.primary); // id
            sqlite3_bind_int(st, b++, q.limit);
            Page<SavestateLite> page{};
            while (sqlite3_step(st) == SQLITE_ROW) {
                SavestateLite r{};
                r.id = sqlite3_column_int64(st, 0);
                r.savestate_type = sqlite3_column_int(st, 1);
                r.note = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
                if (sqlite3_column_type(st, 3) != SQLITE_NULL) r.object_ref_id = sqlite3_column_int64(st, 3);
                r.filename = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
                r.complete = sqlite3_column_int(st, 5);
                page.items.push_back(std::move(r));
            }
            sqlite3_finalize(st);
            if (q.after && !page.items.empty()) std::reverse(page.items.begin(), page.items.end());
            if (!page.items.empty()) {
                page.prev = KeysetCursor{ page.items.front().id, page.items.front().id };
                page.next = KeysetCursor{ page.items.back().id,  page.items.back().id };
            }
            return DbResult<Page<SavestateLite>>::Ok(std::move(page));
        }
        std::future<DbResult<Page<SavestateLite>>> SavestateRepo::ListPagedAsync(const PagedQuery<>& q, const std::string& search, RetryPolicy rp) {
            return DBService::instance().submit_res<Page<SavestateLite>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ListSavestate(e, q, search); });
        }

        std::future<DbResult<void>> SavestateRepo::DeleteAsync(int64_t id, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::High, rp,
                [=](DbEnv& e) { return Impl_Delete(e, id); });
        }

    } // namespace db
} // namespace simcore
