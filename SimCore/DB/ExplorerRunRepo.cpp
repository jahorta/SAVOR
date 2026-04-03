#include "ExplorerRunRepo.h"
#include <sqlite3.h>

namespace simcore {
    namespace db {

        static inline DbResult<int64_t> Impl_IdempotentCreate(DbEnv& env, int64_t root_job_set_id, int64_t settings_id, int64_t plan_id, int64_t delta_seed_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};

            int rc = sqlite3_prepare_v2(db,
                "SELECT id FROM explorer_run WHERE root_job_set_id=? AND settings_id=? AND plan_id=? AND delta_seed_id=? LIMIT 1;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare find" });
            sqlite3_bind_int64(st, 1, root_job_set_id);
            sqlite3_bind_int64(st, 2, settings_id);
            sqlite3_bind_int64(st, 3, plan_id);
            sqlite3_bind_int64(st, 4, delta_seed_id);
            rc = sqlite3_step(st);
            if (rc == SQLITE_ROW) {
                int64_t id = sqlite3_column_int64(st, 0);
                sqlite3_finalize(st);
                return DbResult<int64_t>::Ok(id);
            }
            sqlite3_finalize(st);

            rc = sqlite3_prepare_v2(db,
                "INSERT INTO explorer_run(root_job_set_id, settings_id, plan_id, delta_seed_id, has_victory) VALUES(?,?,?,?,0);",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare insert" });
            sqlite3_bind_int64(st, 1, root_job_set_id);
            sqlite3_bind_int64(st, 2, settings_id);
            sqlite3_bind_int64(st, 3, plan_id);
            sqlite3_bind_int64(st, 4, delta_seed_id);
            rc = sqlite3_step(st);
            if (rc != SQLITE_DONE) {
                sqlite3_finalize(st);
                return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "insert explorer_run" });
            }
            const int64_t id = sqlite3_last_insert_rowid(db);
            sqlite3_finalize(st);
            return DbResult<int64_t>::Ok(id);
        }

        static inline DbResult<ExplorerRunRow> Impl_Get(DbEnv& env, int64_t run_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id, root_job_set_id, settings_id, plan_id, delta_seed_id, has_victory FROM explorer_run WHERE id=? LIMIT 1;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<ExplorerRunRow>::Err({ map_sqlite_err(rc), rc, "prepare get" });
            sqlite3_bind_int64(st, 1, run_id);
            rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) {
                sqlite3_finalize(st);
                return DbResult<ExplorerRunRow>::Err({ DbErrorKind::NotFound, SQLITE_DONE, "explorer_run not found" });
            }

            ExplorerRunRow row{};
            row.id = sqlite3_column_int64(st, 0);
            row.root_job_set_id = sqlite3_column_int64(st, 1);
            row.settings_id = sqlite3_column_int64(st, 2);
            row.plan_id = sqlite3_column_int64(st, 3);
            row.delta_seed_id = sqlite3_column_int64(st, 4);
            row.has_victory = sqlite3_column_int(st, 5) != 0;
            sqlite3_finalize(st);
            return DbResult<ExplorerRunRow>::Ok(row);
        }

        static inline DbResult<void> Impl_SetHasVictory(DbEnv& env, int64_t run_id, bool has_victory) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db, "UPDATE explorer_run SET has_victory=? WHERE id=?;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare set has_victory" });
            sqlite3_bind_int(st, 1, has_victory ? 1 : 0);
            sqlite3_bind_int64(st, 2, run_id);
            rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update has_victory" });
            return DbResult<void>::Ok();
        }

        static inline DbResult<std::vector<int64_t>> Impl_ListRootJobSetIdsByVictory(DbEnv& env, bool has_victory) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(
                db,
                "SELECT DISTINCT root_job_set_id FROM explorer_run WHERE has_victory=?;",
                -1,
                &st,
                nullptr);
            if (rc != SQLITE_OK) {
                return DbResult<std::vector<int64_t>>::Err({ map_sqlite_err(rc), rc, "prepare list root_job_set_ids by victory" });
            }

            sqlite3_bind_int(st, 1, has_victory ? 1 : 0);
            std::vector<int64_t> rootIds;
            while (true) {
                rc = sqlite3_step(st);
                if (rc == SQLITE_ROW) {
                    rootIds.push_back(sqlite3_column_int64(st, 0));
                    continue;
                }
                if (rc == SQLITE_DONE) {
                    break;
                }
                sqlite3_finalize(st);
                return DbResult<std::vector<int64_t>>::Err({ map_sqlite_err(rc), rc, "step list root_job_set_ids by victory" });
            }
            sqlite3_finalize(st);
            return DbResult<std::vector<int64_t>>::Ok(std::move(rootIds));
        }

        static inline DbResult<Page<int64_t>> Impl_ListRootJobSetIdsByVictoryPaged(
            DbEnv& env,
            bool has_victory,
            const std::optional<KeysetCursor>& before,
            int limit)
        {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            const char* sqlWithCursor =
                "SELECT r.root_job_set_id, js.created_at "
                "FROM explorer_run r "
                "JOIN job_sets js ON js.job_set_id = r.root_job_set_id "
                "WHERE r.has_victory=? "
                "  AND (js.created_at < ? OR (js.created_at = ? AND r.root_job_set_id < ?)) "
                "GROUP BY r.root_job_set_id, js.created_at "
                "ORDER BY js.created_at DESC, r.root_job_set_id DESC "
                "LIMIT ?;";
            const char* sqlWithoutCursor =
                "SELECT r.root_job_set_id, js.created_at "
                "FROM explorer_run r "
                "JOIN job_sets js ON js.job_set_id = r.root_job_set_id "
                "WHERE r.has_victory=? "
                "GROUP BY r.root_job_set_id, js.created_at "
                "ORDER BY js.created_at DESC, r.root_job_set_id DESC "
                "LIMIT ?;";

            int rc = sqlite3_prepare_v2(db, before.has_value() ? sqlWithCursor : sqlWithoutCursor, -1, &st, nullptr);
            if (rc != SQLITE_OK) {
                return DbResult<Page<int64_t>>::Err({ map_sqlite_err(rc), rc, "prepare list root_job_set_ids by victory paged" });
            }

            int bindIdx = 1;
            sqlite3_bind_int(st, bindIdx++, has_victory ? 1 : 0);
            if (before.has_value()) {
                sqlite3_bind_int64(st, bindIdx++, before->primary);
                sqlite3_bind_int64(st, bindIdx++, before->primary);
                sqlite3_bind_int64(st, bindIdx++, before->secondary);
            }
            sqlite3_bind_int(st, bindIdx++, limit);

            Page<int64_t> page{};
            page.items.reserve(static_cast<size_t>(limit));
            std::optional<KeysetCursor> lastCursor;
            while (true) {
                rc = sqlite3_step(st);
                if (rc == SQLITE_ROW) {
                    page.items.push_back(sqlite3_column_int64(st, 0));
                    lastCursor = KeysetCursor{
                        sqlite3_column_int64(st, 1),
                        sqlite3_column_int64(st, 0)
                    };
                    continue;
                }
                if (rc == SQLITE_DONE) {
                    break;
                }
                sqlite3_finalize(st);
                return DbResult<Page<int64_t>>::Err({ map_sqlite_err(rc), rc, "step list root_job_set_ids by victory paged" });
            }
            sqlite3_finalize(st);

            if (before.has_value() && !page.items.empty()) {
                page.prev = before;
            }
            if (static_cast<int>(page.items.size()) == limit && lastCursor.has_value()) {
                page.next = lastCursor;
            }
            return DbResult<Page<int64_t>>::Ok(std::move(page));
        }

        std::future<DbResult<int64_t>> ExplorerRunRepo::IdempotentCreateAsync(int64_t root_job_set_id, int64_t settings_id, int64_t plan_id, int64_t delta_seed_id, RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_IdempotentCreate(e, root_job_set_id, settings_id, plan_id, delta_seed_id); });
        }

        std::future<DbResult<ExplorerRunRow>> ExplorerRunRepo::GetAsync(int64_t run_id, RetryPolicy rp) {
            return DBService::instance().submit_res<ExplorerRunRow>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Get(e, run_id); });
        }

        std::future<DbResult<void>> ExplorerRunRepo::SetHasVictoryAsync(int64_t run_id, bool has_victory, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_SetHasVictory(e, run_id, has_victory); });
        }

        std::future<DbResult<std::vector<int64_t>>> ExplorerRunRepo::ListRootJobSetIdsByVictoryAsync(bool has_victory, RetryPolicy rp) {
            return DBService::instance().submit_res<std::vector<int64_t>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ListRootJobSetIdsByVictory(e, has_victory); });
        }

        std::future<DbResult<Page<int64_t>>> ExplorerRunRepo::ListRootJobSetIdsByVictoryPagedAsync(
            bool has_victory,
            std::optional<KeysetCursor> before,
            int limit,
            RetryPolicy rp)
        {
            if (limit <= 0) {
                limit = 50;
            }
            return DBService::instance().submit_res<Page<int64_t>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ListRootJobSetIdsByVictoryPaged(e, has_victory, before, limit); });
        }

    } // db
} // simcore
