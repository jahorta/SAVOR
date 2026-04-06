#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <sqlite3.h>

#include "IArchiveDb.h"
#include "../Common/Types/UtcTimestamp.h"

namespace simcore::db::archive {

struct RehydrateExecutionRequest {
    std::int64_t rehydrate_request_id = 0;
    types::UtcTimePoint now_utc = types::UtcNow();
    std::string event_id_prefix;
    std::string correlation_id;
    std::string causation_id;
};

struct RehydrateExecutionResult {
    bool success = false;
    std::string namespace_token;
    std::int64_t restored_job_count = 0;
    std::optional<std::string> error;
};

struct IRehydrateExecutor {
    virtual ~IRehydrateExecutor() = default;
    virtual RehydrateExecutionResult Execute(const RehydrateExecutionRequest& request) = 0;
};

class SqliteRehydrateExecutor final : public IRehydrateExecutor {
public:
    SqliteRehydrateExecutor(
        sqlite3* execution_db,
        sqlite3* archive_db,
        simcore::db::IArchiveDb* archive_service,
        std::filesystem::path archive_store_root);

    RehydrateExecutionResult Execute(const RehydrateExecutionRequest& request) override;

private:
    sqlite3* execution_db_ = nullptr;
    sqlite3* archive_db_ = nullptr;
    simcore::db::IArchiveDb* archive_service_ = nullptr;
    std::filesystem::path archive_store_root_{};
};

} // namespace simcore::db::archive
