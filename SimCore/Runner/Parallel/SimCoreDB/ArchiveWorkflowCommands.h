#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "../../../../SimCoreDB/Archive/ArchivePackageService.h"
#include "../../../../SimCoreDB/Archive/RehydrateExecutor.h"
#include "../../../../SimCoreDB/Common/Retention/OutboxRetention.h"

namespace simcore::runner::parallel::simcoredb {

struct ArchiveCommandSummary {
    bool success = false;
    int candidate_count = 0;
    int package_count = 0;
    int purged_outbox_rows = 0;
    int purged_source_roots = 0;
    std::optional<std::int64_t> ui_safe_floor_outbox_id;
    std::optional<std::int64_t> retention_safe_floor_outbox_id;
    std::vector<std::string> blocking_reasons;
    std::vector<std::int64_t> request_ids;
    std::vector<std::string> errors;
};

struct ArchiveCommandRequest {
    simcore::db::types::UtcTimePoint older_than_utc{};
    simcore::db::types::UtcTimePoint now_utc{};
    int max_candidates = 100;
    int max_outbox_purge_rows = 0;
    simcore::db::retention::OutboxRetentionPolicy outbox_policy{};
    simcore::db::archive::ArchiveSourcePurgePolicy source_purge_policy{};
};

struct PackageVerifyRequest {
    std::int64_t archive_package_id = 0;
};

struct PackageVerifySummary {
    bool success = false;
    std::int64_t archive_package_id = 0;
    int manifest_file_count = 0;
    int manifest_row_total = 0;
    int archive_item_row_total = 0;
    int checksum_verified_files = 0;
    std::vector<std::string> blocking_reasons;
};

struct RehydratePreviewRequest {
    std::int64_t archive_package_id = 0;
    std::string target_namespace;
};

struct RehydratePreviewSummary {
    bool success = false;
    int manifest_row_total = 0;
    int manifest_file_count = 0;
    int expected_jobs = 0;
    std::vector<std::string> blocking_reasons;
};

struct RehydrateExecuteRequest {
    std::int64_t archive_package_id = 0;
    simcore::db::types::UtcTimePoint now_utc{};
    std::string target_namespace;
    std::string event_id_prefix;
};

struct RehydrateCleanupRequest {
    std::int64_t rehydrate_request_id = 0;
};

class ArchiveWorkflowCommands {
public:
    ArchiveWorkflowCommands(
        sqlite3* archive_db,
        simcore::db::IArchiveDb* archive_service,
        simcore::db::archive::IArchivePackageService* archive_package_service,
        simcore::db::archive::IRehydrateExecutor* rehydrate_executor);

    ArchiveCommandSummary ArchivePreview(const ArchiveCommandRequest& request) const;
    ArchiveCommandSummary ArchiveExecute(const ArchiveCommandRequest& request) const;
    PackageVerifySummary PackageVerify(const PackageVerifyRequest& request) const;
    RehydratePreviewSummary RehydratePreview(const RehydratePreviewRequest& request) const;
    ArchiveCommandSummary RehydrateExecute(const RehydrateExecuteRequest& request) const;
    ArchiveCommandSummary RehydrateCleanup(const RehydrateCleanupRequest& request) const;

private:
    std::optional<std::filesystem::path> ManifestPathForPackage(std::int64_t archive_package_id, std::string* error_out) const;

    sqlite3* archive_db_ = nullptr;
    simcore::db::IArchiveDb* archive_service_ = nullptr;
    simcore::db::archive::IArchivePackageService* archive_package_service_ = nullptr;
    simcore::db::archive::IRehydrateExecutor* rehydrate_executor_ = nullptr;
};

} // namespace simcore::runner::parallel::simcoredb
