#include "ExplorerSettingsPlanLinkRepo.h"
#include <sqlite3.h>

namespace simcore {
    namespace db {

        static inline DbResult<void> Impl_Clear(DbEnv& env, int64_t settings_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db, "DELETE FROM explorer_settings_plan_link WHERE settings_id=?;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare del" });
            sqlite3_bind_int64(st, 1, settings_id);
            rc = sqlite3_step(st); sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "delete" });
            return DbResult<void>{ true };
        }

        static inline DbResult<void> Impl_ReplaceAll(DbEnv& env, int64_t settings_id, const std::vector<int64_t>& plan_ids) {
            auto d = Impl_Clear(env, settings_id);
            if (!d.ok) return d;

            sqlite3* db = env.handle();
            sqlite3_stmt* ins{};
            int rc = sqlite3_prepare_v2(db,
                "INSERT INTO explorer_settings_plan_link(settings_id, ordinal, plan_id) VALUES(?,?,?);",
                -1, &ins, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare ins" });

            for (int i = 0; i < (int)plan_ids.size(); ++i) {
                sqlite3_reset(ins); sqlite3_clear_bindings(ins);
                sqlite3_bind_int64(ins, 1, settings_id);
                sqlite3_bind_int(ins, 2, i);
                sqlite3_bind_int64(ins, 3, plan_ids[i]);
                rc = sqlite3_step(ins);
                if (rc != SQLITE_DONE) { sqlite3_finalize(ins); return DbResult<void>::Err({ map_sqlite_err(rc), rc, "insert" }); }
            }
            sqlite3_finalize(ins);
            return DbResult<void>{ true };
        }

        static inline DbResult<std::vector<SettingsPlanLinkRow>> Impl_List(DbEnv& env, int64_t settings_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT settings_id, ordinal, plan_id FROM explorer_settings_plan_link WHERE settings_id=? ORDER BY ordinal;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::vector<SettingsPlanLinkRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, settings_id);

            std::vector<SettingsPlanLinkRow> out;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                SettingsPlanLinkRow r{};
                r.settings_id = sqlite3_column_int64(st, 0);
                r.ordinal = sqlite3_column_int(st, 1);
                r.plan_id = sqlite3_column_int64(st, 2);
                out.push_back(r);
            }
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<std::vector<SettingsPlanLinkRow>>::Err({ map_sqlite_err(rc), rc, "scan" });
            return DbResult<std::vector<SettingsPlanLinkRow>>::Ok(std::move(out));
        }

        static inline DbResult<void> Impl_Append(DbEnv& env, int64_t settings_id, int64_t plan_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "INSERT INTO explorer_settings_plan_link(settings_id, ordinal, plan_id)"
                " VALUES(?, COALESCE((SELECT MAX(ordinal)+1 FROM explorer_settings_plan_link WHERE settings_id=?), 0), ?);",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare ins" });
            sqlite3_bind_int64(st, 1, settings_id);
            sqlite3_bind_int64(st, 2, settings_id);
            sqlite3_bind_int64(st, 3, plan_id);
            rc = sqlite3_step(st); sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "insert" });
            return DbResult<void>{ true };
        }

        static inline DbResult<void> Impl_Remove(DbEnv& env, int64_t settings_id, int32_t ordinal) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "DELETE FROM explorer_settings_plan_link WHERE settings_id=? AND ordinal=?;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare del" });
            sqlite3_bind_int64(st, 1, settings_id);
            sqlite3_bind_int(st, 2, ordinal);
            rc = sqlite3_step(st); sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "delete" });
            return DbResult<void>{ true };
        }

        std::future<DbResult<void>> ExplorerSettingsPlanLinkRepo::ReplaceAllAsync(int64_t settings_id, const std::vector<int64_t>& plan_ids, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
                [=, &plan_ids](DbEnv& e) { return Impl_ReplaceAll(e, settings_id, plan_ids); });
        }

        std::future<DbResult<std::vector<SettingsPlanLinkRow>>> ExplorerSettingsPlanLinkRepo::ListAsync(int64_t settings_id, RetryPolicy rp) {
            return DBService::instance().submit_res<std::vector<SettingsPlanLinkRow>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_List(e, settings_id); });
        }

        std::future<DbResult<void>> ExplorerSettingsPlanLinkRepo::AppendAsync(int64_t settings_id, int64_t plan_id, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Append(e, settings_id, plan_id); });
        }

        std::future<DbResult<void>> ExplorerSettingsPlanLinkRepo::ClearAsync(int64_t settings_id, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Clear(e, settings_id); });
        }

        std::future<DbResult<void>> ExplorerSettingsPlanLinkRepo::RemoveAsync(int64_t settings_id, int32_t ordinal, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Remove(e, settings_id, ordinal); });
        }

    }
} // namespace
