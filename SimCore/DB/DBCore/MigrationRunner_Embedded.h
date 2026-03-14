#pragma once

#include <sqlite3.h>
#include <stdexcept>
#include <string>

#ifdef __INTELLISENSE__
#include <vector>
namespace simcore {
    namespace db {
        inline const std::vector<std::pair<std::string, std::string>> kEmbeddedMigrations{};
    }
}
#else
#include "GeneratedMigrations.h"  // This file is generated using a Visual Studio pre-build command.
#endif

#include "DbEnv.h"
#include "MigrationRunner.h"

namespace simcore::db {
    inline int ApplyEmbeddedMigrations(DbEnv& env) {
        sqlite3* db = env.handle();

        auto parse_version = [](const std::string& name) -> int {
            int v = 0;
            size_t i = 0;
            while (i < name.size() && name[i] >= '0' && name[i] <= '9') {
                v = v * 10 + (name[i] - '0');
                ++i;
            }
            return v;
            };

        std::vector<size_t> order;
        order.reserve(kEmbeddedMigrations.size());
        for (size_t i = 0; i < kEmbeddedMigrations.size(); ++i) order.push_back(i);
        for (size_t i = 0; i < order.size(); ++i) {
            size_t min_i = i;
            int min_v = parse_version(kEmbeddedMigrations[order[i]].first);
            for (size_t j = i + 1; j < order.size(); ++j) {
                int vj = parse_version(kEmbeddedMigrations[order[j]].first);
                if (vj < min_v) { min_v = vj; min_i = j; }
            }
            if (min_i != i) std::swap(order[i], order[min_i]);
        }

        const int current_version = GetCurrentSchemaVersion(env);

        for (size_t idx : order) {
            const auto& name = kEmbeddedMigrations[idx].first;
            const auto& sql = kEmbeddedMigrations[idx].second;
            const int ver = parse_version(name);
            if (ver <= current_version) continue;

            const bool script_has_tx =
                (sql.find("BEGIN") != std::string::npos) ||
                (sql.find("COMMIT") != std::string::npos);

            char* err = nullptr;

            if (!script_has_tx) {
                if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, &err) != SQLITE_OK) {
                    std::string msg = err ? err : "BEGIN failed"; sqlite3_free(err); throw std::runtime_error(msg);
                }
            }

            if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
                std::string msg = "Migration failed (" + name + "): " + (err ? err : "");
                sqlite3_free(err);
                sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
                throw std::runtime_error(msg);
            }

            if (!script_has_tx) {
                if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &err) != SQLITE_OK) {
                    std::string msg = err ? err : "COMMIT failed"; sqlite3_free(err); throw std::runtime_error(msg);
                }
            }
        }

        sqlite3_exec(env.handle(), "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);

        return GetCurrentSchemaVersion(env);
    }
} // namespace simcore::db
