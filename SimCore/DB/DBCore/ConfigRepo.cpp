// simcore/db/Repos/ConfigRepo.cpp
#include "ConfigRepo.h"

namespace simcore::db {

    DbResult<int> ConfigRepo::EnsureDefaults(const std::vector<SeedAny>& items) {
        return DBService::instance().submit_res<int>(OpType::Write, Priority::High, RetryPolicy{},
            [&items](DbEnv& env)->DbResult<int> {
                auto db = env.handle();
                int writes = 0;
                for (const auto& it : items) {
                    sqlite3_stmt* st = nullptr;
                    std::string sql = std::string("UPDATE config SET ") + it.col + " = COALESCE(" + it.col + ", ?) WHERE config_id=1;";
                    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
                        return DbResult<int>::Err({ DbErrorKind::IO, sqlite3_errcode(db), sqlite3_errmsg(db)});
                    }
                    switch (it.kind) {
                    case SeedAny::Kind::Text:  sqlite3_bind_text(st, 1, it.text.c_str(), -1, SQLITE_TRANSIENT); break;
                    case SeedAny::Kind::Int64: sqlite3_bind_int64(st, 1, it.i64); break;
                    case SeedAny::Kind::Bool:  sqlite3_bind_int64(st, 1, it.b ? 1 : 0); break;
                    }
                    int rc = sqlite3_step(st);
                    if (rc != SQLITE_DONE) { sqlite3_finalize(st); return DbResult<int>::Err({ DbErrorKind::IO, sqlite3_errcode(db), "UPDATE failed" }); }
                    writes += sqlite3_changes(db);
                    sqlite3_finalize(st);
                }
                return DbResult<int>::Ok(writes);
            }).get();
    }

} // namespace simcore::db
