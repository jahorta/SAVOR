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

    bool RequestSeedProbeRun(
        const RequestSeedProbeRunCommand& command,
        std::int64_t* probe_run_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool RecordSeedProbeNeutralSeed(
        const RecordSeedProbeNeutralSeedCommand& command,
        std::int64_t* neutral_seed_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool RecordSeedProbeGridSeed(
        const RecordSeedProbeGridSeedCommand& command,
        std::int64_t* grid_seed_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool RecordSeedProbeUniqueSeed(
        const RecordSeedProbeUniqueSeedCommand& command,
        std::int64_t* unique_seed_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool RecordSeedProbeEncounterProjection(
        const RecordSeedProbeEncounterProjectionCommand& command,
        std::int64_t* encounter_projection_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool CompleteSeedProbeRun(
        const CompleteSeedProbeRunCommand& command,
        std::int64_t* probe_result_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool CreateBattleSet(
        const CreateBattleSetCommand& command,
        std::int64_t* battle_set_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool AddBattleSeedCandidate(
        const AddBattleSeedCandidateCommand& command,
        std::int64_t* seed_candidate_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool CreateBattleTurnWave(
        const CreateBattleTurnWaveCommand& command,
        std::int64_t* wave_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool RecordBattleTurnJob(
        const RecordBattleTurnJobCommand& command,
        std::int64_t* turn_job_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool CreateBattleSelectionPool(
        const CreateBattleSelectionPoolCommand& command,
        std::int64_t* selection_pool_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool RecordBattleSelectionDecision(
        const RecordBattleSelectionDecisionCommand& command,
        std::int64_t* selection_decision_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool UpsertBattleTerminalFollowup(
        const UpsertBattleTerminalFollowupCommand& command,
        std::int64_t* terminal_followup_id_out = nullptr,
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

    std::optional<SpinePayloadRecord> ResolveSpinePayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;

    std::optional<SpinePayloadRecord> ResolveSpinePayload(
        const events::EventEnvelope& envelope) const override;

private:
    sqlite3* db_ = nullptr;
    SqliteSeedProbePayloadRowResolver seed_probe_row_resolver_;
    SqliteBattlePayloadRowResolver battle_row_resolver_;
    SqliteAnalysisSpinePayloadRowResolver spine_row_resolver_;
};

} // namespace simcore::db::analysis
