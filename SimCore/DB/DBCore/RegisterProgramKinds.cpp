#include "RegisterProgramKinds.h"
#include "../../Runner/IPC/Wire.h"   // for PK_* enum values
#include <sqlite3.h>

namespace simcore {
    namespace db {

        static inline DbResult<void> Impl_RegisterProgramKinds(DbEnv& env) {
            static const struct Row { int32_t id; const char* name; int32_t base_pri; int32_t spawn_ms; } kRows[] = {
                { PK_SeedProbe,          "SeedProbe",          0, 0 },
                { PK_TasMovie,           "TasMovie",           0, 0 },
                { PK_BattleTurnRunner,   "BattleTurnRunner",   0, 0 },
                { PK_BattleContextProbe, "BattleContextProbe", 0, 0 },
                { PK_BattleSingleTurnRunner, "BattleSingleTurnRunner", 0, 0 }
                // Add more ProgramKinds here if needed later.
            };

            sqlite3* db = env.handle();
            const char* sql =
                "INSERT OR IGNORE INTO program_kinds(kind_id,name,base_priority,learned_spawn_ms) "
                "VALUES(?,?,?,?);";
            sqlite3_stmt* st = nullptr;
            int rc = sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
            if (rc != SQLITE_OK) {
                return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare program_kinds insert" });
            }

            for (const auto& r : kRows) {
                sqlite3_reset(st);
                sqlite3_clear_bindings(st);
                sqlite3_bind_int(st, 1, r.id);
                sqlite3_bind_text(st, 2, r.name, -1, SQLITE_STATIC);
                sqlite3_bind_int(st, 3, r.base_pri);
                sqlite3_bind_int(st, 4, r.spawn_ms);
                rc = sqlite3_step(st);
                if (rc != SQLITE_DONE && rc != SQLITE_CONSTRAINT) {
                    sqlite3_finalize(st);
                    return DbResult<void>::Err({ map_sqlite_err(rc), rc, "insert program_kinds" });
                }
            }

            sqlite3_finalize(st);
            return DbResult<void>::Ok();
        }

        std::future<DbResult<void>> RegisterProgramKindsAsync(RetryPolicy rp) {
            // Write op: DBService will wrap in a transaction and handle retries/backoff.
            return DBService::instance().submit_res<void>(OpType::Write, Priority::High, rp,
                [](DbEnv& e) { return Impl_RegisterProgramKinds(e); });
        }

    } // namespace db
} // namespace simcore
