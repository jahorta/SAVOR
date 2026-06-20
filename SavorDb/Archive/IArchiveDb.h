#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../Common/Events/EventEnvelope.h"
#include "../Common/Events/EventPayloadViews.h"
#include "../Common/Types/UtcTimestamp.h"
#include "../Common/Retention/OutboxRetention.h"

namespace savor::db {

using ArchivePayloadRecord = events::ArchivePackagePayloadView;

struct CreateArchivePackageCommand {
    std::string source_context;
    std::int64_t source_root_job_set_id = 0;
    std::string source_scope_kind = "root_job_set";
    std::int64_t source_workflow_count = 0;
    std::optional<std::string> selection_summary;
    std::string archive_name;
    std::optional<std::string> archive_notes;
    types::UtcTimePoint created_at_utc{};
    std::int64_t schema_version = 1;
    int event_catalog_version = 1;
    types::UtcTimePoint time_range_start_utc{};
    types::UtcTimePoint time_range_end_utc{};
    std::string manifest_path;
    std::string checksum_status;
    std::string correlation_id;
    std::string causation_id;
};

struct AddArchiveWorkflowPackageMemberCommand {
    std::int64_t archive_package_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::optional<std::string> workflow_kind;
    std::optional<std::string> display_state;
    std::optional<std::string> root_scope_kind;
    std::optional<std::int64_t> root_scope_id;
    std::optional<types::UtcTimePoint> created_at_utc;
    std::optional<types::UtcTimePoint> completed_at_utc;
    int battle_final_victory_count = 0;
};

struct AddArchiveItemCommand {
    std::int64_t archive_package_id = 0;
    std::string item_kind;
    int item_count = 0;
    std::optional<std::string> blob_path;
    std::optional<std::string> checksum;
    types::UtcTimePoint indexed_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct RequestRehydrateCommand {
    std::int64_t archive_package_id = 0;
    std::string status;
    types::UtcTimePoint requested_at_utc{};
    std::string target_namespace;
    std::string correlation_id;
    std::string causation_id;
};

struct RehydrateMapEntry {
    std::string entity_kind;
    std::string old_id;
    std::string new_id;
};

struct CompleteRehydrateCommand {
    std::int64_t rehydrate_request_id = 0;
    std::string status;
    types::UtcTimePoint completed_at_utc{};
    std::vector<RehydrateMapEntry> entity_mappings;
    std::string correlation_id;
    std::string causation_id;
};

struct FailRehydrateCommand {
    std::int64_t rehydrate_request_id = 0;
    std::string status;
    types::UtcTimePoint completed_at_utc{};
    std::string error_text;
    std::string correlation_id;
    std::string causation_id;
};

struct IArchiveDb {
    virtual ~IArchiveDb() = default;

    virtual bool CreateArchivePackage(
        const CreateArchivePackageCommand& command,
        std::int64_t* archive_package_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool AddArchiveItem(
        const AddArchiveItemCommand& command,
        std::int64_t* archive_item_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool AddArchiveWorkflowPackageMember(
        const AddArchiveWorkflowPackageMemberCommand& command,
        std::string* error_out = nullptr) = 0;

    virtual bool RequestRehydrate(
        const RequestRehydrateCommand& command,
        std::int64_t* rehydrate_request_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool CompleteRehydrate(
        const CompleteRehydrateCommand& command,
        std::string* error_out = nullptr) = 0;

    virtual bool FailRehydrate(
        const FailRehydrateCommand& command,
        std::string* error_out = nullptr) = 0;

    virtual std::vector<events::EventEnvelope> ReadUnpublishedOutboxBatch(
        std::int64_t after_outbox_id,
        int max_batch_size) = 0;

    virtual bool MarkOutboxPublished(
        std::int64_t outbox_id,
        types::UtcTimePoint published_at_utc) = 0;

    virtual bool MarkOutboxPublishFailure(
        std::int64_t outbox_id,
        std::string_view last_error) = 0;

    virtual retention::OutboxRetentionPreview PreviewOutboxRetention(
        const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
        types::UtcTimePoint now_utc,
        const retention::OutboxRetentionPolicy& policy) const = 0;

    virtual bool PurgeOutboxThroughRetentionFloor(
        const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
        types::UtcTimePoint now_utc,
        const retention::OutboxRetentionPolicy& policy,
        int max_rows,
        int* rows_deleted_out = nullptr,
        std::string* error_out = nullptr) = 0;

    // Resolves archive payload references for a specific event version.
    virtual std::optional<ArchivePayloadRecord> ResolveArchivePayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<ArchivePayloadRecord> ResolveArchivePayload(
        const events::EventEnvelope& envelope) const = 0;
};

} // namespace savor::db
