#pragma once
#include <memory>
#include <string>

struct sqlite3;

namespace simcore::db {

    class DbEnv {
    public:
        class Tx {
        public:
            explicit Tx(DbEnv& env);
            ~Tx();
            void commit();
            sqlite3* handle() const { return m_db; }
        private:
            sqlite3* m_db{};
            bool m_committed{ false };
        };

        ~DbEnv();
        static std::unique_ptr<DbEnv> open(const std::string& path);
        sqlite3* handle() const { return m_db; }

    private:
        explicit DbEnv(sqlite3* db) : m_db(db) {}
        sqlite3* m_db{};
    };

} // namespace simcore::db
