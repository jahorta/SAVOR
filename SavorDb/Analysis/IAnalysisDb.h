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
#include "../../SavorCore/Phases/Programs/BattleRunner/BattleOutcome.h"

namespace savor::db {

using SeedProbePayloadRecord = events::AnalysisSeedProbePayloadView;
using BattlePayloadRecord = events::AnalysisBattlePayloadView;
using SpinePayloadRecord = events::AnalysisSpinePayloadView;
using BattleTurnOutcome = savor::battle::Outcome;

enum class BattleSetStatus {
    Unknown = 0,
    Active,
    Victory,
    Completed,
    NoSurvivors,
    Failed,
};

enum class BattleSeedCandidateSourceKind {
    Unknown = 0,
    SeedProbeUnique,
    Manual,
    Synthetic,
};

enum class BattleSeedCandidateStatus {
    Unknown = 0,
    Pending,
    Ready,
    Selected,
    Rejected,
};

enum class BattleTurnWaveStatus {
    Unknown = 0,
    Ready,
    Running,
    ContextProbing,
    Completed,
    NoSurvivors,
    Selected,
};

enum class BattleContextProbeStatus {
    Unknown = 0,
    Queued,
    Running,
    Succeeded,
    Failed,
};

enum class BattleTurnJobState {
    Unknown = 0,
    Queued,
    Running,
    Completed,
    Succeeded,
    Failed,
};

enum class BattleAdvancementCriterionKind {
    Unknown = 0,
    MaxVi,
    ViDelta,
    BestFakeAttacksByRngSeed,
};

enum class BattleAdvancementDecisionKind {
    Unknown = 0,
    Selected,
    NotSelected,
    Rejected,
};

enum class BattleManualFollowupStatus {
    Unknown = 0,
    Unreviewed,
    Recorded,
};

inline std::string_view ToDbString(BattleSetStatus value) {
    switch (value) {
    case BattleSetStatus::Active: return "ACTIVE";
    case BattleSetStatus::Victory: return "VICTORY";
    case BattleSetStatus::Completed: return "COMPLETED";
    case BattleSetStatus::NoSurvivors: return "NO_SURVIVORS";
    case BattleSetStatus::Failed: return "FAILED";
    default: return "";
    }
}

inline BattleSetStatus ParseBattleSetStatus(std::string_view value) {
    if (value == "ACTIVE") return BattleSetStatus::Active;
    if (value == "VICTORY") return BattleSetStatus::Victory;
    if (value == "COMPLETED" || value == "COMPLETE") return BattleSetStatus::Completed;
    if (value == "NO_SURVIVORS") return BattleSetStatus::NoSurvivors;
    if (value == "FAILED") return BattleSetStatus::Failed;
    return BattleSetStatus::Unknown;
}

inline std::string_view ToDbString(BattleSeedCandidateSourceKind value) {
    switch (value) {
    case BattleSeedCandidateSourceKind::SeedProbeUnique: return "SP_UNIQUE";
    case BattleSeedCandidateSourceKind::Manual: return "MANUAL";
    case BattleSeedCandidateSourceKind::Synthetic: return "SYNTHETIC";
    default: return "";
    }
}

inline BattleSeedCandidateSourceKind ParseBattleSeedCandidateSourceKind(std::string_view value) {
    if (value == "SP_UNIQUE") return BattleSeedCandidateSourceKind::SeedProbeUnique;
    if (value == "MANUAL") return BattleSeedCandidateSourceKind::Manual;
    if (value == "SYNTHETIC") return BattleSeedCandidateSourceKind::Synthetic;
    return BattleSeedCandidateSourceKind::Unknown;
}

inline std::string_view ToDbString(BattleSeedCandidateStatus value) {
    switch (value) {
    case BattleSeedCandidateStatus::Pending: return "PENDING";
    case BattleSeedCandidateStatus::Ready: return "READY";
    case BattleSeedCandidateStatus::Selected: return "SELECTED";
    case BattleSeedCandidateStatus::Rejected: return "REJECTED";
    default: return "";
    }
}

inline BattleSeedCandidateStatus ParseBattleSeedCandidateStatus(std::string_view value) {
    if (value == "PENDING") return BattleSeedCandidateStatus::Pending;
    if (value == "READY") return BattleSeedCandidateStatus::Ready;
    if (value == "SELECTED") return BattleSeedCandidateStatus::Selected;
    if (value == "REJECTED") return BattleSeedCandidateStatus::Rejected;
    return BattleSeedCandidateStatus::Unknown;
}

inline std::string_view ToDbString(BattleTurnWaveStatus value) {
    switch (value) {
    case BattleTurnWaveStatus::Ready: return "READY";
    case BattleTurnWaveStatus::Running: return "RUNNING";
    case BattleTurnWaveStatus::ContextProbing: return "CONTEXT_PROBING";
    case BattleTurnWaveStatus::Completed: return "COMPLETED";
    case BattleTurnWaveStatus::NoSurvivors: return "NO_SURVIVORS";
    case BattleTurnWaveStatus::Selected: return "SELECTED";
    default: return "";
    }
}

inline BattleTurnWaveStatus ParseBattleTurnWaveStatus(std::string_view value) {
    if (value == "READY") return BattleTurnWaveStatus::Ready;
    if (value == "RUNNING") return BattleTurnWaveStatus::Running;
    if (value == "CONTEXT_PROBING") return BattleTurnWaveStatus::ContextProbing;
    if (value == "COMPLETED") return BattleTurnWaveStatus::Completed;
    if (value == "NO_SURVIVORS") return BattleTurnWaveStatus::NoSurvivors;
    if (value == "SELECTED") return BattleTurnWaveStatus::Selected;
    return BattleTurnWaveStatus::Unknown;
}

inline std::string_view ToDbString(BattleContextProbeStatus value) {
    switch (value) {
    case BattleContextProbeStatus::Queued: return "QUEUED";
    case BattleContextProbeStatus::Running: return "RUNNING";
    case BattleContextProbeStatus::Succeeded: return "SUCCEEDED";
    case BattleContextProbeStatus::Failed: return "FAILED";
    default: return "";
    }
}

inline BattleContextProbeStatus ParseBattleContextProbeStatus(std::string_view value) {
    if (value == "QUEUED") return BattleContextProbeStatus::Queued;
    if (value == "RUNNING") return BattleContextProbeStatus::Running;
    if (value == "SUCCEEDED") return BattleContextProbeStatus::Succeeded;
    if (value == "FAILED") return BattleContextProbeStatus::Failed;
    return BattleContextProbeStatus::Unknown;
}

inline std::string_view ToDbString(BattleTurnJobState value) {
    switch (value) {
    case BattleTurnJobState::Queued: return "QUEUED";
    case BattleTurnJobState::Running: return "RUNNING";
    case BattleTurnJobState::Completed: return "COMPLETED";
    case BattleTurnJobState::Succeeded: return "SUCCEEDED";
    case BattleTurnJobState::Failed: return "FAILED";
    default: return "";
    }
}

inline BattleTurnJobState ParseBattleTurnJobState(std::string_view value) {
    if (value == "QUEUED") return BattleTurnJobState::Queued;
    if (value == "RUNNING") return BattleTurnJobState::Running;
    if (value == "COMPLETED") return BattleTurnJobState::Completed;
    if (value == "SUCCEEDED") return BattleTurnJobState::Succeeded;
    if (value == "FAILED") return BattleTurnJobState::Failed;
    return BattleTurnJobState::Unknown;
}

inline std::string_view ToDbString(BattleAdvancementCriterionKind value) {
    switch (value) {
    case BattleAdvancementCriterionKind::MaxVi: return "MAX_VI";
    case BattleAdvancementCriterionKind::ViDelta: return "VI_DELTA";
    case BattleAdvancementCriterionKind::BestFakeAttacksByRngSeed: return "BEST_FAKE_ATTACKS_BY_RNG_SEED";
    default: return "";
    }
}

inline BattleAdvancementCriterionKind ParseBattleAdvancementCriterionKind(std::string_view value) {
    if (value == "MAX_VI") return BattleAdvancementCriterionKind::MaxVi;
    if (value == "VI_DELTA") return BattleAdvancementCriterionKind::ViDelta;
    if (value == "BEST_FAKE_ATTACKS_BY_RNG_SEED") return BattleAdvancementCriterionKind::BestFakeAttacksByRngSeed;
    return BattleAdvancementCriterionKind::Unknown;
}

inline std::string_view ToDbString(BattleAdvancementDecisionKind value) {
    switch (value) {
    case BattleAdvancementDecisionKind::Selected: return "SELECTED";
    case BattleAdvancementDecisionKind::NotSelected: return "NOT_SELECTED";
    case BattleAdvancementDecisionKind::Rejected: return "REJECTED";
    default: return "";
    }
}

inline BattleAdvancementDecisionKind ParseBattleAdvancementDecisionKind(std::string_view value) {
    if (value == "SELECTED" || value == "WINNER") return BattleAdvancementDecisionKind::Selected;
    if (value == "NOT_SELECTED" || value == "DUPLICATE") return BattleAdvancementDecisionKind::NotSelected;
    if (value == "REJECTED") return BattleAdvancementDecisionKind::Rejected;
    return BattleAdvancementDecisionKind::Unknown;
}

inline std::string_view ToDbString(BattleManualFollowupStatus value) {
    switch (value) {
    case BattleManualFollowupStatus::Unreviewed: return "UNREVIEWED";
    case BattleManualFollowupStatus::Recorded: return "RECORDED";
    default: return "";
    }
}

inline BattleManualFollowupStatus ParseBattleManualFollowupStatus(std::string_view value) {
    if (value == "UNREVIEWED") return BattleManualFollowupStatus::Unreviewed;
    if (value == "RECORDED") return BattleManualFollowupStatus::Recorded;
    return BattleManualFollowupStatus::Unknown;
}

struct CreateSeedProbeSetCommand {
    std::string name;
    std::string probe_flavor;
    std::string breakpoint_policy_name;
    std::optional<std::int64_t> dungeon_segment_file_num;
    std::optional<std::string> dungeon_segment_file_letter;
    std::optional<std::string> dungeon_segment_code;
    std::string segment_source_kind;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct RequestSeedProbeRunCommand {
    std::int64_t probe_set_id = 0;
    std::int64_t entry_savestate_id = 0;
    std::int64_t seed_probe_spec_id = 0;
    int launch_samples_per_axis = 0;
    int codec_version = 1;
    std::string status;
    types::UtcTimePoint requested_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct RecordSeedProbeNeutralSeedCommand {
    std::int64_t probe_result_id = 0;
    std::int64_t neutral_seed_value = 0;
    std::string source_kind;
    types::UtcTimePoint recorded_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct SeedProbeRunSnapshot {
    std::int64_t probe_run_id = 0;
    std::int64_t probe_set_id = 0;
    std::int64_t seed_probe_spec_id = 0;
    std::int64_t entry_savestate_id = 0;
    int launch_samples_per_axis = 0;
    int codec_version = 0;
    std::string status;
    std::int64_t unique_input_set_id = 0;
    types::UtcTimePoint requested_at_utc{};
    std::optional<types::UtcTimePoint> completed_at_utc;
};

struct SetSeedProbeRunEntrySavestateCommand {
    std::int64_t probe_run_id = 0;
    std::int64_t entry_savestate_id = 0;
    types::UtcTimePoint updated_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct RecordSeedProbeGridSeedCommand {
    std::int64_t probe_result_id = 0;
    std::string source_family;
    std::int64_t axis_xy_id = 0;
    std::int64_t seed_value = 0;
    std::int64_t seed_delta = 0;
    types::UtcTimePoint recorded_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct SeedProbeGridSeedRow {
    std::int64_t grid_seed_id = 0;
    std::int64_t probe_result_id = 0;
    std::string source_family;
    std::int32_t axis_x = 0;
    std::int32_t axis_y = 0;
    std::int64_t seed_value = 0;
    std::int64_t seed_delta = 0;
};

struct SeedProbeUniqueSeedRow {
    std::int64_t unique_seed_id = 0;
    std::int64_t probe_result_id = 0;
    std::int64_t seed_value = 0;
    std::int64_t seed_delta = 0;
    std::int32_t main_x = 0;
    std::int32_t main_y = 0;
    std::int32_t cstick_x = 0;
    std::int32_t cstick_y = 0;
    std::int32_t trigger_x = 0;
    std::int32_t trigger_y = 0;
};

struct AnalysisInputSetFrameRow {
    std::int64_t input_frame_id = 0;
    int ordinal = 0;
    std::int32_t main_x = 0;
    std::int32_t main_y = 0;
    std::int32_t cstick_x = 0;
    std::int32_t cstick_y = 0;
    std::int32_t trigger_x = 0;
    std::int32_t trigger_y = 0;
};

struct RecordSeedProbeUniqueSeedCommand {
    std::int64_t probe_result_id = 0;
    std::int64_t input_frame_id = 0;
    std::int64_t seed_value = 0;
    std::int64_t seed_delta = 0;
    types::UtcTimePoint recorded_at_utc{};
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
    std::string correlation_id;
    std::string causation_id;
};

struct CreateBattleSetCommand {
    std::string name;
    std::int64_t entry_savestate_id = 0;
    std::int64_t battle_run_spec_id = 0;
    std::int64_t explorer_settings_id = 0;
    int launch_fake_attack_min = 0;
    int launch_fake_attack_max = 0;
    BattleSetStatus status = BattleSetStatus::Unknown;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct AddBattleSeedCandidateCommand {
    std::int64_t battle_set_id = 0;
    std::optional<std::int64_t> source_unique_seed_id;
    std::optional<std::int64_t> source_input_frame_id;
    std::int64_t seed_value = 0;
    BattleSeedCandidateSourceKind source_kind = BattleSeedCandidateSourceKind::Unknown;
    BattleSeedCandidateStatus candidate_status = BattleSeedCandidateStatus::Unknown;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct CreateBattleTurnWaveCommand {
    std::int64_t battle_set_id = 0;
    int turn_index = 0;
    std::optional<std::int64_t> context_probe_id;
    std::optional<std::int64_t> parent_wave_id;
    std::optional<std::int64_t> parent_turn_job_id;
    std::int64_t seed_candidate_id = 0;
    std::optional<std::int64_t> battle_advancement_pool_id;
    BattleTurnWaveStatus status = BattleTurnWaveStatus::Unknown;
    types::UtcTimePoint created_at_utc{};
    std::optional<types::UtcTimePoint> completed_at_utc;
    std::string correlation_id;
    std::string causation_id;
};

struct RecordBattleTurnJobCommand {
    std::int64_t wave_id = 0;
    std::optional<std::int64_t> exec_job_id;
    std::int64_t plan_id = 0;
    std::optional<std::int64_t> source_savestate_id;
    std::optional<std::int64_t> seed_candidate_id;
    std::optional<std::int64_t> authored_plan_id;
    std::optional<int> authored_turn_index;
    std::optional<std::string> resolved_turn_commands_blob;
    std::optional<std::string> resolved_turn_variant_key;
    int fake_attacks_this_turn = 0;
    int fake_attacks_used_before = 0;
    BattleTurnJobState job_state = BattleTurnJobState::Unknown;
    std::optional<types::UtcTimePoint> started_at_utc;
    std::optional<types::UtcTimePoint> ended_at_utc;
    bool has_results = false;
    std::optional<int> vi_start;
    std::optional<int> vi_end;
    std::optional<int> delta_vi;
    std::optional<std::int64_t> rng_seed;
    std::optional<BattleTurnOutcome> battle_outcome;
    std::optional<int> plan_materialize_err;
    std::optional<int> pred_passed;
    std::optional<int> pred_total;
    std::optional<int> pred_abort_run;
    std::optional<std::int64_t> output_savestate_id;
    std::optional<std::int64_t> applied_input_artifact_id;
    std::optional<std::int64_t> input_trace_artifact_id;
    std::optional<std::string> result_context_blob_base64;
    std::optional<int> result_context_version;
    std::optional<types::UtcTimePoint> recorded_at_utc;
    std::string correlation_id;
    std::string causation_id;
};

struct CreateBattleAdvancementPoolCommand {
    std::int64_t battle_set_id = 0;
    int turn_index = 0;
    std::string pool_name;
    BattleAdvancementCriterionKind criterion_kind = BattleAdvancementCriterionKind::Unknown;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct RecordBattleAdvancementDecisionCommand {
    std::int64_t battle_advancement_pool_id = 0;
    std::int64_t turn_job_id = 0;
    BattleAdvancementDecisionKind decision_kind = BattleAdvancementDecisionKind::Unknown;
    std::optional<std::string> decision_reason;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct UpsertBattleManualFollowupCommand {
    std::int64_t turn_job_id = 0;
    BattleManualFollowupStatus manual_followup_status = BattleManualFollowupStatus::Unknown;
    std::optional<std::int64_t> recorded_dtm_artifact_id;
    std::optional<std::int64_t> recorded_dtmini_artifact_id;
    std::optional<std::int64_t> recorded_sav_artifact_id;
    std::optional<std::string> note;
    types::UtcTimePoint updated_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct BattleSetSnapshot {
    std::int64_t battle_set_id = 0;
    std::string name;
    std::int64_t entry_savestate_id = 0;
    std::int64_t battle_run_spec_id = 0;
    std::int64_t explorer_settings_id = 0;
    int launch_fake_attack_min = 0;
    int launch_fake_attack_max = 0;
    BattleSetStatus status = BattleSetStatus::Unknown;
    types::UtcTimePoint created_at_utc{};
    std::optional<types::UtcTimePoint> completed_at_utc;
};

struct BattleSeedCandidateRow {
    std::int64_t seed_candidate_id = 0;
    std::int64_t battle_set_id = 0;
    std::optional<std::int64_t> source_unique_seed_id;
    std::optional<std::int64_t> source_input_frame_id;
    std::int64_t seed_value = 0;
    BattleSeedCandidateSourceKind source_kind = BattleSeedCandidateSourceKind::Unknown;
    BattleSeedCandidateStatus candidate_status = BattleSeedCandidateStatus::Unknown;
    types::UtcTimePoint created_at_utc{};
};

struct BattleTurnWaveSnapshot {
    std::int64_t wave_id = 0;
    std::int64_t battle_set_id = 0;
    int turn_index = 0;
    std::optional<std::int64_t> context_probe_id;
    std::optional<std::int64_t> parent_wave_id;
    std::optional<std::int64_t> parent_turn_job_id;
    std::int64_t seed_candidate_id = 0;
    std::optional<std::int64_t> battle_advancement_pool_id;
    BattleTurnWaveStatus status = BattleTurnWaveStatus::Unknown;
    types::UtcTimePoint created_at_utc{};
    std::optional<types::UtcTimePoint> completed_at_utc;
};

struct CreateBattleContextProbeCommand {
    std::int64_t wave_id = 0;
    std::int64_t source_savestate_id = 0;
    BattleContextProbeStatus probe_status = BattleContextProbeStatus::Unknown;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct CompleteBattleContextProbeCommand {
    std::int64_t exec_job_id = 0;
    BattleContextProbeStatus probe_status = BattleContextProbeStatus::Unknown;
    std::optional<std::string> context_blob;
    std::optional<int> context_version;
    types::UtcTimePoint recorded_at_utc{};
};

struct BattleContextProbeSnapshot {
    std::int64_t context_probe_id = 0;
    std::int64_t wave_id = 0;
    std::int64_t source_savestate_id = 0;
    std::optional<std::int64_t> exec_job_id;
    BattleContextProbeStatus probe_status = BattleContextProbeStatus::Unknown;
    std::optional<std::string> context_blob;
    std::optional<int> context_version;
    std::optional<types::UtcTimePoint> recorded_at_utc;
    types::UtcTimePoint created_at_utc{};
};

struct BattleTurnJobSnapshot {
    std::int64_t turn_job_id = 0;
    std::int64_t wave_id = 0;
    std::optional<std::int64_t> exec_job_id;
    std::int64_t plan_id = 0;
    std::optional<std::int64_t> source_savestate_id;
    std::optional<std::int64_t> seed_candidate_id;
    std::optional<std::int64_t> authored_plan_id;
    std::optional<int> authored_turn_index;
    std::optional<std::string> resolved_turn_commands_blob;
    std::optional<std::string> resolved_turn_variant_key;
    int fake_attacks_this_turn = 0;
    int fake_attacks_used_before = 0;
    BattleTurnJobState job_state = BattleTurnJobState::Unknown;
    std::optional<types::UtcTimePoint> started_at_utc;
    std::optional<types::UtcTimePoint> ended_at_utc;
    bool has_results = false;
    std::optional<int> vi_start;
    std::optional<int> vi_end;
    std::optional<int> delta_vi;
    std::optional<std::int64_t> rng_seed;
    std::optional<BattleTurnOutcome> battle_outcome;
    std::optional<int> plan_materialize_err;
    std::optional<int> pred_passed;
    std::optional<int> pred_total;
    std::optional<int> pred_abort_run;
    std::optional<std::int64_t> output_savestate_id;
    std::optional<std::int64_t> applied_input_artifact_id;
    std::optional<std::int64_t> input_trace_artifact_id;
    std::optional<std::string> result_context_blob_base64;
    std::optional<int> result_context_version;
    std::optional<types::UtcTimePoint> recorded_at_utc;
};

struct BattleAdvancementDecisionRow {
    std::int64_t battle_advancement_decision_id = 0;
    std::int64_t battle_advancement_pool_id = 0;
    std::int64_t turn_job_id = 0;
    BattleAdvancementDecisionKind decision_kind = BattleAdvancementDecisionKind::Unknown;
    std::optional<std::string> decision_reason;
    types::UtcTimePoint created_at_utc{};
};

struct IAnalysisDb {
    virtual ~IAnalysisDb() = default;

    virtual std::optional<std::int64_t> LookupSeedProbeRunSavestateId(std::int64_t probe_run_id) const = 0;
    virtual std::optional<std::int64_t> LookupSeedProbeResultId(std::int64_t probe_run_id) const = 0;
    virtual std::optional<std::int64_t> LookupSeedProbeNeutralSeed(std::int64_t probe_run_id) const = 0;
    virtual std::vector<SeedProbeGridSeedRow> ListSeedProbeGridSeeds(std::int64_t probe_run_id) const = 0;
    virtual std::vector<SeedProbeUniqueSeedRow> ListSeedProbeUniqueSeeds(std::int64_t probe_run_id) const = 0;
    virtual std::optional<SeedProbeUniqueSeedRow> GetSeedProbeUniqueSeed(std::int64_t unique_seed_id) const = 0;
    virtual std::optional<AnalysisInputSetFrameRow> GetAnalysisInputFrame(std::int64_t input_frame_id) const = 0;
    virtual std::vector<AnalysisInputSetFrameRow> ListAnalysisInputSetFrames(std::int64_t input_set_id) const = 0;
    virtual bool EnsureSeedProbeInputFrame(
        std::int64_t main_axis_xy_id,
        std::int64_t cstick_axis_xy_id,
        std::int64_t trigger_axis_xy_id,
        std::int64_t* input_frame_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual bool EnsureSeedProbeUniqueSeedDelta(
        const RecordSeedProbeUniqueSeedCommand& command,
        bool* inserted_out = nullptr,
        std::int64_t* unique_seed_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

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

    virtual bool SetSeedProbeRunEntrySavestate(
        const SetSeedProbeRunEntrySavestateCommand& command,
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

    virtual bool CreateBattleContextProbe(
        const CreateBattleContextProbeCommand& command,
        std::int64_t* context_probe_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool SetBattleContextProbeExecJobId(
        std::int64_t context_probe_id,
        std::int64_t exec_job_id,
        std::string* error_out = nullptr) = 0;

    virtual bool CompleteBattleContextProbe(
        const CompleteBattleContextProbeCommand& command,
        std::string* error_out = nullptr) = 0;

    virtual bool RecordBattleTurnJob(
        const RecordBattleTurnJobCommand& command,
        std::int64_t* turn_job_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool SetBattleTurnJobExecJobId(
        std::int64_t turn_job_id,
        std::int64_t exec_job_id,
        std::string* error_out = nullptr) = 0;

    virtual bool UpdateBattleTurnJobResult(
        const RecordBattleTurnJobCommand& command,
        std::string* error_out = nullptr) = 0;

    virtual bool CreateBattleAdvancementPool(
        const CreateBattleAdvancementPoolCommand& command,
        std::int64_t* battle_advancement_pool_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool EnsureBattleAdvancementPool(
        const CreateBattleAdvancementPoolCommand& command,
        std::int64_t* battle_advancement_pool_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool RecordBattleAdvancementDecision(
        const RecordBattleAdvancementDecisionCommand& command,
        std::int64_t* battle_advancement_decision_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool UpsertBattleManualFollowup(
        const UpsertBattleManualFollowupCommand& command,
        std::int64_t* manual_followup_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool UpdateBattleSetStatus(
        std::int64_t battle_set_id,
        BattleSetStatus status,
        std::optional<types::UtcTimePoint> completed_at_utc,
        std::string* error_out = nullptr) = 0;

    virtual bool UpdateBattleTurnWaveStatus(
        std::int64_t wave_id,
        BattleTurnWaveStatus status,
        std::optional<types::UtcTimePoint> completed_at_utc,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<BattleSetSnapshot> GetBattleSet(std::int64_t battle_set_id) const = 0;
    virtual std::vector<BattleSeedCandidateRow> ListBattleSeedCandidates(std::int64_t battle_set_id) const = 0;
    virtual std::optional<BattleSeedCandidateRow> GetBattleSeedCandidate(std::int64_t seed_candidate_id) const = 0;
    virtual std::optional<BattleTurnWaveSnapshot> GetBattleTurnWave(std::int64_t wave_id) const = 0;
    virtual std::vector<BattleTurnWaveSnapshot> ListBattleTurnWaves(std::int64_t battle_set_id) const = 0;
    virtual std::vector<BattleTurnWaveSnapshot> ListBattleTurnWavesForContextProbe(std::int64_t context_probe_id) const = 0;
    virtual std::optional<BattleContextProbeSnapshot> GetBattleContextProbe(std::int64_t context_probe_id) const = 0;
    virtual std::optional<BattleContextProbeSnapshot> GetBattleContextProbeForExecJob(std::int64_t exec_job_id) const = 0;
    virtual std::optional<BattleContextProbeSnapshot> GetLatestBattleContextForWave(std::int64_t wave_id) const = 0;
    virtual std::optional<BattleTurnJobSnapshot> GetBattleTurnJobForExecJob(std::int64_t exec_job_id) const = 0;
    virtual std::vector<BattleTurnJobSnapshot> ListBattleTurnJobsForWave(std::int64_t wave_id) const = 0;
    virtual std::vector<BattleTurnJobSnapshot> ListBattleTurnJobsForBattleTurn(std::int64_t battle_set_id, int turn_index) const = 0;
    virtual std::vector<BattleAdvancementDecisionRow> ListBattleAdvancementDecisionsForPool(std::int64_t battle_advancement_pool_id) const = 0;

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

} // namespace savor::db
