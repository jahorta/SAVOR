#include "AddressProgramRepo.h"
#include <sqlite3.h>

namespace simcore {
    namespace db {

        static inline DbResult<int64_t> Impl_Ensure(DbEnv& env,
            int32_t program_version,
            const std::vector<uint8_t>& prog_bytes,
            const std::optional<int32_t>& dbv,
            const std::optional<std::string>& dbh,
            const std::optional<std::string>& sh,
            const std::string& desc) {
            sqlite3* db = env.handle();

            sqlite3_stmt* sel{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id FROM address_program WHERE program_version=? AND prog_bytes=? AND "
                "COALESCE(derived_buffer_version,0)=COALESCE(?1,0) AND "
                "COALESCE(derived_buffer_schema_hash,'')=COALESCE(?2,'') AND "
                "COALESCE(soa_structs_hash,'')=COALESCE(?3,'') "
                "LIMIT 1;", -1, &sel, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare sel" });

            sqlite3_bind_int(sel, 1, program_version);
            sqlite3_bind_blob(sel, 2, prog_bytes.data(), (int)prog_bytes.size(), SQLITE_TRANSIENT);
            if (dbv) sqlite3_bind_int(sel, 3, *dbv); else sqlite3_bind_null(sel, 3);
            if (dbh) sqlite3_bind_text(sel, 4, dbh->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(sel, 4);
            if (sh)  sqlite3_bind_text(sel, 5, sh->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(sel, 5);

            rc = sqlite3_step(sel);
            if (rc == SQLITE_ROW) {
                int64_t id = sqlite3_column_int64(sel, 0);
                sqlite3_finalize(sel);
                return DbResult<int64_t>::Ok(id);
            }
            sqlite3_finalize(sel);

            sqlite3_stmt* ins{};
            rc = sqlite3_prepare_v2(db,
                "INSERT INTO address_program(program_version, prog_bytes, derived_buffer_version, derived_buffer_schema_hash, soa_structs_hash, description) "
                "VALUES(?,?,?,?,?,?);", -1, &ins, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare ins" });

            sqlite3_bind_int(ins, 1, program_version);
            sqlite3_bind_blob(ins, 2, prog_bytes.data(), (int)prog_bytes.size(), SQLITE_TRANSIENT);
            if (dbv) sqlite3_bind_int(ins, 3, *dbv); else sqlite3_bind_null(ins, 3);
            if (dbh) sqlite3_bind_text(ins, 4, dbh->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(ins, 4);
            if (sh)  sqlite3_bind_text(ins, 5, sh->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(ins, 5);
            sqlite3_bind_text(ins, 6, desc.c_str(), -1, SQLITE_TRANSIENT);

            rc = sqlite3_step(ins); sqlite3_finalize(ins);
            if (rc != SQLITE_DONE) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "insert" });
            return DbResult<int64_t>::Ok(sqlite3_last_insert_rowid(db));
        }

        static inline DbResult<AddressProgramRow> Impl_Get(DbEnv& env, int64_t id) {
            sqlite3* db = env.handle();
            sqlite3_stmt* st{};
            int rc = sqlite3_prepare_v2(db,
                "SELECT id, program_version, prog_bytes, derived_buffer_version, derived_buffer_schema_hash, soa_structs_hash, description "
                "FROM address_program WHERE id=?;", -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<AddressProgramRow>::Err({ map_sqlite_err(rc), rc, "prepare" });

            sqlite3_bind_int64(st, 1, id);
            rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DbResult<AddressProgramRow>::Err({ DbErrorKind::NotFound, rc, "not found" }); }

            AddressProgramRow r;
            r.id = sqlite3_column_int64(st, 0);
            r.program_version = sqlite3_column_int(st, 1);
            {
                const void* p = sqlite3_column_blob(st, 2); int n = sqlite3_column_bytes(st, 2);
                r.prog_bytes.assign((const uint8_t*)p, (const uint8_t*)p + n);
            }
            if (sqlite3_column_type(st, 3) == SQLITE_NULL) r.derived_buffer_version.reset(); else r.derived_buffer_version = sqlite3_column_int(st, 3);
            if (sqlite3_column_type(st, 4) == SQLITE_NULL) r.derived_buffer_schema_hash.reset(); else r.derived_buffer_schema_hash = std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 4)));
            if (sqlite3_column_type(st, 5) == SQLITE_NULL) r.soa_structs_hash.reset(); else r.soa_structs_hash = std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 5)));
            r.description = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
            sqlite3_finalize(st);

            return DbResult<AddressProgramRow>::Ok(std::move(r));
        }

        // Async

        std::future<DbResult<int64_t>> AddressProgramRepo::EnsureAsync(
            int32_t program_version,
            std::vector<uint8_t> prog_bytes,
            std::optional<int32_t> derived_buffer_version,
            std::optional<std::string> derived_buffer_schema_hash,
            std::optional<std::string> soa_structs_hash,
            std::string description,
            RetryPolicy rp) {
            return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
                [=, wb = std::move(prog_bytes), dbh = std::move(derived_buffer_schema_hash), sh = std::move(soa_structs_hash), d = std::move(description)](DbEnv& e) {
                    return Impl_Ensure(e, program_version, wb, derived_buffer_version, dbh, sh, d);
                });
        }

        std::future<DbResult<AddressProgramRow>> AddressProgramRepo::GetAsync(int64_t id, RetryPolicy rp) {
            return DBService::instance().submit_res<AddressProgramRow>(OpType::Read, Priority::Normal, rp,
                [=](DbEnv& e) { return Impl_Get(e, id); });
        }

    } // namespace db
} // namespace simcore
