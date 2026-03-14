#include "ExplorerSettingsRepo.h"
#include <sqlite3.h>

namespace simcore {
    namespace db {

        static inline DbResult<std::optional<int64_t>> Impl_FindByFingerprint(DbEnv& env, const std::string& fp) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db, "SELECT id FROM explorer_settings WHERE fingerprint=? LIMIT 1;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::optional<int64_t>>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_text(st, 1, fp.c_str(), -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(st);
            if (rc == SQLITE_ROW) { int64_t id = sqlite3_column_int64(st, 0); sqlite3_finalize(st); return DbResult<std::optional<int64_t>>::Ok(id); }
            sqlite3_finalize(st);
            return DbResult<std::optional<int64_t>>::Ok(std::optional<int64_t>{});
        }

        static inline DbResult<int64_t> Impl_EnsureByFingerprint(DbEnv& env, const std::string& name, const std::string& desc, const std::string& fp) {
            auto f = Impl_FindByFingerprint(env, fp);
            if (!f.ok) return DbResult<int64_t>::Err(f.error);
            if (f.value) return DbResult<int64_t>::Ok(*f.value);

            sqlite3* db = env.handle();
            sqlite3_stmt* ins{};
            int rc = sqlite3_prepare_v2(db, "INSERT INTO explorer_settings(name, description, fingerprint) VALUES(?,?,?);", -1, &ins, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_text(ins, 1, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins, 2, desc.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins, 3, fp.c_str(), -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(ins); sqlite3_finalize(ins);
            if (rc != SQLITE_DONE) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "insert" });
            return DbResult<int64_t>::Ok(sqlite3_last_insert_rowid(db));
        }

        static inline DbResult<ExplorerSettingsRow> Impl_Get(DbEnv& env, int64_t id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db, "SELECT id,name,description,fingerprint FROM explorer_settings WHERE id=?;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<ExplorerSettingsRow>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, id);
            rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DbResult<ExplorerSettingsRow>::Err({ DbErrorKind::NotFound, rc, "not found" }); }
            ExplorerSettingsRow r;
            r.id = sqlite3_column_int64(st, 0);
            r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
            r.description = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
            r.fingerprint = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
            sqlite3_finalize(st);
            return DbResult<ExplorerSettingsRow>::Ok(std::move(r));
        }

        // Async surface

        std::future<DbResult<std::optional<int64_t>>> ExplorerSettingsRepo::FindByFingerprintAsync(const std::string& fingerprint, RetryPolicy rp) {
            return DBService::instance().submit_res<std::optional<int64_t>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_FindByFingerprint(e, fingerprint); });
        }

        std::future<DbResult<int64_t>> ExplorerSettingsRepo::EnsureByFingerprintAsync(const std::string& name, const std::string& description, const std::string& fingerprint, RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_EnsureByFingerprint(e, name, description, fingerprint); });
        }

        std::future<DbResult<ExplorerSettingsRow>> ExplorerSettingsRepo::GetAsync(int64_t id, RetryPolicy rp) {
            return DBService::instance().submit_res<ExplorerSettingsRow>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Get(e, id); });
        }

        static inline DbResult<void> Impl_SetSeedProbeId(DbEnv& env, int64_t settings_id, int64_t seed_probe_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db, "UPDATE explorer_settings SET seed_probe_id=? WHERE id=?;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, seed_probe_id);
            sqlite3_bind_int64(st, 2, settings_id);
            rc = sqlite3_step(st); sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
            return DbResult<void>{ true };
        }

        std::future<DbResult<void>> ExplorerSettingsRepo::SetSeedProbeIdAsync(int64_t settings_id, int64_t seed_probe_id, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_SetSeedProbeId(e, settings_id, seed_probe_id); });
        }

        static inline void bind_like(sqlite3_stmt* st, int idx, const std::string& s) {
            std::string pat = "%" + s + "%";
            sqlite3_bind_text(st, idx, pat.c_str(), -1, SQLITE_TRANSIENT);
        }
        static inline DbResult<Page<ExplorerSettingsLite>> Impl_ListES(DbEnv& env, const PagedQuery<>& q, const std::string& search) {
            sqlite3* db = env.handle();
            std::string sql = "SELECT id,COALESCE(name,''),COALESCE(description,'') FROM explorer_settings ";

            std::string where;
            if (!search.empty()) { where += "WHERE (name LIKE ? OR description LIKE ?)"; }

            std::string keyset; KeysetCursor cur{}; bool has = false;
            if (q.before) { has = true; cur = *q.before; where += (where.empty() ? "WHERE " : " AND "); where += "(id < ?)"; }
            if (q.after) { has = true; cur = *q.after;  where += (where.empty() ? "WHERE " : " AND "); where += "(id > ?)"; }

            std::string order = q.after ? " ORDER BY id ASC " : " ORDER BY id DESC ";

            sqlite3_stmt* st{};
            std::string final = sql + where + order + " LIMIT ?;";
            if (sqlite3_prepare_v2(db, final.c_str(), -1, &st, nullptr) != SQLITE_OK)
                return DbResult<Page<ExplorerSettingsLite>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });

            int b = 1;
            if (!search.empty()) { bind_like(st, b++, search); bind_like(st, b++, search); }
            if (q.before) { sqlite3_bind_int64(st, b++, cur.primary); }
            if (q.after) { sqlite3_bind_int64(st, b++, cur.primary); }
            sqlite3_bind_int(st, b++, q.limit);

            Page<ExplorerSettingsLite> page{};
            while (sqlite3_step(st) == SQLITE_ROW) {
                ExplorerSettingsLite r{};
                r.id = sqlite3_column_int64(st, 0);
                r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                r.description = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
                page.items.push_back(std::move(r));
            }
            sqlite3_finalize(st);
            if (q.after && !page.items.empty()) std::reverse(page.items.begin(), page.items.end());
            if (!page.items.empty()) {
                page.prev = KeysetCursor{ page.items.front().id };
                page.next = KeysetCursor{ page.items.back().id };
            }
            return DbResult<Page<ExplorerSettingsLite>>::Ok(std::move(page));
        }

        std::future<DbResult<Page<ExplorerSettingsLite>>> ExplorerSettingsRepo::ListPagedAsync(const PagedQuery<>& q, const std::string& search, RetryPolicy rp) {
            return DBService::instance().submit_res<Page<ExplorerSettingsLite>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ListES(e, q, search); });
        }

    } // db
} // simcore
