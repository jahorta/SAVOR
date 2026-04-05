#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../Common/Events/EventEnvelope.h"
#include "../Common/Types/UtcTimestamp.h"

namespace simcore::db {

struct AuthoringPayloadRecord {
    std::int64_t template_id = 0;
    std::int64_t seed_probe_spec_id = 0;
    std::int64_t battle_run_spec_id = 0;
};

struct IAuthoringDb {
    virtual ~IAuthoringDb() = default;

    virtual std::vector<events::EventEnvelope> ReadUnpublishedOutboxBatch(
        std::int64_t after_outbox_id,
        int max_batch_size) = 0;

    virtual bool MarkOutboxPublished(
        std::int64_t outbox_id,
        types::UtcTimePoint published_at_utc) = 0;

    virtual bool MarkOutboxPublishFailure(
        std::int64_t outbox_id,
        std::string_view last_error) = 0;

    // Resolves authoring payload references for a specific event version.
    virtual std::optional<AuthoringPayloadRecord> ResolveAuthoringPayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;
};

} // namespace simcore::db
