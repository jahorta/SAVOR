#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <sqlite3.h>

#include "ArchiveProgress.h"
#include "IArchiveDb.h"
#include "../Common/Types/UtcTimestamp.h"

namespace savor::db::archive {

struct RehydrateExecutionRequest {
    std::int64_t rehydrate_request_id = 0;
    types::UtcTimePoint now_utc = types::UtcNow();
    std::string correlation_id;
    std::string causation_id;
    ArchiveProgressSink progress_sink;
};

struct RehydrateExecutionResult {
    bool success = false;
    std::string namespace_token;
    std::int64_t restored_job_count = 0;
    std::optional<std::string> error;
};

struct RehydratePackagePreviewRequest {
    std::int64_t archive_package_id = 0;
    std::string target_namespace;
};

struct RehydratePackagePreviewResult {
    bool success = false;
    std::string namespace_token;
    int manifest_file_count = 0;
    int manifest_row_total = 0;
    int workflow_count = 0;
    int job_count = 0;
    int execution_row_count = 0;
    int analysis_row_count = 0;
    int state_savestate_count = 0;
    int savestate_zip_entry_count = 0;
    std::vector<std::string> blocking_reasons;
    std::optional<std::string> error;
};

struct IRehydrateExecutor {
    virtual ~IRehydrateExecutor() = default;
    virtual RehydratePackagePreviewResult PreviewPackage(const RehydratePackagePreviewRequest& request) = 0;
    virtual RehydrateExecutionResult Execute(const RehydrateExecutionRequest& request) = 0;
};

class SqliteRehydrateExecutor final : public IRehydrateExecutor {
public:
    SqliteRehydrateExecutor(
        sqlite3* execution_db,
        sqlite3* archive_db,
        savor::db::IArchiveDb* archive_service,
        std::filesystem::path archive_store_root,
        sqlite3* state_db = nullptr,
        sqlite3* analysis_db = nullptr,
        std::filesystem::path object_store_root = {});

    RehydratePackagePreviewResult PreviewPackage(const RehydratePackagePreviewRequest& request) override;
    RehydrateExecutionResult Execute(const RehydrateExecutionRequest& request) override;

private:
    sqlite3* execution_db_ = nullptr;
    sqlite3* archive_db_ = nullptr;
    sqlite3* state_db_ = nullptr;
    sqlite3* analysis_db_ = nullptr;
    savor::db::IArchiveDb* archive_service_ = nullptr;
    std::filesystem::path archive_store_root_{};
    std::filesystem::path object_store_root_{};
};

} // namespace savor::db::archive
