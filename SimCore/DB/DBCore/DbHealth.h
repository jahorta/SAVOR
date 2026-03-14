#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "DbEnv.h"

namespace simcore {
    namespace db {

        struct DbHealthReport {
            bool ok{ false };
            bool wal{ false };
            bool foreign_keys{ false };
            std::string synchronous;      // "OFF","NORMAL","FULL","EXTRA"
            int busy_timeout_ms{ 0 };
            bool schema_version_ok{ false };
            int schema_version{ 0 };
            bool quick_check_ran{ false };
            bool quick_check_ok{ false };
            std::string quick_check_msg;
            std::vector<std::string> notes;
        };

        DbHealthReport RunDbHealth(DbEnv& env, bool do_quick_check);

    } // namespace db
} // namespace simcore
