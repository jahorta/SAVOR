#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../Common/Events/EventEnvelope.h"
#include "../Common/Types/UtcTimestamp.h"

namespace simcore::db {

struct SeedProbePayloadRecord {
    std::int64_t probe_set_id = 0;
    std::int64_t probe_run_id = 0;
    std::int64_t probe_result_id = 0;
};

struct BattlePayloadRecord {
    std::int64_t battle_set_id = 0;
    std::int64_t wave_id = 0;
    std::int64_t turn_job_id = 0;
};

struct IAnalysisDb {
    virtual ~IAnalysisDb() = default;

    virtual std::vector<events::EventEnvelope> ReadUnpublishedOutboxBatch(
        std::int64_t after_outbox_id,
        int max_batch_size) = 0;

    virtual bool MarkOutboxPublished(
        std::int64_t outbox_id,
        types::UtcTimePoint published_at_utc) = 0;

    virtual bool MarkOutboxPublishFailure(
        std::int64_t outbox_id,
        std::string_view last_error) = 0;

    // Seed-probe family payload resolver.
    virtual std::optional<SeedProbePayloadRecord> ResolveSeedProbePayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    // Battle-analysis family payload resolver.
    virtual std::optional<BattlePayloadRecord> ResolveBattlePayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;
};

} // namespace simcore::db
