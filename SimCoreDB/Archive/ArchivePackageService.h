#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "IArchiveDb.h"
#include "../Common/DbConfigPaths.h"
#include "../Common/Types/UtcTimestamp.h"

namespace simcore::db::archive {

struct ArchivePackageRetentionPolicy {
    bool include_workflow_event = true;
    bool include_trigger = false;
    bool include_outbox_message = false;
    std::size_t inline_payload_max_bytes = 4096;
};

struct CreateArchivePackageRequest {
    std::int64_t source_root_job_set_id = 0;
    ArchivePackageRetentionPolicy retention_policy{};
    types::UtcTimePoint created_at_utc = types::UtcNow();
    std::string source_context = "Execution";
    int schema_version = 1;
    int event_catalog_version = 1;
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct ArchivePackageFileSummary {
    std::string item_kind;
    std::filesystem::path relative_path;
    int row_count = 0;
    std::string checksum;
};

struct CreateArchivePackageResult {
    bool success = false;
    std::int64_t archive_package_id = 0;
    std::filesystem::path package_root;
    std::vector<ArchivePackageFileSummary> files;
    std::optional<std::string> error;
};

struct IArchivePackageService {
    virtual ~IArchivePackageService() = default;

    virtual CreateArchivePackageResult CreatePackage(const CreateArchivePackageRequest& request) = 0;
};

class SqliteArchivePackageService final : public IArchivePackageService {
public:
    SqliteArchivePackageService(
        sqlite3* execution_db,
        IArchiveDb* archive_db,
        DbConfigPaths config_paths);

    CreateArchivePackageResult CreatePackage(const CreateArchivePackageRequest& request) override;

private:
    sqlite3* execution_db_ = nullptr;
    IArchiveDb* archive_db_ = nullptr;
    DbConfigPaths config_paths_{};
};

} // namespace simcore::db::archive
