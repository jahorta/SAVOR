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
#include "../Common/Retention/OutboxRetention.h"
#include "../Common/Types/UtcTimestamp.h"
#include "../Execution/IExecutionDb.h"
#include "../UIRead/IUiReadDb.h"

namespace savor::db::archive {

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
    std::int64_t schema_version = 0;
    int event_catalog_version = 1;
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

struct ArchiveCandidateRoot {
    std::int64_t root_job_set_id = 0;
    types::UtcTimePoint terminal_at_utc{};
};

enum class ArchiveSourcePurgeAction {
    None = 0,
    MarkArchived = 1,
    DeleteRows = 2,
};

struct ArchiveSourcePurgePolicy {
    ArchiveSourcePurgeAction source_action = ArchiveSourcePurgeAction::None;
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
    std::vector<std::int64_t> archived_root_job_set_ids;
    std::vector<std::string> errors;
};

struct IArchivePackageService {
    virtual ~IArchivePackageService() = default;

    virtual CreateArchivePackageResult CreatePackage(const CreateArchivePackageRequest& request) = 0;
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
    virtual ArchiveBatchResult ExecuteArchiveBatch(
        types::UtcTimePoint older_than_utc,
        types::UtcTimePoint now_utc,
        int max_candidates,
        int max_outbox_purge_rows,
        const retention::OutboxRetentionPolicy& outbox_policy,
        const ArchiveSourcePurgePolicy& source_purge_policy) = 0;
};

class SqliteArchivePackageService final : public IArchivePackageService {
public:
    SqliteArchivePackageService(
        sqlite3* execution_db,
        savor::db::IExecutionDb* execution_retention_db,
        savor::db::IUiReadDb* ui_read_db,
        IArchiveDb* archive_db,
        DbConfigPaths config_paths);

    CreateArchivePackageResult CreatePackage(const CreateArchivePackageRequest& request) override;
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
    ArchiveBatchResult ExecuteArchiveBatch(
        types::UtcTimePoint older_than_utc,
        types::UtcTimePoint now_utc,
        int max_candidates,
        int max_outbox_purge_rows,
        const retention::OutboxRetentionPolicy& outbox_policy,
        const ArchiveSourcePurgePolicy& source_purge_policy) override;

private:
    bool PurgePublishedOutboxRowsBeforeFloor(
        std::int64_t safe_floor_outbox_id,
        int max_rows,
        int* rows_deleted_out,
        std::string* error_out) const;
    bool ApplySourcePurgePolicyForRoot(
        std::int64_t root_job_set_id,
        ArchiveSourcePurgeAction action,
        std::int64_t archive_package_id,
        int* job_sets_affected_out,
        std::string* error_out) const;

    sqlite3* execution_db_ = nullptr;
    savor::db::IExecutionDb* execution_retention_db_ = nullptr;
    savor::db::IUiReadDb* ui_read_db_ = nullptr;
    IArchiveDb* archive_db_ = nullptr;
    DbConfigPaths config_paths_{};
};

} // namespace savor::db::archive
