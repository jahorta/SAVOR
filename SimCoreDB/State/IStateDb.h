#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../Common/Events/EventEnvelope.h"
#include "../Common/Types/UtcTimestamp.h"

namespace simcore::db {

struct ArtifactPayloadRecord {
    std::int64_t artifact_id = 0;
    std::int64_t savestate_id = 0;
    std::int64_t tas_variant_id = 0;
};

struct IStateDb {
    virtual ~IStateDb() = default;

    // Reads unpublished outbox rows in ascending outbox cursor order.
    virtual std::vector<events::EventEnvelope> ReadUnpublishedOutboxBatch(
        std::int64_t after_outbox_id,
        int max_batch_size) = 0;

    // Marks an outbox row as published.
    virtual bool MarkOutboxPublished(
        std::int64_t outbox_id,
        types::UtcTimePoint published_at_utc) = 0;

    // Increments delivery attempt count and stores the most recent publish error.
    virtual bool MarkOutboxPublishFailure(
        std::int64_t outbox_id,
        std::string_view last_error) = 0;

    // Resolves state/artifact event payload references for a specific event version.
    virtual std::optional<ArtifactPayloadRecord> ResolveArtifactPayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;
};

} // namespace simcore::db
