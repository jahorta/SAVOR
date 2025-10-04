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

        static inline DbResult<std::string> Impl_Materialize(DbEnv& env, int64_t savestate_id, const std::string& objdir, const std::string& tmpdir) {
            auto row = Impl_Get(env, savestate_id);
            if (!row.ok) return DbResult<std::string>::Err(row.error);
            if (!row.value.has_value()) return DbResult<std::string>::Err({ DbErrorKind::NotFound, 0, "savestate not found" });
            const auto ss = row.value.value();
            if (!ss.complete || ss.object_ref_id <= 0) return DbResult<std::string>::Err({ DbErrorKind::InvalidState, 0, "savestate incomplete" });
            auto mat = ObjectStore::MaterializeToTemp(ss.object_ref_id);
            if (!mat.ok) return DbResult<std::string>::Err(mat.error);
            return DbResult<std::string>::Ok(mat.value);
        }

        std::future<DbResult<std::string>> SavestateRepo::MaterializeToTempPathAsync(
            int64_t savestate_id, std::string objdir, std::string tmpdir, RetryPolicy rp) {
            return DBService::instance().submit_res<std::string>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Materialize(e, savestate_id, objdir, tmpdir); });
        }

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

    } // namespace db
} // namespace simcore
