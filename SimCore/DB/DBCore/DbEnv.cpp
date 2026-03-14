#include "DbEnv.h"

#include <sqlite3.h>
#include <stdexcept>
#include <filesystem>

namespace simcore::db {

        DbEnv::~DbEnv() {
            if (m_db) sqlite3_close(m_db);
        }

        std::unique_ptr<DbEnv> DbEnv::open(const std::string& path) {
            try {
                std::filesystem::path p(path);
                if (p.has_parent_path() && !p.parent_path().empty())
                    std::filesystem::create_directories(p.parent_path());
            }
            catch (...) {
                throw std::runtime_error("Failed to create parent directory for DB: " + path);
            }

            sqlite3* db = nullptr;
            if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
                throw std::runtime_error("Failed to open database");
            }
            
            // Pragmas
            sqlite3_exec(db, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "PRAGMA temp_store = MEMORY;", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "PRAGMA wal_autocheckpoint = 1000;", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "PRAGMA busy_timeout = 5000;", nullptr, nullptr, nullptr);

            return std::unique_ptr<DbEnv>(new DbEnv(db));
        }

        DbEnv::Tx::Tx(DbEnv& env) : m_db(env.handle()) {
            if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
                throw std::runtime_error("BEGIN failed");
            }
        }

        DbEnv::Tx::~Tx() {
            if (!m_committed) sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        }

        void DbEnv::Tx::commit() {
            if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
                throw std::runtime_error("COMMIT failed");
            }
            m_committed = true;
        }

} // namespace simcore::db
