#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "ArchiveProgress.h"
#include "IArchiveDb.h"
#include "../Common/DbConfigPaths.h"
#include "../Common/Retention/OutboxRetention.h"
#include "../Common/Types/UtcTimestamp.h"
#include "../Execution/IExecutionDb.h"
#include "../UIRead/IUiReadDb.h"

namespace savor::runner::parallel::savordb {
class ArchiveWorkflowCommands;
}

namespace savor::db::archive {

struct ArchivePackageRetentionPolicy {
    bool include_workflow_event = true;
    bool include_trigger = false;
    bool include_outbox_message = false;
    std::size_t inline_payload_max_bytes = 4096;
};

struct CreateArchivePackageRequest {
    std::int64_t source_job_set_id = 0;
    ArchivePackageRetentionPolicy retention_policy{};
    types::UtcTimePoint created_at_utc = types::UtcNow();
    std::string source_context = "Execution";
    std::string archive_name;
    std::optional<std::string> archive_notes;
    std::int64_t schema_version = 0;
    int event_catalog_version = 1;
    std::string correlation_id;
    std::string causation_id;
};

struct ArchiveWorkflowSelection {
    std::vector<std::int64_t> workflow_instance_ids;
    std::string created_by_filter_snapshot;
    std::vector<std::int64_t> explicit_exclusions;
};

struct CreateWorkflowArchivePackageRequest {
    ArchiveWorkflowSelection selection;
    ArchivePackageRetentionPolicy retention_policy{};
    types::UtcTimePoint created_at_utc = types::UtcNow();
    std::string archive_name;
    std::optional<std::string> archive_notes;
    bool include_execution = true;
    bool include_analysis = true;
    bool include_ui_read_snapshot = true;
    bool include_state_savestates = true;
    std::string source_context = "Workflow";
    std::int64_t schema_version = 0;
    std::int64_t state_schema_version = 0;
    std::int64_t analysis_schema_version = 0;
    std::int64_t ui_read_schema_version = 0;
    int event_catalog_version = 1;
    std::string correlation_id;
    std::string causation_id;
    ArchiveProgressSink progress_sink;
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

struct WorkflowArchivePreview {
    bool success = false;
    int workflow_count = 0;
    int execution_row_count = 0;
    int analysis_row_count = 0;
    int ui_read_snapshot_row_count = 0;
    int savestate_count = 0;
    int shared_savestate_count = 0;
    int exclusive_savestate_count = 0;
    std::uint64_t savestate_bytes = 0;
    std::vector<std::string> purge_blockers;
    std::optional<std::string> error;
};

struct WorkflowArchivePurgeResult {
    bool success = false;
    int workflow_rows_deleted = 0;
    int execution_rows_deleted = 0;
    int analysis_rows_deleted = 0;
    int ui_read_rows_deleted = 0;
    int savestate_rows_deleted = 0;
    int artifact_rows_deleted = 0;
    int savestate_files_deleted = 0;
    std::vector<std::string> blockers;
    std::optional<std::string> error;
};

struct ArchiveCandidateRoot {
    std::int64_t job_set_id = 0;
    types::UtcTimePoint terminal_at_utc{};
};

struct ArchiveBatchPreview {
    std::vector<ArchiveCandidateRoot> candidates;
    retention::OutboxRetentionPreview outbox_retention{};
    std::optional<std::int64_t> ui_safe_floor_outbox_id;
    std::vector<std::string> blocking_reasons;
    std::int64_t purgeable_published_outbox_rows = 0;
};

struct ArchiveBatchResult {
    bool success = false;
    int candidates_considered = 0;
    int packages_written = 0;
    int source_job_sets_purged = 0;
    int outbox_rows_purged = 0;
    std::vector<std::int64_t> archived_job_set_ids;
    std::vector<std::string> errors;
};

struct IArchivePackageService {
    virtual ~IArchivePackageService() = default;

    virtual CreateArchivePackageResult CreatePackage(const CreateArchivePackageRequest& request) = 0;
    virtual WorkflowArchivePreview PreviewWorkflowArchive(
        const ArchiveWorkflowSelection& selection,
        std::string* error_out = nullptr) const = 0;
    virtual CreateArchivePackageResult CreateWorkflowPackage(const CreateWorkflowArchivePackageRequest& request) = 0;
    virtual std::vector<ArchiveCandidateRoot> ListArchiveCandidateRoots(
        types::UtcTimePoint older_than_utc,
        types::UtcTimePoint now_utc,
        int max_candidates,
        std::string* error_out = nullptr) const = 0;
    virtual ArchiveBatchPreview PreviewArchiveBatch(
        types::UtcTimePoint older_than_utc,
        types::UtcTimePoint now_utc,
        int max_candidates,
        const retention::OutboxRetentionPolicy& outbox_policy,
        std::string* error_out = nullptr) const = 0;
};

class SqliteArchivePackageService final : public IArchivePackageService {
public:
    SqliteArchivePackageService(
        sqlite3* execution_db,
        savor::db::IExecutionDb* execution_retention_db,
        savor::db::IUiReadDb* ui_read_db,
        IArchiveDb* archive_db,
        DbConfigPaths config_paths,
        sqlite3* state_db = nullptr,
        sqlite3* analysis_db = nullptr,
        sqlite3* ui_read_sqlite_db = nullptr);

    CreateArchivePackageResult CreatePackage(const CreateArchivePackageRequest& request) override;
    WorkflowArchivePreview PreviewWorkflowArchive(
        const ArchiveWorkflowSelection& selection,
        std::string* error_out = nullptr) const override;
    CreateArchivePackageResult CreateWorkflowPackage(const CreateWorkflowArchivePackageRequest& request) override;
    std::vector<ArchiveCandidateRoot> ListArchiveCandidateRoots(
        types::UtcTimePoint older_than_utc,
        types::UtcTimePoint now_utc,
        int max_candidates,
        std::string* error_out = nullptr) const override;
    ArchiveBatchPreview PreviewArchiveBatch(
        types::UtcTimePoint older_than_utc,
        types::UtcTimePoint now_utc,
        int max_candidates,
        const retention::OutboxRetentionPolicy& outbox_policy,
        std::string* error_out = nullptr) const override;
private:
    friend class ::savor::runner::parallel::savordb::ArchiveWorkflowCommands;

    WorkflowArchivePurgeResult PurgeWorkflowArchiveSource(
        const ArchiveWorkflowSelection& selection,
        std::int64_t archive_package_id,
        std::string* error_out = nullptr);
    bool PurgePublishedOutboxRowsBeforeFloor(
        std::int64_t safe_floor_outbox_id,
        int max_rows,
        int* rows_deleted_out,
        std::string* error_out) const;
    sqlite3* execution_db_ = nullptr;
    sqlite3* state_db_ = nullptr;
    sqlite3* analysis_db_ = nullptr;
    sqlite3* ui_read_sqlite_db_ = nullptr;
    savor::db::IExecutionDb* execution_retention_db_ = nullptr;
    savor::db::IUiReadDb* ui_read_db_ = nullptr;
    IArchiveDb* archive_db_ = nullptr;
    DbConfigPaths config_paths_{};
};

} // namespace savor::db::archive
