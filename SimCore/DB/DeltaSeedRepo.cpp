#include "DeltaSeedRepo.h"
#include <sqlite3.h>
#include <cstring> // for memcpy

namespace simcore {
    namespace db {

        // Serialize GCInputFrame to blob
        static void bind_input_blob(sqlite3_stmt* st, int idx, const GCInputFrame& input) {
            sqlite3_bind_blob(st, idx, &input, sizeof(GCInputFrame), SQLITE_TRANSIENT);
        }

        // Deserialize GCInputFrame from blob column
        static GCInputFrame column_input_blob(sqlite3_stmt* st, int idx) {
            GCInputFrame f{};
            const void* blob = sqlite3_column_blob(st, idx);
            int bytes = sqlite3_column_bytes(st, idx);
            if (blob && bytes == sizeof(GCInputFrame)) {
                std::memcpy(&f, blob, sizeof(GCInputFrame));
            }
            return f;
        }

        static inline DbResult<int64_t> Impl_BulkQueue(DbEnv& env, int64_t probe_id, const std::vector<DeltaSeedRow>& rows) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "INSERT INTO seed_delta(probe_id, seed_delta, input, is_grid, is_unique, complete) "
                "VALUES(?, ?, ?, 0, 1, 0);",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare" });

            int64_t inserted = 0;
            for (const auto& r : rows) {
                sqlite3_bind_int64(st, 1, probe_id);
                sqlite3_bind_int(st, 2, r.seed_delta);
                bind_input_blob(st, 3, r.input);
                rc = sqlite3_step(st);
                sqlite3_reset(st);
                sqlite3_clear_bindings(st);
                if (rc == SQLITE_DONE) ++inserted;
                else {
                    sqlite3_finalize(st);
                    return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "insert" });
                }
            }
            sqlite3_finalize(st);
            return DbResult<int64_t>::Ok(inserted);
        }

        static inline DbResult<void> Impl_MarkDone(DbEnv& env, int64_t id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "UPDATE seed_delta SET complete=1 WHERE id=?;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, id);
            rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
            return DbResult<void>{ true };
        }

        static inline DbResult<void> Impl_UpdateSeedDelta(DbEnv& env, int64_t id, int32_t seed_delta) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "UPDATE seed_delta SET seed_delta=? WHERE id=?;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int(st, 1, seed_delta);
            sqlite3_bind_int64(st, 2, id);
            rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
            return DbResult<void>{ true };
        }

        static inline DbResult<void> Impl_SetGrid(DbEnv& env, int64_t id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "UPDATE seed_delta SET is_grid=1 WHERE id=?;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, id);
            rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
            return DbResult<void>{ true };
        }

        static inline DbResult<void> Impl_SetUnique(DbEnv& env, int64_t id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "UPDATE seed_delta SET is_unique=1 WHERE id=?;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, id);
            rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
            return DbResult<void>{ true };
        }

        static inline DeltaSeedRow read_row(sqlite3_stmt* st) {
            DeltaSeedRow r{};
            r.id = sqlite3_column_int64(st, 0);
            r.probe_id = sqlite3_column_int64(st, 1);
            r.seed_delta = sqlite3_column_int(st, 2);
            r.input = column_input_blob(st, 3);
            r.complete = sqlite3_column_int(st, 4) != 0;
            return r;
        }

        static inline DbResult<std::vector<DeltaSeedRow>> Impl_ListActive(DbEnv& env, int64_t probe_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id, probe_id, seed_delta, input, complete "
                "FROM seed_delta WHERE probe_id=? AND complete=0 ORDER BY id ASC;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::vector<DeltaSeedRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, probe_id);

            std::vector<DeltaSeedRow> out;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                out.push_back(read_row(st));
            }
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<std::vector<DeltaSeedRow>>::Err({ map_sqlite_err(rc), rc, "select" });
            return DbResult<std::vector<DeltaSeedRow>>::Ok(std::move(out));
        }

        static inline DbResult<std::vector<DeltaSeedRow>> Impl_ListForProbe(DbEnv& env, int64_t probe_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id, probe_id, seed_delta, input, complete "
                "FROM seed_delta WHERE probe_id=? ORDER BY id ASC;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::vector<DeltaSeedRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, probe_id);

            std::vector<DeltaSeedRow> out;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                out.push_back(read_row(st));
            }
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<std::vector<DeltaSeedRow>>::Err({ map_sqlite_err(rc), rc, "select" });
            return DbResult<std::vector<DeltaSeedRow>>::Ok(std::move(out));
        }

        static inline DbResult<std::vector<DeltaSeedRow>> Impl_ListGridForProbe(DbEnv& env, int64_t probe_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id, probe_id, seed_delta, input, complete "
                "FROM seed_delta WHERE probe_id=? AND is_grid=1 ORDER BY id ASC;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::vector<DeltaSeedRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, probe_id);

            std::vector<DeltaSeedRow> out;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) out.push_back(read_row(st));
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<std::vector<DeltaSeedRow>>::Err({ map_sqlite_err(rc), rc, "scan" });
            return DbResult<std::vector<DeltaSeedRow>>::Ok(std::move(out));
        }

        static inline DbResult<std::vector<DeltaSeedRow>> Impl_ListUniqueForProbe(DbEnv& env, int64_t probe_id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id, probe_id, seed_delta, input, complete "
                "FROM seed_delta WHERE probe_id=? AND is_unique=1 ORDER BY id ASC;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::vector<DeltaSeedRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, probe_id);

            std::vector<DeltaSeedRow> out;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) out.push_back(read_row(st));
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<std::vector<DeltaSeedRow>>::Err({ map_sqlite_err(rc), rc, "scan" });
            return DbResult<std::vector<DeltaSeedRow>>::Ok(std::move(out));
        }

        static inline DbResult<std::optional<DeltaSeedRow>> Impl_Get(DbEnv& env, int64_t id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id, probe_id, seed_delta, input, complete FROM seed_delta WHERE id=? LIMIT 1;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::optional<DeltaSeedRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, id);
            rc = sqlite3_step(st);
            std::optional<DeltaSeedRow> out{};
            if (rc == SQLITE_ROW) out = read_row(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_ROW && rc != SQLITE_DONE) return DbResult<std::optional<DeltaSeedRow>>::Err({ map_sqlite_err(rc), rc, "scan" });
            return DbResult<std::optional<DeltaSeedRow>>::Ok(out);
        }

        static inline DbResult<bool> Impl_ExistsForProbeSeedDelta(DbEnv& env, int64_t probe_id, int32_t seed_delta) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT 1 FROM seed_delta WHERE probe_id=? AND seed_delta=? LIMIT 1;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<bool>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, probe_id);
            sqlite3_bind_int(st, 2, seed_delta);
            rc = sqlite3_step(st);
            const bool exists = (rc == SQLITE_ROW);
            sqlite3_finalize(st);
            if (rc != SQLITE_ROW && rc != SQLITE_DONE) return DbResult<bool>::Err({ map_sqlite_err(rc), rc, "scan" });
            return DbResult<bool>::Ok(exists);
        }

        static inline DbResult<int64_t> Impl_InsertOne(DbEnv& env, int64_t probe_id, const DeltaSeedRow& r, bool is_grid, bool is_unique) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "INSERT INTO seed_delta(probe_id, seed_delta, input, is_grid, is_unique, complete) VALUES(?, ?, ?, ?, ?, 0);",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, probe_id);
            sqlite3_bind_int(st, 2, r.seed_delta);
            bind_input_blob(st, 3, r.input);
            sqlite3_bind_int(st, 4, is_grid ? 1 : 0);
            sqlite3_bind_int(st, 5, is_unique ? 1 : 0);
            rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "insert" });
            return DbResult<int64_t>::Ok(1);
        }

        // Async via DBService

        std::future<DbResult<int64_t>> DeltaSeedRepo::BulkQueueAsync(int64_t probe_id, const std::vector<DeltaSeedRow>& rows, RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
                [=, &rows](DbEnv& e) { return Impl_BulkQueue(e, probe_id, rows); });
        }

        std::future<DbResult<void>> DeltaSeedRepo::MarkDoneAsync(int64_t id, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::High, rp,
                [=](DbEnv& e) { return Impl_MarkDone(e, id); });
        }

        std::future<DbResult<void>> DeltaSeedRepo::UpdateSeedDeltaAsync(int64_t id, int32_t seed_delta, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::High, rp,
                [=](DbEnv& e) { return Impl_UpdateSeedDelta(e, id, seed_delta); });
        }

        std::future<DbResult<void>> DeltaSeedRepo::SetGridAsync(int64_t id, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_SetGrid(e, id); });
        }

        std::future<DbResult<void>> DeltaSeedRepo::SetUniqueAsync(int64_t id, RetryPolicy rp) {
            return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_SetUnique(e, id); });
        }

        std::future<DbResult<std::vector<DeltaSeedRow>>> DeltaSeedRepo::ListActiveAsync(int64_t probe_id, RetryPolicy rp) {
            return DBService::instance().submit_res<std::vector<DeltaSeedRow>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ListActive(e, probe_id); });
        }

        std::future<DbResult<std::vector<DeltaSeedRow>>> DeltaSeedRepo::ListForProbeAsync(int64_t probe_id, RetryPolicy rp) {
            return DBService::instance().submit_res<std::vector<DeltaSeedRow>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ListForProbe(e, probe_id); });
        }

        std::future<DbResult<int64_t>> DeltaSeedRepo::InsertOneAsync(int64_t probe_id, const DeltaSeedRow& row, bool is_grid, bool is_unique, RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
                [=, &row](DbEnv& e) { return Impl_InsertOne(e, probe_id, row, is_grid, is_unique); });
        }

        std::future<DbResult<std::vector<DeltaSeedRow>>> DeltaSeedRepo::ListGridForProbeAsync(int64_t probe_id, RetryPolicy rp) {
            return DBService::instance().submit_res<std::vector<DeltaSeedRow>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ListGridForProbe(e, probe_id); });
        }

        std::future<DbResult<std::vector<DeltaSeedRow>>> DeltaSeedRepo::ListUniqueForProbeAsync(int64_t probe_id, RetryPolicy rp) {
            return DBService::instance().submit_res<std::vector<DeltaSeedRow>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ListUniqueForProbe(e, probe_id); });
        }

        std::future<DbResult<std::optional<DeltaSeedRow>>> DeltaSeedRepo::GetAsync(int64_t id, RetryPolicy rp) {
            return DBService::instance().submit_res<std::optional<DeltaSeedRow>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Get(e, id); });
        }

        std::future<DbResult<bool>> DeltaSeedRepo::ExistsForProbeSeedDeltaAsync(int64_t probe_id, int32_t seed_delta, RetryPolicy rp) {
            return DBService::instance().submit_res<bool>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ExistsForProbeSeedDelta(e, probe_id, seed_delta); });
        }

    } // namespace db
} // namespace simcore
