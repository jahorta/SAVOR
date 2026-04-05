#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "IStateDb.h"

namespace simcore::db::state {

class SqliteStateDb final : public simcore::db::IStateDb {
public:
    explicit SqliteStateDb(sqlite3* db);

    std::vector<events::EventEnvelope> ReadUnpublishedOutboxBatch(
        std::int64_t after_outbox_id,
        int max_batch_size) override;

    bool MarkOutboxPublished(
        std::int64_t outbox_id,
        types::UtcTimePoint published_at_utc) override;

    bool MarkOutboxPublishFailure(
        std::int64_t outbox_id,
        std::string_view last_error) override;

    std::optional<ArtifactPayloadRecord> ResolveArtifactPayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;

    std::optional<ArtifactPayloadRecord> ResolveArtifactPayload(
        const events::EventEnvelope& envelope) const override;

private:
    std::optional<ArtifactPayloadRecord> ResolveArtifactPayloadForEvent(
        std::string_view event_type,
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const;

    sqlite3* db_ = nullptr;
};

} // namespace simcore::db::state
