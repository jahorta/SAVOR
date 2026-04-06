#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "IArchiveDb.h"

namespace simcore::db {

class SqliteArchiveDb final : public IArchiveDb {
public:
    explicit SqliteArchiveDb(sqlite3* db);

    bool CreateArchivePackage(
        const CreateArchivePackageCommand& command,
        std::int64_t* archive_package_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool AddArchiveItem(
        const AddArchiveItemCommand& command,
        std::int64_t* archive_item_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool RequestRehydrate(
        const RequestRehydrateCommand& command,
        std::int64_t* rehydrate_request_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool CompleteRehydrate(
        const CompleteRehydrateCommand& command,
        std::string* error_out = nullptr) override;

    bool FailRehydrate(
        const FailRehydrateCommand& command,
        std::string* error_out = nullptr) override;

    std::vector<events::EventEnvelope> ReadUnpublishedOutboxBatch(
        std::int64_t after_outbox_id,
        int max_batch_size) override;

    bool MarkOutboxPublished(
        std::int64_t outbox_id,
        types::UtcTimePoint published_at_utc) override;

    bool MarkOutboxPublishFailure(
        std::int64_t outbox_id,
        std::string_view last_error) override;
    retention::OutboxRetentionPreview PreviewOutboxRetention(
        const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
        types::UtcTimePoint now_utc,
        const retention::OutboxRetentionPolicy& policy) const override;
    bool PurgeOutboxThroughRetentionFloor(
        const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
        types::UtcTimePoint now_utc,
        const retention::OutboxRetentionPolicy& policy,
        int max_rows,
        int* rows_deleted_out = nullptr,
        std::string* error_out = nullptr) override;

    std::optional<ArchivePayloadRecord> ResolveArchivePayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;

    std::optional<ArchivePayloadRecord> ResolveArchivePayload(
        const events::EventEnvelope& envelope) const override;

private:
    sqlite3* db_ = nullptr;
};

} // namespace simcore::db
