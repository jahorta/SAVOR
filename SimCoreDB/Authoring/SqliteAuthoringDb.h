#pragma once

#include <sqlite3.h>

#include "IAuthoringDb.h"

namespace simcore::db {

class SqliteAuthoringDb final : public IAuthoringDb {
public:
    explicit SqliteAuthoringDb(sqlite3* db);

    bool SaveSeedProbeSpec(
        const SaveSeedProbeSpecCommand& command,
        std::int64_t* seed_probe_spec_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool SaveTasSpec(
        const SaveTasSpecCommand& command,
        std::int64_t* tas_spec_id_out = nullptr,
        std::int64_t* tas_spec_base_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool SaveBattleRunSpec(
        const SaveBattleRunSpecCommand& command,
        std::int64_t* battle_run_spec_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool SavePlan(
        const SavePlanCommand& command,
        std::int64_t* plan_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool SavePredicateSpec(
        const SavePredicateSpecCommand& command,
        std::int64_t* predicate_spec_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool SaveExplorerSettings(
        const SaveExplorerSettingsCommand& command,
        std::int64_t* explorer_settings_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool SaveTemplate(
        const SaveTemplateCommand& command,
        std::int64_t* template_id_out = nullptr,
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

    std::optional<AuthoringPayloadRecord> ResolveAuthoringPayload(
        const events::EventEnvelope& envelope) const override;

    std::optional<AuthoringPayloadRecord> ResolveAuthoringPayload(
        std::string_view event_type,
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;

private:
    sqlite3* db_ = nullptr;
};

} // namespace simcore::db
