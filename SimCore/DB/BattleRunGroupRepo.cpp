#include "BattleRunGroupRepo.h"
#include <sqlite3.h>

namespace simcore {
    namespace db {

        static inline DbResult<int64_t> Impl_Create(DbEnv& env,
            int64_t settings_id, int64_t seed_probe_id,
            const std::string& name, const std::string& desc,
            const std::optional<std::string>& pred_fp,
            const std::optional<std::string>& plan_fp) {

            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "INSERT INTO battle_run_groups(settings_id,seed_probe_id,name,description,predicate_vector_fingerprint,plan_vector_fingerprint)"
                " VALUES(?,?,?,?,?,?);",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare" });

            sqlite3_bind_int64(st, 1, settings_id);
            sqlite3_bind_int64(st, 2, seed_probe_id);
            if (!name.empty()) sqlite3_bind_text(st, 3, name.c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 3);
            if (!desc.empty()) sqlite3_bind_text(st, 4, desc.c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 4);
            if (pred_fp && !pred_fp->empty()) sqlite3_bind_text(st, 5, pred_fp->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 5);
            if (plan_fp && !plan_fp->empty()) sqlite3_bind_text(st, 6, plan_fp->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 6);

            rc = sqlite3_step(st);
            if (rc != SQLITE_DONE) { sqlite3_finalize(st); return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "insert" }); }
            int64_t id = sqlite3_last_insert_rowid(db);
            sqlite3_finalize(st);
            return DbResult<int64_t>::Ok(id);
        }

        static inline DbResult<BattleRunGroupRow> Impl_Get(DbEnv& env, int64_t group_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT group_id,settings_id,seed_probe_id,name,description,predicate_vector_fingerprint,plan_vector_fingerprint,created_at "
                "FROM battle_run_groups WHERE group_id=? LIMIT 1;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<BattleRunGroupRow>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, group_id);
            rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DbResult<BattleRunGroupRow>::Err({ DbErrorKind::NotFound, rc, "not found" }); }

            BattleRunGroupRow r{};
            r.group_id = sqlite3_column_int64(st, 0);
            r.settings_id = sqlite3_column_int64(st, 1);
            r.seed_probe_id = sqlite3_column_int64(st, 2);
            if (sqlite3_column_type(st, 3) != SQLITE_NULL) r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
            if (sqlite3_column_type(st, 4) != SQLITE_NULL) r.description = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
            if (sqlite3_column_type(st, 5) != SQLITE_NULL) r.predicate_vector_fingerprint = std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 5)));
            if (sqlite3_column_type(st, 6) != SQLITE_NULL) r.plan_vector_fingerprint = std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 6)));
            r.created_at = sqlite3_column_int64(st, 7);
            sqlite3_finalize(st);
            return DbResult<BattleRunGroupRow>::Ok(std::move(r));
        }

        static inline DbResult<std::vector<int64_t>> Impl_ListRunIds(DbEnv& env, int64_t group_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id FROM explorer_run WHERE group_id=? ORDER BY id;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::vector<int64_t>>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, group_id);

            std::vector<int64_t> out;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) out.push_back(sqlite3_column_int64(st, 0));
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<std::vector<int64_t>>::Err({ map_sqlite_err(rc), rc, "scan" });
            return DbResult<std::vector<int64_t>>::Ok(std::move(out));
        }

        static inline DbResult<bool> Impl_IsComplete(DbEnv& env, int64_t group_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT COUNT(*) FROM explorer_run WHERE group_id=? AND status!='done';",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<bool>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, group_id);
            rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DbResult<bool>::Err({ map_sqlite_err(rc), rc, "count" }); }
            int64_t remaining = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
            return DbResult<bool>::Ok(remaining == 0);
        }

        std::future<DbResult<int64_t>> BattleRunGroupRepo::CreateAsync(
            int64_t settings_id, int64_t seed_probe_id,
            std::string name, std::string description,
            std::optional<std::string> predicate_vec_fp, std::optional<std::string> plan_vec_fp,
            RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Create(e, settings_id, seed_probe_id, name, description, predicate_vec_fp, plan_vec_fp); });
        }

        std::future<DbResult<BattleRunGroupRow>> BattleRunGroupRepo::GetAsync(int64_t group_id, RetryPolicy rp) {
            return DBService::instance().submit_res<BattleRunGroupRow>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Get(e, group_id); });
        }

        std::future<DbResult<std::vector<int64_t>>> BattleRunGroupRepo::ListRunIdsAsync(int64_t group_id, RetryPolicy rp) {
            return DBService::instance().submit_res<std::vector<int64_t>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ListRunIds(e, group_id); });
        }

        std::future<DbResult<bool>> BattleRunGroupRepo::IsCompleteAsync(int64_t group_id, RetryPolicy rp) {
            return DBService::instance().submit_res<bool>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_IsComplete(e, group_id); });
        }

        static inline void bind_like(sqlite3_stmt* st, int idx, const std::string& s) {
            std::string pat = "%" + s + "%";
            sqlite3_bind_text(st, idx, pat.c_str(), -1, SQLITE_TRANSIENT);
        }
        static inline DbResult<Page<BattleRunGroupLite>> Impl_ListBRG(DbEnv& env, const PagedQuery<>& q, const std::string& search) {
            sqlite3* db = env.handle();
            std::string sql = "SELECT group_id,settings_id,seed_probe_id,COALESCE(name,''),COALESCE(description,''),created_at FROM battle_run_groups ";
            std::string where;
            if (!search.empty()) { where += "WHERE (name LIKE ? OR description LIKE ?)"; }
            std::string keyset; KeysetCursor cur{}; bool has = false;
            if (q.before) { has = true; cur = *q.before; where += (where.empty() ? "WHERE " : " AND "); where += "(created_at < ? OR (created_at = ? AND group_id < ?))"; }
            if (q.after) { has = true; cur = *q.after;  where += (where.empty() ? "WHERE " : " AND "); where += "(created_at > ? OR (created_at = ? AND group_id > ?))"; }
            std::string order = q.after ? " ORDER BY created_at ASC, group_id ASC " : " ORDER BY created_at DESC, group_id DESC ";
            sqlite3_stmt* st{};
            std::string final = sql + where + order + " LIMIT ?;";
            if (sqlite3_prepare_v2(db, final.c_str(), -1, &st, nullptr) != SQLITE_OK)
                return DbResult<Page<BattleRunGroupLite>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
            int b = 1;
            if (!search.empty()) { bind_like(st, b++, search); bind_like(st, b++, search); }
            if (q.before) { sqlite3_bind_int64(st, b++, cur.primary); sqlite3_bind_int64(st, b++, cur.primary); sqlite3_bind_int64(st, b++, cur.secondary); }
            if (q.after) { sqlite3_bind_int64(st, b++, cur.primary); sqlite3_bind_int64(st, b++, cur.primary); sqlite3_bind_int64(st, b++, cur.secondary); }
            sqlite3_bind_int(st, b++, q.limit);
            Page<BattleRunGroupLite> page{};
            while (sqlite3_step(st) == SQLITE_ROW) {
                BattleRunGroupLite r{};
                r.group_id = sqlite3_column_int64(st, 0);
                r.settings_id = sqlite3_column_int64(st, 1);
                r.seed_probe_id = sqlite3_column_int64(st, 2);
                r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
                r.description = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
                r.created_at = sqlite3_column_int64(st, 5);
                page.items.push_back(std::move(r));
            }
            sqlite3_finalize(st);
            if (q.after && !page.items.empty()) std::reverse(page.items.begin(), page.items.end());
            if (!page.items.empty()) {
                page.prev = KeysetCursor{ page.items.front().created_at, page.items.front().group_id };
                page.next = KeysetCursor{ page.items.back().created_at,  page.items.back().group_id };
            }
            return DbResult<Page<BattleRunGroupLite>>::Ok(std::move(page));
        }
        std::future<DbResult<Page<BattleRunGroupLite>>> BattleRunGroupRepo::ListPagedAsync(const PagedQuery<>& q, const std::string& search, RetryPolicy rp) {
            return DBService::instance().submit_res<Page<BattleRunGroupLite>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ListBRG(e, q, search); });
        }
    }
} // namespace
