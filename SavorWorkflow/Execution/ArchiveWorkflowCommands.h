#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "Archive/ArchivePackageService.h"
#include "Archive/RehydrateExecutor.h"
#include "Common/Retention/OutboxRetention.h"

namespace savor::runner::parallel::savordb {

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
    savor::db::types::UtcTimePoint older_than_utc{};
    savor::db::types::UtcTimePoint now_utc{};
    int max_candidates = 100;
    int max_outbox_purge_rows = 0;
    savor::db::retention::OutboxRetentionPolicy outbox_policy{};
};

struct WorkflowArchiveCommandRequest {
    savor::db::archive::ArchiveWorkflowSelection selection;
    savor::db::types::UtcTimePoint now_utc{};
    bool purge_after_verify = false;
    std::string trace_id;
    std::string archive_name;
    std::optional<std::string> archive_notes;
    savor::db::archive::ArchiveProgressSink progress_sink;
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

struct WorkflowArchiveCommandSummary {
    bool success = false;
    std::int64_t archive_package_id = 0;
    std::filesystem::path package_root;
    savor::db::archive::WorkflowArchivePreview preview{};
    PackageVerifySummary verify{};
    savor::db::archive::WorkflowArchivePurgeResult purge{};
    std::vector<std::string> blocking_reasons;
    std::vector<std::string> errors;
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
    int expected_workflows = 0;
    int execution_row_count = 0;
    int analysis_row_count = 0;
    int state_savestate_count = 0;
    int savestate_zip_entry_count = 0;
    std::vector<std::string> blocking_reasons;
};

struct RehydrateExecuteRequest {
    std::int64_t archive_package_id = 0;
    savor::db::types::UtcTimePoint now_utc{};
    std::string target_namespace;
    std::string trace_id;
    savor::db::archive::ArchiveProgressSink progress_sink;
};

struct RehydrateCleanupRequest {
    std::int64_t rehydrate_request_id = 0;
};

class ArchiveWorkflowCommands {
public:
    ArchiveWorkflowCommands(
        sqlite3* archive_db,
        savor::db::IArchiveDb* archive_service,
        savor::db::archive::IArchivePackageService* archive_package_service,
        savor::db::archive::IRehydrateExecutor* rehydrate_executor);

    ArchiveCommandSummary ArchivePreview(const ArchiveCommandRequest& request) const;
    WorkflowArchiveCommandSummary WorkflowArchivePreview(const WorkflowArchiveCommandRequest& request) const;
    WorkflowArchiveCommandSummary WorkflowArchiveExecute(const WorkflowArchiveCommandRequest& request) const;
    PackageVerifySummary PackageVerify(const PackageVerifyRequest& request) const;
    RehydratePreviewSummary RehydratePreview(const RehydratePreviewRequest& request) const;
    ArchiveCommandSummary RehydrateExecute(const RehydrateExecuteRequest& request) const;
    ArchiveCommandSummary RehydrateCleanup(const RehydrateCleanupRequest& request) const;

private:
    std::optional<std::filesystem::path> ManifestPathForPackage(std::int64_t archive_package_id, std::string* error_out) const;

    sqlite3* archive_db_ = nullptr;
    savor::db::IArchiveDb* archive_service_ = nullptr;
    savor::db::archive::IArchivePackageService* archive_package_service_ = nullptr;
    savor::db::archive::IRehydrateExecutor* rehydrate_executor_ = nullptr;
};

} // namespace savor::runner::parallel::savordb
