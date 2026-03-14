#include "DbHealth.h"
#include <sqlite3.h>
#include <string>

namespace simcore {
    namespace db {

        static std::string pragma_text(sqlite3* db, const char* sql) {
            sqlite3_stmt* st{};
            std::string out;
            if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
                if (sqlite3_step(st) == SQLITE_ROW) {
                    const unsigned char* t = sqlite3_column_text(st, 0);
                    if (t) out.assign(reinterpret_cast<const char*>(t));
                }
            }
            sqlite3_finalize(st);
            return out;
        }

        static int pragma_int(sqlite3* db, const char* sql) {
            sqlite3_stmt* st{};
            int v = 0;
            if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
                if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
            }
            sqlite3_finalize(st);
            return v;
        }

        static bool table_exists(sqlite3* db, const char* name) {
            sqlite3_stmt* st{};
            const char* sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;";
            if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
            sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
            bool ok = (sqlite3_step(st) == SQLITE_ROW);
            sqlite3_finalize(st);
            return ok;
        }

        static int read_schema_version(sqlite3* db, bool& ok) {
            ok = false;
            if (!table_exists(db, "schema_version")) return 0;
            sqlite3_stmt* st{};
            int ver = 0;
            if (sqlite3_prepare_v2(db, "SELECT version FROM schema_version LIMIT 1;", -1, &st, nullptr) == SQLITE_OK) {
                if (sqlite3_step(st) == SQLITE_ROW) { ver = sqlite3_column_int(st, 0); ok = true; }
            }
            sqlite3_finalize(st);
            return ver;
        }

        DbHealthReport RunDbHealth(DbEnv& env, bool do_quick_check) {
            DbHealthReport r;
            sqlite3* db = env.handle();

            const std::string jmode = pragma_text(db, "PRAGMA journal_mode;");
            r.wal = (jmode == "wal");

            r.foreign_keys = pragma_int(db, "PRAGMA foreign_keys;") == 1;
            r.synchronous = pragma_text(db, "PRAGMA synchronous;");
            r.busy_timeout_ms = pragma_int(db, "PRAGMA busy_timeout;");

            r.schema_version = read_schema_version(db, r.schema_version_ok);

            if (do_quick_check) {
                r.quick_check_ran = true;
                sqlite3_stmt* st{};
                if (sqlite3_prepare_v2(db, "PRAGMA quick_check(1);", -1, &st, nullptr) == SQLITE_OK) {
                    if (sqlite3_step(st) == SQLITE_ROW) {
                        const unsigned char* t = sqlite3_column_text(st, 0);
                        if (t) r.quick_check_msg.assign(reinterpret_cast<const char*>(t));
                    }
                }
                sqlite3_finalize(st);
                r.quick_check_ok = (r.quick_check_msg == "ok" || r.quick_check_msg.empty());
            }

            if (!r.wal) r.notes.emplace_back("journal_mode != wal");
            if (!r.foreign_keys) r.notes.emplace_back("foreign_keys != ON");
            if (r.synchronous.empty()) r.notes.emplace_back("synchronous unreadable");
            if (!r.schema_version_ok) r.notes.emplace_back("schema_version missing/empty");

            r.ok = r.wal && r.foreign_keys && r.schema_version_ok && (!r.quick_check_ran || r.quick_check_ok);
            return r;
        }

    } // namespace db
} // namespace simcore
