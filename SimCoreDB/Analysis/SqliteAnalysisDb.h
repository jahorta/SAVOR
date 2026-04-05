#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "IAnalysisDb.h"
#include "SqliteAnalysisPayloadResolvers.h"

namespace simcore::db::analysis {

class SqliteAnalysisDb final : public simcore::db::IAnalysisDb {
public:
    explicit SqliteAnalysisDb(sqlite3* db);

    std::vector<events::EventEnvelope> ReadUnpublishedOutboxBatch(
        std::int64_t after_outbox_id,
        int max_batch_size) override;

    bool MarkOutboxPublished(
        std::int64_t outbox_id,
        types::UtcTimePoint published_at_utc) override;

    bool MarkOutboxPublishFailure(
        std::int64_t outbox_id,
        std::string_view last_error) override;

    std::optional<SeedProbePayloadRecord> ResolveSeedProbePayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;

    std::optional<SeedProbePayloadRecord> ResolveSeedProbePayload(
        const events::EventEnvelope& envelope) const override;

    std::optional<BattlePayloadRecord> ResolveBattlePayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;

    std::optional<BattlePayloadRecord> ResolveBattlePayload(
        const events::EventEnvelope& envelope) const override;

private:
    sqlite3* db_ = nullptr;
    SqliteSeedProbePayloadRowResolver seed_probe_row_resolver_;
    SqliteBattlePayloadRowResolver battle_row_resolver_;
};

} // namespace simcore::db::analysis
