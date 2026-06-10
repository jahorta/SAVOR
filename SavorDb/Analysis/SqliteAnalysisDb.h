#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "IAnalysisDb.h"
#include "SqliteAnalysisPayloadResolvers.h"

namespace savor::db::analysis {

class SqliteAnalysisDb final : public savor::db::IAnalysisDb {
public:
    explicit SqliteAnalysisDb(sqlite3* db);

    std::optional<std::int64_t> LookupSeedProbeRunSavestateId(std::int64_t probe_run_id) const override;
    std::optional<std::int64_t> LookupSeedProbeResultId(std::int64_t probe_run_id) const override;
    std::optional<std::int64_t> LookupSeedProbeNeutralSeed(std::int64_t probe_run_id) const override;
    std::vector<SeedProbeGridSeedRow> ListSeedProbeGridSeeds(std::int64_t probe_run_id) const override;
    std::vector<SeedProbeUniqueSeedRow> ListSeedProbeUniqueSeeds(std::int64_t probe_run_id) const override;
    std::optional<SeedProbeUniqueSeedRow> GetSeedProbeUniqueSeed(std::int64_t unique_seed_id) const override;
    std::optional<AnalysisInputSetFrameRow> GetAnalysisInputFrame(std::int64_t input_frame_id) const override;
    std::vector<AnalysisInputSetFrameRow> ListAnalysisInputSetFrames(std::int64_t input_set_id) const override;
    bool EnsureSeedProbeInputFrame(
        std::int64_t main_axis_xy_id,
        std::int64_t cstick_axis_xy_id,
        std::int64_t trigger_axis_xy_id,
        std::int64_t* input_frame_id_out = nullptr,
        std::string* error_out = nullptr) override;
    bool EnsureSeedProbeUniqueSeedDelta(
        const RecordSeedProbeUniqueSeedCommand& command,
        bool* inserted_out = nullptr,
        std::int64_t* unique_seed_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool CreateSeedProbeSet(
        const CreateSeedProbeSetCommand& command,
        std::int64_t* probe_set_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool RequestSeedProbeRun(
        const RequestSeedProbeRunCommand& command,
        std::int64_t* probe_run_id_out = nullptr,
        std::string* error_out = nullptr) override;
    bool CreateSeedProbeRunForSet(
        std::int64_t probe_set_id,
        std::int64_t* probe_run_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<SeedProbeRunSnapshot> GetSeedProbeRun(
        std::int64_t probe_run_id) const override;
    bool SetSeedProbeRunNeutralSeed(
        std::int64_t probe_run_id,
        std::int64_t neutral_seed_value,
        std::string* error_out = nullptr) override;
    bool SetSeedProbeRunEntrySavestate(
        const SetSeedProbeRunEntrySavestateCommand& command,
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
    bool CreateBattleContextProbe(
        const CreateBattleContextProbeCommand& command,
        std::int64_t* context_probe_id_out = nullptr,
        std::string* error_out = nullptr) override;
    bool SetBattleContextProbeExecJobId(
        std::int64_t context_probe_id,
        std::int64_t exec_job_id,
        std::string* error_out = nullptr) override;
    bool CompleteBattleContextProbe(
        const CompleteBattleContextProbeCommand& command,
        std::string* error_out = nullptr) override;

    bool RecordBattleTurnJob(
        const RecordBattleTurnJobCommand& command,
        std::int64_t* turn_job_id_out = nullptr,
        std::string* error_out = nullptr) override;
    bool SetBattleTurnJobExecJobId(
        std::int64_t turn_job_id,
        std::int64_t exec_job_id,
        std::string* error_out = nullptr) override;
    bool UpdateBattleTurnJobResult(
        const RecordBattleTurnJobCommand& command,
        std::string* error_out = nullptr) override;

    bool CreateBattleSelectionPool(
        const CreateBattleSelectionPoolCommand& command,
        std::int64_t* selection_pool_id_out = nullptr,
        std::string* error_out = nullptr) override;
    bool EnsureBattleSelectionPool(
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
    bool UpdateBattleSetStatus(
        std::int64_t battle_set_id,
        BattleSetStatus status,
        std::optional<types::UtcTimePoint> completed_at_utc,
        std::string* error_out = nullptr) override;
    bool UpdateBattleTurnWaveStatus(
        std::int64_t wave_id,
        BattleTurnWaveStatus status,
        std::optional<types::UtcTimePoint> completed_at_utc,
        std::string* error_out = nullptr) override;

    std::optional<BattleSetSnapshot> GetBattleSet(std::int64_t battle_set_id) const override;
    std::vector<BattleSeedCandidateRow> ListBattleSeedCandidates(std::int64_t battle_set_id) const override;
    std::optional<BattleSeedCandidateRow> GetBattleSeedCandidate(std::int64_t seed_candidate_id) const override;
    std::optional<BattleTurnWaveSnapshot> GetBattleTurnWave(std::int64_t wave_id) const override;
    std::vector<BattleTurnWaveSnapshot> ListBattleTurnWaves(std::int64_t battle_set_id) const override;
    std::vector<BattleTurnWaveSnapshot> ListBattleTurnWavesForContextProbe(std::int64_t context_probe_id) const override;
    std::optional<BattleContextProbeSnapshot> GetBattleContextProbe(std::int64_t context_probe_id) const override;
    std::optional<BattleContextProbeSnapshot> GetBattleContextProbeForExecJob(std::int64_t exec_job_id) const override;
    std::optional<BattleContextProbeSnapshot> GetLatestBattleContextForWave(std::int64_t wave_id) const override;
    std::optional<BattleTurnJobSnapshot> GetBattleTurnJobForExecJob(std::int64_t exec_job_id) const override;
    std::vector<BattleTurnJobSnapshot> ListBattleTurnJobsForWave(std::int64_t wave_id) const override;
    std::vector<BattleTurnJobSnapshot> ListBattleTurnJobsForBattleTurn(std::int64_t battle_set_id, int turn_index) const override;
    std::vector<BattleSelectionDecisionRow> ListBattleSelectionDecisionsForPool(std::int64_t selection_pool_id) const override;

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

} // namespace savor::db::analysis
