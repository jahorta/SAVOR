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

namespace simcore::db {

using SeedProbePayloadRecord = events::AnalysisSeedProbePayloadView;
using BattlePayloadRecord = events::AnalysisBattlePayloadView;
using SpinePayloadRecord = events::AnalysisSpinePayloadView;

struct CreateSeedProbeSetCommand {
    std::string name;
    std::string probe_flavor;
    std::string breakpoint_policy_name;
    std::optional<std::int64_t> dungeon_segment_file_num;
    std::optional<std::string> dungeon_segment_file_letter;
    std::optional<std::string> dungeon_segment_code;
    std::string segment_source_kind;
    types::UtcTimePoint created_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct RequestSeedProbeRunCommand {
    std::int64_t probe_set_id = 0;
    std::int64_t entry_savestate_id = 0;
    std::int64_t seed_probe_spec_id = 0;
    int codec_version = 1;
    std::string status;
    types::UtcTimePoint requested_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct RecordSeedProbeNeutralSeedCommand {
    std::int64_t probe_result_id = 0;
    std::int64_t neutral_seed_value = 0;
    std::string source_kind;
    types::UtcTimePoint recorded_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct SeedProbeRunSnapshot {
    std::int64_t probe_run_id = 0;
    std::int64_t seed_probe_spec_id = 0;
    std::int64_t entry_savestate_id = 0;
    int codec_version = 0;
    std::string status;
};

struct RecordSeedProbeGridSeedCommand {
    std::int64_t probe_result_id = 0;
    std::string source_family;
    std::int64_t axis_xy_id = 0;
    std::int64_t seed_value = 0;
    std::int64_t seed_delta = 0;
    types::UtcTimePoint recorded_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct SeedProbeGridSeedRow {
    std::int64_t probe_result_id = 0;
    std::string source_family;
    std::int32_t axis_x = 0;
    std::int32_t axis_y = 0;
    std::int64_t seed_value = 0;
    std::int64_t seed_delta = 0;
};

struct RecordSeedProbeUniqueSeedCommand {
    std::int64_t probe_result_id = 0;
    std::int64_t input_frame_id = 0;
    std::int64_t seed_value = 0;
    std::int64_t seed_delta = 0;
    types::UtcTimePoint recorded_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct RecordSeedProbeEncounterProjectionCommand {
    std::int64_t probe_run_id = 0;
    std::int64_t seed_value = 0;
    int option_ordinal = 0;
    std::string encounter_id;
    std::int64_t encounter_frame = 0;
    std::optional<std::int64_t> stutter_step_at;
    bool movement_required = false;
    types::UtcTimePoint recorded_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct CompleteSeedProbeRunCommand {
    std::int64_t probe_run_id = 0;
    std::optional<std::int64_t> neutral_seed_value;
    int grid_count = 0;
    int unique_count = 0;
    std::string result_status;
    std::string run_status;
    types::UtcTimePoint recorded_at_utc{};
    types::UtcTimePoint completed_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct CreateBattleSetCommand {
    std::string name;
    std::int64_t entry_savestate_id = 0;
    std::int64_t battle_run_spec_id = 0;
    std::int64_t explorer_settings_id = 0;
    std::string status;
    types::UtcTimePoint created_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct AddBattleSeedCandidateCommand {
    std::int64_t battle_set_id = 0;
    std::optional<std::int64_t> source_unique_seed_id;
    std::int64_t seed_value = 0;
    std::string source_kind;
    std::string candidate_status;
    types::UtcTimePoint created_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct CreateBattleTurnWaveCommand {
    std::int64_t battle_set_id = 0;
    int turn_index = 0;
    std::optional<std::int64_t> parent_wave_id;
    std::int64_t seed_candidate_id = 0;
    std::optional<std::int64_t> selection_pool_id;
    std::string status;
    types::UtcTimePoint created_at_utc{};
    std::optional<types::UtcTimePoint> completed_at_utc;
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct RecordBattleTurnJobCommand {
    std::int64_t wave_id = 0;
    std::optional<std::int64_t> exec_job_id;
    std::int64_t plan_id = 0;
    int fake_attacks_this_turn = 0;
    int fake_attacks_used_before = 0;
    std::string job_state;
    std::optional<types::UtcTimePoint> started_at_utc;
    std::optional<types::UtcTimePoint> ended_at_utc;
    bool has_results = false;
    std::optional<int> vi_start;
    std::optional<int> vi_end;
    std::optional<int> delta_vi;
    std::optional<std::int64_t> rng_seed;
    std::optional<int> battle_outcome;
    std::optional<int> plan_materialize_err;
    std::optional<int> pred_passed;
    std::optional<int> pred_total;
    std::optional<int> pred_abort_run;
    std::optional<std::int64_t> output_savestate_id;
    std::optional<types::UtcTimePoint> recorded_at_utc;
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct CreateBattleSelectionPoolCommand {
    std::int64_t battle_set_id = 0;
    int turn_index = 0;
    std::string pool_name;
    std::string criterion_kind;
    types::UtcTimePoint created_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct RecordBattleSelectionDecisionCommand {
    std::int64_t selection_pool_id = 0;
    std::int64_t turn_job_id = 0;
    std::string decision_kind;
    std::optional<std::string> decision_reason;
    types::UtcTimePoint created_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct UpsertBattleTerminalFollowupCommand {
    std::int64_t turn_job_id = 0;
    bool is_victory = false;
    std::string manual_followup_status;
    std::optional<std::int64_t> recorded_dtm_artifact_id;
    std::optional<std::int64_t> recorded_dtmini_artifact_id;
    std::optional<std::int64_t> recorded_sav_artifact_id;
    std::optional<std::string> note;
    types::UtcTimePoint updated_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct IAnalysisDb {
    virtual ~IAnalysisDb() = default;

    virtual std::optional<std::int64_t> LookupSeedProbeRunSavestateId(std::int64_t probe_run_id) const = 0;
    virtual std::optional<std::int64_t> LookupSeedProbeResultId(std::int64_t probe_run_id) const = 0;
    virtual std::optional<std::int64_t> LookupSeedProbeNeutralSeed(std::int64_t probe_run_id) const = 0;
    virtual std::vector<SeedProbeGridSeedRow> ListSeedProbeGridSeeds(std::int64_t probe_run_id) const = 0;
    virtual bool HasSeedProbeUniqueSeedDelta(std::int64_t probe_run_id, std::int64_t seed_delta) const = 0;

    virtual bool CreateSeedProbeSet(
        const CreateSeedProbeSetCommand& command,
        std::int64_t* probe_set_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool RequestSeedProbeRun(
        const RequestSeedProbeRunCommand& command,
        std::int64_t* probe_run_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool CreateSeedProbeRunForSet(
        std::int64_t probe_set_id,
        std::int64_t* probe_run_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<SeedProbeRunSnapshot> GetSeedProbeRun(
        std::int64_t probe_run_id) const = 0;

    virtual bool SetSeedProbeRunNeutralSeed(
        std::int64_t probe_run_id,
        std::int64_t neutral_seed_value,
        std::string* error_out = nullptr) = 0;

    virtual bool RecordSeedProbeNeutralSeed(
        const RecordSeedProbeNeutralSeedCommand& command,
        std::int64_t* neutral_seed_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool RecordSeedProbeGridSeed(
        const RecordSeedProbeGridSeedCommand& command,
        std::int64_t* grid_seed_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool RecordSeedProbeUniqueSeed(
        const RecordSeedProbeUniqueSeedCommand& command,
        std::int64_t* unique_seed_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool RecordSeedProbeEncounterProjection(
        const RecordSeedProbeEncounterProjectionCommand& command,
        std::int64_t* encounter_projection_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool CompleteSeedProbeRun(
        const CompleteSeedProbeRunCommand& command,
        std::int64_t* probe_result_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool CreateBattleSet(
        const CreateBattleSetCommand& command,
        std::int64_t* battle_set_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool AddBattleSeedCandidate(
        const AddBattleSeedCandidateCommand& command,
        std::int64_t* seed_candidate_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool CreateBattleTurnWave(
        const CreateBattleTurnWaveCommand& command,
        std::int64_t* wave_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool RecordBattleTurnJob(
        const RecordBattleTurnJobCommand& command,
        std::int64_t* turn_job_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool CreateBattleSelectionPool(
        const CreateBattleSelectionPoolCommand& command,
        std::int64_t* selection_pool_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool RecordBattleSelectionDecision(
        const RecordBattleSelectionDecisionCommand& command,
        std::int64_t* selection_decision_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool UpsertBattleTerminalFollowup(
        const UpsertBattleTerminalFollowupCommand& command,
        std::int64_t* terminal_followup_id_out = nullptr,
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

    // Seed-probe family payload resolver.
    virtual std::optional<SeedProbePayloadRecord> ResolveSeedProbePayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<SeedProbePayloadRecord> ResolveSeedProbePayload(
        const events::EventEnvelope& envelope) const = 0;

    // Battle-analysis family payload resolver.
    virtual std::optional<BattlePayloadRecord> ResolveBattlePayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<BattlePayloadRecord> ResolveBattlePayload(
        const events::EventEnvelope& envelope) const = 0;

    // Analysis-spine family payload resolver.
    virtual std::optional<SpinePayloadRecord> ResolveSpinePayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;

    virtual std::optional<SpinePayloadRecord> ResolveSpinePayload(
        const events::EventEnvelope& envelope) const = 0;
};

} // namespace simcore::db
