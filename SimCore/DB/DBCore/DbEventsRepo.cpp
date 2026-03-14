#include "DbEventsRepo.h"
#include "CoordinatorClock.h"
#include <sqlite3.h>

namespace simcore {
    namespace db {

        static inline DbResult<int64_t> Impl_Insert(DbEnv& env,
            const std::string& kind,
            int64_t created_mono_ns,
            const std::string& created_utc,
            const std::string& coord_boot_id,
            const std::string& payload) {

            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "INSERT INTO db_event(kind, created_mono_ns, created_utc, coord_boot_id, payload) VALUES(?, ?, ?, ?, ?);",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare insert db_event" });

            sqlite3_bind_text(st, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, created_mono_ns);
            sqlite3_bind_text(st, 3, created_utc.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 4, coord_boot_id.c_str(), -1, SQLITE_TRANSIENT);
            if (payload.empty()) sqlite3_bind_null(st, 5);
            else sqlite3_bind_text(st, 5, payload.c_str(), -1, SQLITE_TRANSIENT);

            rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "insert db_event" });

            return DbResult<int64_t>::Ok(sqlite3_last_insert_rowid(db));
        }

        static inline DbResult<void> Impl_InsertBootEvent(DbEnv& env, const std::string& kind, const std::string& payload) {
            auto& clk = CoordinatorClock::instance();
            auto r = Impl_Insert(env, kind, clk.mono_now_ns(), clk.boot_wall_utc_iso(), clk.boot_id(), payload);
            if (!r.ok) return DbResult<void>::Err(r.error);
            return DbResult<void>::Ok();
        }

        static inline DbResult<std::vector<DbEventRow>> Impl_ListRecent(DbEnv& env, int limit) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id, kind, created_mono_ns, created_utc, coord_boot_id, payload FROM db_event ORDER BY created_mono_ns DESC LIMIT ?;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::vector<DbEventRow>>::Err({ map_sqlite_err(rc), rc, "prepare list recent" });
            sqlite3_bind_int(st, 1, (limit <= 0 ? 50 : limit));
            std::vector<DbEventRow> out;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                DbEventRow r{};
                r.id = sqlite3_column_int64(st, 0);
                r.kind = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                r.created_mono_ns = sqlite3_column_int64(st, 2);
                r.created_utc = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
                r.coord_boot_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
                const unsigned char* p = sqlite3_column_text(st, 5);
                if (p) r.payload = reinterpret_cast<const char*>(p);
                out.push_back(std::move(r));
            }
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<std::vector<DbEventRow>>::Err({ map_sqlite_err(rc), rc, "scan list recent" });
            return DbResult<std::vector<DbEventRow>>::Ok(std::move(out));
        }

        static inline DbResult<std::vector<DbEventRow>> Impl_ListSince(DbEnv& env, int64_t since_mono_ns, const std::string& kind_filter) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            const char* SQL_ALL =
                "SELECT id, kind, created_mono_ns, created_utc, coord_boot_id, payload FROM db_event WHERE created_mono_ns > ? ORDER BY created_mono_ns ASC;";
            const char* SQL_KIND =
                "SELECT id, kind, created_mono_ns, created_utc, coord_boot_id, payload FROM db_event WHERE created_mono_ns > ? AND kind=? ORDER BY created_mono_ns ASC;";
            int rc = sqlite3_prepare_v2(db, kind_filter.empty() ? SQL_ALL : SQL_KIND, -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::vector<DbEventRow>>::Err({ map_sqlite_err(rc), rc, "prepare list since" });

            sqlite3_bind_int64(st, 1, since_mono_ns);
            if (!kind_filter.empty()) sqlite3_bind_text(st, 2, kind_filter.c_str(), -1, SQLITE_TRANSIENT);

            std::vector<DbEventRow> out;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                DbEventRow r{};
                r.id = sqlite3_column_int64(st, 0);
                r.kind = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                r.created_mono_ns = sqlite3_column_int64(st, 2);
                r.created_utc = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
                r.coord_boot_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
                const unsigned char* p = sqlite3_column_text(st, 5);
                if (p) r.payload = reinterpret_cast<const char*>(p);
                out.push_back(std::move(r));
            }
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<std::vector<DbEventRow>>::Err({ map_sqlite_err(rc), rc, "scan list since" });
            return DbResult<std::vector<DbEventRow>>::Ok(std::move(out));
        }

        // Async

        std::future<DbResult<int64_t>> DbEventsRepo::InsertAsync(const std::string& k, int64_t ns, const std::string& utc, const std::string& boot, const std::string& p, RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Insert(e, k, ns, utc, boot, p); });
        }

        std::future<DbResult<void>> DbEventsRepo::InsertBootEventAsync(const std::string& k, const std::string& p, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::High, rp,
                [=](DbEnv& e) { return Impl_InsertBootEvent(e, k, p); });
        }

        std::future<DbResult<std::vector<DbEventRow>>> DbEventsRepo::ListRecentAsync(int limit, RetryPolicy rp) {
            return DBService::instance().submit_res<std::vector<DbEventRow>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ListRecent(e, limit); });
        }

        std::future<DbResult<std::vector<DbEventRow>>> DbEventsRepo::ListSinceAsync(int64_t ns, const std::string& kf, RetryPolicy rp) {
            return DBService::instance().submit_res<std::vector<DbEventRow>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ListSince(e, ns, kf); });
        }

    } // namespace db
} // namespace simcore
