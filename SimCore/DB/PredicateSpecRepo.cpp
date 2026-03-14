#include "PredicateSpecRepo.h"
#include <sqlite3.h>
#include "../Runner/Breakpoints/Predicate.h"

namespace simcore {
    namespace db {

        static void bind_spec(sqlite3_stmt* st, const PredicateSpecRow& r) {
            // Column order in INSERT (no pred_id):
            // spec_version, required_bp, kind, width, cmp_op, flags,
            // lhs_addr, lhs_key, rhs_value, rhs_key, turn_mask, lhs_prog_id, rhs_prog_id, name, desc, fingerprint
            sqlite3_bind_int(st, 1, r.spec_version);
            sqlite3_bind_int(st, 2, r.required_bp);
            sqlite3_bind_int(st, 3, r.kind);
            sqlite3_bind_int(st, 4, r.width);
            sqlite3_bind_int(st, 5, r.cmp_op);
            sqlite3_bind_int(st, 6, r.flags);
            sqlite3_bind_int64(st, 7, r.lhs_addr);
            if (r.lhs_key) sqlite3_bind_int(st, 8, *r.lhs_key); else sqlite3_bind_null(st, 8);
            sqlite3_bind_int64(st, 9, r.rhs_value);
            if (r.rhs_key) sqlite3_bind_int(st, 10, *r.rhs_key); else sqlite3_bind_null(st, 10);
            sqlite3_bind_int(st, 11, r.turn_mask);
            if (r.lhs_prog_id) sqlite3_bind_int64(st, 12, *r.lhs_prog_id); else sqlite3_bind_null(st, 12);
            if (r.rhs_prog_id) sqlite3_bind_int64(st, 13, *r.rhs_prog_id); else sqlite3_bind_null(st, 13);
            sqlite3_bind_text(st, 14, r.name.c_str(),        -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 15, r.description.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 16, r.fingerprint.c_str(), -1, SQLITE_TRANSIENT);
        }

        static inline DbResult<int64_t> Impl_Insert(DbEnv& env, const PredicateSpecRow& r) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "INSERT INTO predicate_spec("
                "spec_version,required_bp,kind,width,cmp_op,flags,"
                "lhs_addr,lhs_key,rhs_value,rhs_key,turn_mask,lhs_prog_id,rhs_prog_id,name,desc,fingerprint)"
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare" });
            bind_spec(st, r);
            rc = sqlite3_step(st); sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "insert" });
            return DbResult<int64_t>::Ok(sqlite3_last_insert_rowid(db));
        }

        static inline DbResult<int64_t> Impl_BulkInsert(DbEnv& env, const std::vector<PredicateSpecRow>& rows) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "INSERT INTO predicate_spec("
                "spec_version,required_bp,kind,width,cmp_op,flags,"
                "lhs_addr,lhs_key,rhs_value,rhs_key,turn_mask,lhs_prog_id,rhs_prog_id,name,desc,fingerprint)"
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare" });
            int64_t cnt = 0;
            for (const auto& r : rows) {
                sqlite3_reset(st); sqlite3_clear_bindings(st);
                bind_spec(st, r);
                rc = sqlite3_step(st);
                if (rc != SQLITE_DONE) { sqlite3_finalize(st); return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "insert" }); }
                ++cnt;
            }
            sqlite3_finalize(st);
            return DbResult<int64_t>::Ok(cnt);
        }

        static inline PredicateSpecRow read_row(sqlite3_stmt* st) {
            // SELECT columns (no pred_id):
            // 0:id,1:spec_version,2:required_bp,3:kind,4:width,5:cmp_op,6:flags,
            // 7:lhs_addr,8:lhs_key,9:rhs_value,10:rhs_key,11:turn_mask,12:lhs_prog_id,13:rhs_prog_id,14:name,15:desc,16:fingerprint
            PredicateSpecRow r;
            r.id = sqlite3_column_int64(st, 0);
            r.spec_version = sqlite3_column_int(st, 1);
            r.required_bp = sqlite3_column_int(st, 2);
            r.kind = sqlite3_column_int(st, 3);
            r.width = sqlite3_column_int(st, 4);
            r.cmp_op = sqlite3_column_int(st, 5);
            r.flags = sqlite3_column_int(st, 6);
            r.lhs_addr = sqlite3_column_int64(st, 7);
            if (sqlite3_column_type(st, 8) == SQLITE_NULL) r.lhs_key.reset(); else r.lhs_key = sqlite3_column_int(st, 8);
            r.rhs_value = sqlite3_column_int64(st, 9);
            if (sqlite3_column_type(st, 10) == SQLITE_NULL) r.rhs_key.reset(); else r.rhs_key = sqlite3_column_int(st, 10);
            r.turn_mask = sqlite3_column_int(st, 11);
            if (sqlite3_column_type(st, 12) == SQLITE_NULL) r.lhs_prog_id.reset(); else r.lhs_prog_id = sqlite3_column_int64(st, 12);
            if (sqlite3_column_type(st, 13) == SQLITE_NULL) r.rhs_prog_id.reset(); else r.rhs_prog_id = sqlite3_column_int64(st, 13);
            r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 14));
            r.description = reinterpret_cast<const char*>(sqlite3_column_text(st, 15));
            r.fingerprint = reinterpret_cast<const char*>(sqlite3_column_text(st, 16));
            return r;
        }

        static inline DbResult<PredicateSpecRow> Impl_Get(DbEnv& env, int64_t id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id,spec_version,required_bp,kind,width,cmp_op,flags,lhs_addr,lhs_key,rhs_value,rhs_key,turn_mask,lhs_prog_id,rhs_prog_id,name,desc,fingerprint "
                "FROM predicate_spec WHERE id=?;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<PredicateSpecRow>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int64(st, 1, id);
            rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DbResult<PredicateSpecRow>::Err({ DbErrorKind::NotFound, rc, "not found" }); }
            auto r = read_row(st);
            sqlite3_finalize(st);
            return DbResult<PredicateSpecRow>::Ok(std::move(r));
        }

        static inline DbResult<std::vector<PredicateSpecRow>> Impl_ListByBp(DbEnv& env, int32_t required_bp) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id,spec_version,required_bp,kind,width,cmp_op,flags,lhs_addr,lhs_key,rhs_value,rhs_key,turn_mask,lhs_prog_id,rhs_prog_id,name,desc,fingerprint "
                "FROM predicate_spec WHERE required_bp=? ORDER BY id;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::vector<PredicateSpecRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_int(st, 1, required_bp);
            std::vector<PredicateSpecRow> out;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) out.push_back(read_row(st));
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<std::vector<PredicateSpecRow>>::Err({ map_sqlite_err(rc), rc, "scan" });
            return DbResult<std::vector<PredicateSpecRow>>::Ok(std::move(out));
        }

        // Async

        std::future<DbResult<int64_t>> PredicateSpecRepo::InsertAsync(const PredicateSpecRow& r, RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Insert(e, r); });
        }

        std::future<DbResult<int64_t>> PredicateSpecRepo::BulkInsertAsync(std::vector<PredicateSpecRow>& rows, RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::High, rp,
                [=, rs = std::move(rows)](DbEnv& e) { return Impl_BulkInsert(e, rs); });
        }

        std::future<DbResult<PredicateSpecRow>> PredicateSpecRepo::GetAsync(int64_t id, RetryPolicy rp) {
            return DBService::instance().submit_res<PredicateSpecRow>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Get(e, id); });
        }

        std::future<DbResult<std::vector<PredicateSpecRow>>> PredicateSpecRepo::ListByBpAsync(int32_t required_bp, RetryPolicy rp) {
            return DBService::instance().submit_res<std::vector<PredicateSpecRow>>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_ListByBp(e, required_bp); });
        }

        static inline DbResult<std::optional<int64_t>> Impl_FindByFingerprint(DbEnv& env, const std::string& fp) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db, "SELECT id FROM predicate_spec WHERE fingerprint=? LIMIT 1;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::optional<int64_t>>::Err({ map_sqlite_err(rc), rc, "prepare" });
            sqlite3_bind_text(st, 1, fp.c_str(), -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(st);
            std::optional<int64_t> out{};
            if (rc == SQLITE_ROW) out = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
            if (rc != SQLITE_ROW && rc != SQLITE_DONE) return DbResult<std::optional<int64_t>>::Err({ map_sqlite_err(rc), rc, "scan" });
            return DbResult<std::optional<int64_t>>::Ok(out);
        }

        std::future<DbResult<int64_t>> PredicateSpecRepo::EnsureByFingerprintAsync(const PredicateSpecRow& r, RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
                [=, &r](DbEnv& e) -> DbResult<int64_t> {
                    auto f = Impl_FindByFingerprint(e, r.fingerprint);
                    if (!f.ok) return DbResult<int64_t>::Err(f.error);
                    if (f.value) return DbResult<int64_t>::Ok(*f.value);
                    return Impl_Insert(e, r);
                });
        }

        // lightweight search for left-pane library
        static inline DbResult<std::vector<PredicateSpecLite>>
            Impl_ListLite(DbEnv& env, const std::string& search, int32_t limit) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};

            // search by description (desc); case-insensitive; escape % _ \ like your other ListLite impls
            const char* sql =
                "SELECT id,flags,required_bp,COALESCE(name,''),COALESCE(desc,'') "
                "FROM predicate_spec "
                "WHERE LOWER(COALESCE(desc,'')) LIKE LOWER(?) ESCAPE '\\' OR LOWER(COALESCE(name,'')) LIKE LOWER(?) ESCAPE '\\'"
                "ORDER BY id DESC "
                "LIMIT ?;";

            if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
                return DbResult<std::vector<PredicateSpecLite>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });

            std::string pattern = "%";
            for (char c : search) { if (c == '%' || c == '_' || c == '\\') pattern.push_back('\\'); pattern.push_back(c); }
            pattern.push_back('%');

            sqlite3_bind_text(st, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 3, (limit <= 0 ? 50 : limit));

            std::vector<PredicateSpecLite> out;
            int rc;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                PredicateSpecLite r{};
                r.id = sqlite3_column_int64(st, 0);
                auto flags = sqlite3_column_int(st, 1);
                r.abort_on_fail = (static_cast<uint32_t>(flags) & (uint32_t)simcore::pred::PredFlag::AbortOnFail);
                auto bp = bp::BPRegistry::find((BPKey)sqlite3_column_int(st, 2));
                if (bp != nullptr) r.required_bp = *bp;
                r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
                r.description = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
                out.emplace_back(std::move(r));
            }
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE)
                return DbResult<std::vector<PredicateSpecLite>>::Err({ map_sqlite_err(rc), rc, "list predicate specs" });

            return DbResult<std::vector<PredicateSpecLite>>::Ok(std::move(out));
        }

        std::future<DbResult<std::vector<PredicateSpecLite>>>
            PredicateSpecRepo::ListLiteAsync(const std::string& search, int32_t limit, RetryPolicy rp) {
            return DBService::instance().submit_res<std::vector<PredicateSpecLite>>(
                OpType::Read, Priority::Normal, rp,
                [=](DbEnv& env) { return Impl_ListLite(env, search, limit); }
            );
        }

    } // db
} // simcore
