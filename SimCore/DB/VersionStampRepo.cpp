#include "VersionStampRepo.h"
#include <sqlite3.h>

namespace simcore {
    namespace db {

        static inline DbResult<int64_t> Impl_Ensure(DbEnv& env,
            const std::string& dolphin_build_hash,
            const std::string& wrapper_commit,
            const std::string& simcore_commit,
            const std::string& vm_opcode_hash) {

            sqlite3* db = env.handle();
            sqlite3_stmt* sel{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id FROM version_stamp WHERE dolphin_build_hash=? AND wrapper_commit=? AND simcore_commit=? AND vm_opcode_hash=? LIMIT 1;",
                -1, &sel, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc),rc,"prepare sel" });
            sqlite3_bind_text(sel, 1, dolphin_build_hash.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(sel, 2, wrapper_commit.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(sel, 3, simcore_commit.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(sel, 4, vm_opcode_hash.c_str(), -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(sel);
            if (rc == SQLITE_ROW) { int64_t id = sqlite3_column_int64(sel, 0); sqlite3_finalize(sel); return DbResult<int64_t>::Ok(id); }
            sqlite3_finalize(sel);

            sqlite3_stmt* ins{};
            rc = sqlite3_prepare_v2(db,
                "INSERT INTO version_stamp(dolphin_build_hash, wrapper_commit, simcore_commit, vm_opcode_hash) VALUES(?,?,?,?);",
                -1, &ins, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc),rc,"prepare ins" });
            sqlite3_bind_text(ins, 1, dolphin_build_hash.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins, 2, wrapper_commit.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins, 3, simcore_commit.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins, 4, vm_opcode_hash.c_str(), -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(ins); sqlite3_finalize(ins);
            if (rc != SQLITE_DONE) return DbResult<int64_t>::Err({ map_sqlite_err(rc),rc,"insert" });
            return DbResult<int64_t>::Ok(sqlite3_last_insert_rowid(db));
        }

        static inline DbResult<VersionStampRow> Impl_Get(DbEnv& env, int64_t id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id, dolphin_build_hash, wrapper_commit, simcore_commit, vm_opcode_hash FROM version_stamp WHERE id=?;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<VersionStampRow>::Err({ map_sqlite_err(rc),rc,"prepare" });
            sqlite3_bind_int64(st, 1, id);
            rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DbResult<VersionStampRow>::Err({ DbErrorKind::NotFound,rc,"not found" }); }
            VersionStampRow row;
            row.id = sqlite3_column_int64(st, 0);
            row.dolphin_build_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
            row.wrapper_commit = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
            row.simcore_commit = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
            row.vm_opcode_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
            sqlite3_finalize(st);
            return DbResult<VersionStampRow>::Ok(std::move(row));
        }

        // Async surface

        std::future<DbResult<int64_t>> VersionStampRepo::EnsureAsync(const std::string& a, const std::string& b, const std::string& c, const std::string& d, RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Ensure(e, a, b, c, d); });
        }

        std::future<DbResult<VersionStampRow>> VersionStampRepo::GetAsync(int64_t id, RetryPolicy rp) {
            return DBService::instance().submit_res<VersionStampRow>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Get(e, id); });
        }

    } // namespace db
} // namespace simcore
