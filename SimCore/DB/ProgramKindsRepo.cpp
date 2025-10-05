// SimCore/DB/ProgramKindsRepo.cpp
#include "ProgramKindsRepo.h"
#include <sqlite3.h>

namespace simcore::db {

    static DbResult<std::vector<ProgramKindRow>> impl_list_all(DbEnv& env) {
        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT kind_id, name, base_priority, learned_spawn_ms FROM program_kinds ORDER BY kind_id", -1, &st, nullptr) != SQLITE_OK) {
            int rc = sqlite3_errcode(db);
            return DbResult<std::vector<ProgramKindRow>>::Err({ map_sqlite_err(rc), rc, sqlite3_errmsg(db) });
        }
        std::vector<ProgramKindRow> out;
        for (;;) {
            int rc = sqlite3_step(st);
            if (rc == SQLITE_ROW) {
                ProgramKindRow r{};
                r.id = sqlite3_column_int(st, 0);
                r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                r.base_priority = sqlite3_column_int(st, 2);
                r.spawn_ms = sqlite3_column_int(st, 3);
                out.push_back(std::move(r));
            }
            else if (rc == SQLITE_DONE) {
                break;
            }
            else {
                int ec = sqlite3_errcode(db);
                sqlite3_finalize(st);
                return DbResult<std::vector<ProgramKindRow>>::Err({ map_sqlite_err(ec), ec, sqlite3_errmsg(db) });
            }
        }
        sqlite3_finalize(st);
        return DbResult<std::vector<ProgramKindRow>>::Ok(std::move(out));
    }

    static DbResult<std::optional<std::string>> impl_get_name(DbEnv& env, int id) {
        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT name FROM program_kinds WHERE kind_id=?", -1, &st, nullptr) != SQLITE_OK) {
            int rc = sqlite3_errcode(db);
            return DbResult<std::optional<std::string>>::Err({ map_sqlite_err(rc), rc, sqlite3_errmsg(db) });
        }
        sqlite3_bind_int(st, 1, id);
        std::optional<std::string> name;
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            name = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        }
        else if (rc != SQLITE_DONE) {
            int ec = sqlite3_errcode(db);
            sqlite3_finalize(st);
            return DbResult<std::optional<std::string>>::Err({ map_sqlite_err(ec), ec, sqlite3_errmsg(db) });
        }
        sqlite3_finalize(st);
        return DbResult<std::optional<std::string>>::Ok(std::move(name));
    }

    std::future<DbResult<std::vector<ProgramKindRow>>> ProgramKindsRepo::ListAllAsync(RetryPolicy rp) {
        return DBService::instance().submit_res<std::vector<ProgramKindRow>>(OpType::Read, Priority::Normal, rp,
            [](DbEnv& e) { return impl_list_all(e); });
    }

    std::future<DbResult<std::optional<std::string>>> ProgramKindsRepo::GetNameAsync(int id, RetryPolicy rp) {
        return DBService::instance().submit_res<std::optional<std::string>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_get_name(e, id); });
    }

} // namespace simcore::db
