#include "MigrationRunner.h"
#include <sqlite3.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;

namespace simcore {
    namespace db {

        static bool table_exists(sqlite3* db, const char* name) {
            sqlite3_stmt* st = nullptr;
            const char* sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;";
            if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
            sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
            int rc = sqlite3_step(st);
            sqlite3_finalize(st);
            return rc == SQLITE_ROW;
        }

        int GetCurrentSchemaVersion(DbEnv& env) {
            sqlite3* db = env.handle();
            if (!table_exists(db, "schema_version")) return 0;

            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT version FROM schema_version LIMIT 1;", -1, &st, nullptr) != SQLITE_OK) {
                return 0;
            }
            int rc = sqlite3_step(st);
            int ver = 0;
            if (rc == SQLITE_ROW) ver = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);
            return ver;
        }

        static std::string slurp(const fs::path& p) {
            std::ifstream f(p, std::ios::binary);
            if (!f) throw std::runtime_error("Failed to open migration file: " + p.string());
            std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return s;
        }

        int ApplyMigrationsFromDir(DbEnv& env, const std::string& dir) {
            sqlite3* db = env.handle();

            // Collect *.sql files
            std::vector<fs::path> files;
            std::regex re_sql(R"(.*\.sql$)", std::regex::icase);
            for (const auto& entry : fs::directory_iterator(dir)) {
                if (!entry.is_regular_file()) continue;
                const auto& p = entry.path();
                if (std::regex_match(p.filename().string(), re_sql)) files.push_back(p);
            }
            std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
                return a.filename().string() < b.filename().string();
                });

            // Apply each script in order
            for (const auto& p : files) {
                const auto sql = slurp(p);

                char* err = nullptr;
                if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, &err) != SQLITE_OK) {
                    std::string msg = err ? err : "BEGIN failed";
                    sqlite3_free(err);
                    throw std::runtime_error(msg);
                }

                if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
                    std::string msg = "Migration failed (" + p.filename().string() + "): " + (err ? err : "");
                    sqlite3_free(err);
                    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
                    throw std::runtime_error(msg);
                }

                if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &err) != SQLITE_OK) {
                    std::string msg = err ? err : "COMMIT failed";
                    sqlite3_free(err);
                    throw std::runtime_error(msg);
                }
            }

            return GetCurrentSchemaVersion(env);
        }

    } // namespace db
} // namespace simcore
