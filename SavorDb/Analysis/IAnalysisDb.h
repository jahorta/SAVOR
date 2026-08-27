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

enum class SeedProbeRunStatus {
    Unknown = 0,
    Survey,
    Search,
    Confirm,
    Completed,
    CompletedPartial,
    Failed,
    Invalidated,
};

enum class SeedProbeEndpoint {
    Unknown = 0,
    AfterRandSeedSet,
    RandSeedCommitted,
};

enum class SeedProbeEvidenceState {
    Unknown = 0,
    Observed,
    Provisional,
    Confirmed,
    Rejected,
};

enum class BattleSetStatus {
    Unknown = 0,
    Active,
    Victory,
    Completed,
    NoSurvivors,
    Failed,
};

enum class BattleContinuationMode {
    Unknown = 0,
    ManualSelection,
    AutomaticBestPerEndingRng,
};

enum class BattleSeedCandidateSourceKind {
    Unknown = 0,
    SeedProbeConfirmedResult,
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
    AwaitingSelection,
    PlanComplete,
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
    SelectedForCompletion,
    DuplicateEndingRng,
    Recommended,
    NotRecommended,
};

enum class BattleManualFollowupStatus {
    Unknown = 0,
    Unreviewed,
    Recorded,
};

enum class RngEffectKind {
    Unknown = 0,
    Preserve,
    AdvanceFixed,
    Variable,
};

inline std::string_view ToDbString(RngEffectKind value) {
    switch (value) {
    case RngEffectKind::Preserve: return "PRESERVE";
    case RngEffectKind::AdvanceFixed: return "ADVANCE_FIXED";
    case RngEffectKind::Variable: return "VARIABLE";
    default: return "";
    }
}

inline RngEffectKind ParseRngEffectKind(std::string_view value) {
    if (value == "PRESERVE") return RngEffectKind::Preserve;
    if (value == "ADVANCE_FIXED") return RngEffectKind::AdvanceFixed;
    if (value == "VARIABLE") return RngEffectKind::Variable;
    return RngEffectKind::Unknown;
}

struct ResolvedRngLineage {
    std::string selected_seed_ref_kind;
    std::int64_t selected_seed_ref_id = 0;
    std::optional<std::int64_t> selected_seed_value;
    std::optional<std::int64_t> current_seed_value;
    std::int64_t cumulative_fixed_draw_count = 0;
    bool requires_probe = false;
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

inline std::string_view ToDbString(BattleContinuationMode value) {
    switch (value) {
    case BattleContinuationMode::ManualSelection: return "manual_selection";
    case BattleContinuationMode::AutomaticBestPerEndingRng:
        return "automatic_best_per_ending_rng";
    case BattleContinuationMode::Unknown: break;
    }
    return "";
}

inline BattleContinuationMode ParseBattleContinuationMode(std::string_view value) {
    if (value == "manual_selection") return BattleContinuationMode::ManualSelection;
    if (value == "automatic_best_per_ending_rng") {
        return BattleContinuationMode::AutomaticBestPerEndingRng;
    }
    return BattleContinuationMode::Unknown;
}

inline std::string_view ToDbString(BattleSeedCandidateSourceKind value) {
    switch (value) {
    case BattleSeedCandidateSourceKind::SeedProbeConfirmedResult: return "SP_CONFIRMED_RESULT";
    case BattleSeedCandidateSourceKind::Manual: return "MANUAL";
    case BattleSeedCandidateSourceKind::Synthetic: return "SYNTHETIC";
    default: return "";
    }
}

inline BattleSeedCandidateSourceKind ParseBattleSeedCandidateSourceKind(std::string_view value) {
    if (value == "SP_CONFIRMED_RESULT") return BattleSeedCandidateSourceKind::SeedProbeConfirmedResult;
    if (value == "MANUAL") return BattleSeedCandidateSourceKind::Manual;
    if (value == "SYNTHETIC") return BattleSeedCandidateSourceKind::Synthetic;
    return BattleSeedCandidateSourceKind::Unknown;
}

inline std::string_view ToDbString(SeedProbeRunStatus value) {
    switch (value) {
    case SeedProbeRunStatus::Survey: return "SURVEY";
    case SeedProbeRunStatus::Search: return "SEARCH";
    case SeedProbeRunStatus::Confirm: return "CONFIRM";
    case SeedProbeRunStatus::Completed: return "COMPLETED";
    case SeedProbeRunStatus::CompletedPartial: return "COMPLETED_PARTIAL";
    case SeedProbeRunStatus::Failed: return "FAILED";
    case SeedProbeRunStatus::Invalidated: return "INVALIDATED";
    default: return "";
    }
}

inline SeedProbeRunStatus ParseSeedProbeRunStatus(std::string_view value) {
    if (value == "SURVEY") return SeedProbeRunStatus::Survey;
    if (value == "SEARCH") return SeedProbeRunStatus::Search;
    if (value == "CONFIRM") return SeedProbeRunStatus::Confirm;
    if (value == "COMPLETED") return SeedProbeRunStatus::Completed;
    if (value == "COMPLETED_PARTIAL") return SeedProbeRunStatus::CompletedPartial;
    if (value == "FAILED") return SeedProbeRunStatus::Failed;
    if (value == "INVALIDATED") return SeedProbeRunStatus::Invalidated;
    return SeedProbeRunStatus::Unknown;
}

inline std::string_view ToDbString(SeedProbeEndpoint value) {
    switch (value) {
    case SeedProbeEndpoint::AfterRandSeedSet:
        return "AFTER_RAND_SEED_SET";
    case SeedProbeEndpoint::RandSeedCommitted:
        return "RAND_SEED_COMMITTED";
    default:
        return "";
    }
}

inline SeedProbeEndpoint ParseSeedProbeEndpoint(std::string_view value) {
    if (value == "AFTER_RAND_SEED_SET") {
        return SeedProbeEndpoint::AfterRandSeedSet;
    }
    if (value == "RAND_SEED_COMMITTED") {
        return SeedProbeEndpoint::RandSeedCommitted;
    }
    return SeedProbeEndpoint::Unknown;
}

inline std::string_view ToDbString(SeedProbeEvidenceState value) {
    switch (value) {
    case SeedProbeEvidenceState::Observed: return "OBSERVED";
    case SeedProbeEvidenceState::Provisional: return "PROVISIONAL";
    case SeedProbeEvidenceState::Confirmed: return "CONFIRMED";
    case SeedProbeEvidenceState::Rejected: return "REJECTED";
    default: return "";
    }
}

inline SeedProbeEvidenceState ParseSeedProbeEvidenceState(std::string_view value) {
    if (value == "OBSERVED") return SeedProbeEvidenceState::Observed;
    if (value == "PROVISIONAL") return SeedProbeEvidenceState::Provisional;
    if (value == "CONFIRMED") return SeedProbeEvidenceState::Confirmed;
    if (value == "REJECTED") return SeedProbeEvidenceState::Rejected;
    return SeedProbeEvidenceState::Unknown;
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
    case BattleTurnWaveStatus::AwaitingSelection: return "AWAITING_SELECTION";
    case BattleTurnWaveStatus::PlanComplete: return "PLAN_COMPLETE";
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
    if (value == "AWAITING_SELECTION") return BattleTurnWaveStatus::AwaitingSelection;
    if (value == "PLAN_COMPLETE") return BattleTurnWaveStatus::PlanComplete;
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
    case BattleAdvancementDecisionKind::SelectedForCompletion: return "SELECTED_FOR_COMPLETION";
    case BattleAdvancementDecisionKind::DuplicateEndingRng: return "DUPLICATE_ENDING_RNG";
    case BattleAdvancementDecisionKind::Recommended: return "RECOMMENDED";
    case BattleAdvancementDecisionKind::NotRecommended: return "NOT_RECOMMENDED";
    default: return "";
    }
}

inline BattleAdvancementDecisionKind ParseBattleAdvancementDecisionKind(std::string_view value) {
    if (value == "SELECTED" || value == "WINNER") return BattleAdvancementDecisionKind::Selected;
    if (value == "NOT_SELECTED" || value == "DUPLICATE") return BattleAdvancementDecisionKind::NotSelected;
    if (value == "REJECTED") return BattleAdvancementDecisionKind::Rejected;
    if (value == "SELECTED_FOR_COMPLETION") return BattleAdvancementDecisionKind::SelectedForCompletion;
    if (value == "DUPLICATE_ENDING_RNG") return BattleAdvancementDecisionKind::DuplicateEndingRng;
    if (value == "RECOMMENDED") return BattleAdvancementDecisionKind::Recommended;
    if (value == "NOT_RECOMMENDED") return BattleAdvancementDecisionKind::NotRecommended;
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
    std::string materialization_key;
    std::int64_t probe_set_id = 0;
    std::int64_t entry_savestate_id = 0;
    std::int64_t seed_probe_spec_id = 0;
    int launch_samples_per_axis = 0;
    int codec_version = 1;
    SeedProbeRunStatus status = SeedProbeRunStatus::Survey;
    types::UtcTimePoint requested_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct SeedProbeRunSnapshot {
    std::int64_t probe_run_id = 0;
    std::string materialization_key;
    std::int64_t probe_set_id = 0;
    std::string probe_flavor;
    std::int64_t seed_probe_spec_id = 0;
    std::int64_t entry_savestate_id = 0;
    int launch_samples_per_axis = 0;
    int codec_version = 0;
    SeedProbeRunStatus status = SeedProbeRunStatus::Unknown;
    std::int64_t accepted_input_set_id = 0;
    types::UtcTimePoint requested_at_utc{};
    std::optional<types::UtcTimePoint> completed_at_utc;
    SeedProbeEndpoint established_endpoint = SeedProbeEndpoint::Unknown;
    std::optional<std::int64_t> established_endpoint_source_job_id;
    std::optional<SeedProbeEndpoint> conflicting_endpoint;
    std::optional<std::int64_t> conflicting_endpoint_source_job_id;
    std::optional<std::string> invalidation_diagnostic;
    std::optional<types::UtcTimePoint> invalidated_at_utc;
};

enum class SeedProbeEndpointObservationDisposition {
    Established = 0,
    Matched,
    Invalidated,
    AlreadyInvalidatedMatching,
    AlreadyInvalidatedConflicting,
};

struct SetSeedProbeRunEntrySavestateCommand {
    std::int64_t probe_run_id = 0;
    std::int64_t entry_savestate_id = 0;
    types::UtcTimePoint updated_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct RecordSeedProbeObservationCommand {
    std::int64_t probe_run_id = 0;
    std::int64_t input_frame_id = 0;
    std::int64_t source_job_id = 0;
    std::uint32_t seed_value = 0;
    std::uint64_t origin_worker_id = 0;
    std::uint64_t origin_process_generation = 0;
    std::uint64_t origin_workset_epoch = 0;
    std::string terminal_sha256;
    std::optional<std::int64_t> confirmation_of_probe_result_id;
    SeedProbeEndpoint endpoint = SeedProbeEndpoint::Unknown;
    std::string endpoint_mismatch_diagnostic;
    types::UtcTimePoint recorded_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct SeedProbeResultRow {
    std::int64_t probe_result_id = 0;
    std::int64_t probe_run_id = 0;
    std::int64_t input_frame_id = 0;
    std::int64_t source_job_id = 0;
    std::uint32_t seed_value = 0;
    std::uint64_t origin_worker_id = 0;
    std::uint64_t origin_process_generation = 0;
    std::uint64_t origin_workset_epoch = 0;
    std::string terminal_sha256;
    std::optional<std::int64_t> confirmation_of_probe_result_id;
    SeedProbeEvidenceState evidence_state = SeedProbeEvidenceState::Unknown;
    types::UtcTimePoint recorded_at_utc{};
};

struct RecordSeedProbeObservationReceipt {
    bool inserted = false;
    SeedProbeResultRow observation;
    SeedProbeEndpointObservationDisposition endpoint_disposition =
        SeedProbeEndpointObservationDisposition::Established;
    SeedProbeEndpoint established_endpoint = SeedProbeEndpoint::Unknown;
    std::optional<SeedProbeEndpoint> conflicting_endpoint;
    bool run_invalidated = false;
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

struct TransitionSeedProbeEvidenceCommand {
    std::int64_t probe_result_id = 0;
    SeedProbeEvidenceState expected_state = SeedProbeEvidenceState::Unknown;
    SeedProbeEvidenceState new_state = SeedProbeEvidenceState::Unknown;
    types::UtcTimePoint changed_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct UpdateSeedProbeRunStatusCommand {
    std::int64_t probe_run_id = 0;
    SeedProbeRunStatus expected_status = SeedProbeRunStatus::Unknown;
    SeedProbeRunStatus new_status = SeedProbeRunStatus::Unknown;
    std::optional<types::UtcTimePoint> completed_at_utc;
    types::UtcTimePoint changed_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct ReplaceSeedProbeAcceptedInputFramesCommand {
    std::int64_t probe_run_id = 0;
    std::vector<std::int64_t> input_frame_ids;
    types::UtcTimePoint replaced_at_utc{};
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

struct CreateBattleSetCommand {
    std::string name;
    std::int64_t entry_savestate_id = 0;
    std::int64_t battle_plan_id = 0;
    std::string battle_plan_fingerprint;
    BattleContinuationMode continuation_mode = BattleContinuationMode::Unknown;
    bool continue_automatic_exploration_after_victory = false;
    int launch_fake_attack_min = 0;
    int launch_fake_attack_max = 0;
    BattleSetStatus status = BattleSetStatus::Unknown;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct AddBattleSeedCandidateCommand {
    std::int64_t battle_set_id = 0;
    std::optional<std::int64_t> source_probe_result_id;
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
    std::int64_t battle_plan_id = 0;
    std::string battle_plan_fingerprint;
    BattleContinuationMode continuation_mode = BattleContinuationMode::Unknown;
    bool continue_automatic_exploration_after_victory = false;
    int launch_fake_attack_min = 0;
    int launch_fake_attack_max = 0;
    BattleSetStatus status = BattleSetStatus::Unknown;
    types::UtcTimePoint created_at_utc{};
    std::optional<types::UtcTimePoint> completed_at_utc;
};

struct BattleSeedCandidateRow {
    std::int64_t seed_candidate_id = 0;
    std::int64_t battle_set_id = 0;
    std::optional<std::int64_t> source_probe_result_id;
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
    std::string materialization_key;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t source_savestate_artifact_id = 0;
    std::string source_savestate_sha256;
    std::int64_t full_phase_program_kind = 0;
    std::int64_t full_phase_program_version = 0;
    std::string full_phase_canonical_id;
    std::int64_t full_phase_contract_revision = 0;
    std::string full_phase_sha256;
    std::string module_canonical_id;
    std::int64_t module_revision = 0;
    std::string module_sha256;
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
    std::optional<std::int64_t> context_artifact_id;
    std::optional<std::string> worker_terminal_sha256;
    std::optional<std::uint32_t> entry_pc;
    std::optional<std::uint64_t> entry_vi_count;
    std::optional<std::uint64_t> entry_epoch;
    std::optional<std::uint32_t> capture_pc;
    std::optional<std::uint64_t> capture_vi_count;
    std::optional<std::uint64_t> capture_epoch;
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
    std::optional<std::int64_t> context_artifact_id;
    std::optional<std::string> worker_terminal_sha256;
    std::optional<std::uint32_t> entry_pc;
    std::optional<std::uint64_t> entry_vi_count;
    std::optional<std::uint64_t> entry_epoch;
    std::optional<std::uint32_t> capture_pc;
    std::optional<std::uint64_t> capture_vi_count;
    std::optional<std::uint64_t> capture_epoch;
    std::optional<std::string> materialization_key;
    std::optional<std::int64_t> workflow_instance_id;
    std::optional<std::int64_t> workflow_step_id;
    std::optional<std::int64_t> source_savestate_artifact_id;
    std::optional<std::string> source_savestate_sha256;
    std::optional<std::int64_t> full_phase_program_kind;
    std::optional<std::int64_t> full_phase_program_version;
    std::optional<std::string> full_phase_canonical_id;
    std::optional<std::int64_t> full_phase_contract_revision;
    std::optional<std::string> full_phase_sha256;
    std::optional<std::string> module_canonical_id;
    std::optional<std::int64_t> module_revision;
    std::optional<std::string> module_sha256;
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

struct CreateBattleCompletionCommand {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::optional<std::int64_t> exec_job_id;
    std::int64_t battle_set_id = 0;
    std::int64_t wave_id = 0;
    std::int64_t selected_turn_job_id = 0;
    std::int64_t selected_execution_job_id = 0;
    std::int64_t entry_savestate_id = 0;
    std::string status;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct BindBattleCompletionExecutionJobCommand {
    std::int64_t battle_completion_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t exec_job_id = 0;
};

struct CompleteBattleCompletionCommand {
    std::int64_t battle_completion_id = 0;
    std::int64_t completion_savestate_id = 0;
    int manifest_version = 1;
    std::string manifest_blob;
    std::string manifest_sha256;
    std::int64_t manifest_artifact_id = 0;
    std::string route_kind;
    std::string transition_filename;
    std::string worker_terminal_sha256;
    std::string status;
    types::UtcTimePoint completed_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct FailBattleCompletionCommand {
    std::int64_t battle_completion_id = 0;
    std::string error_code;
    std::string error_text;
    std::optional<std::string> worker_terminal_sha256;
    types::UtcTimePoint completed_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct BattleCompletionRecord {
    std::int64_t battle_completion_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::optional<std::int64_t> exec_job_id;
    std::int64_t battle_set_id = 0;
    std::int64_t wave_id = 0;
    std::int64_t selected_turn_job_id = 0;
    std::int64_t selected_execution_job_id = 0;
    std::int64_t entry_savestate_id = 0;
    std::optional<std::int64_t> completion_savestate_id;
    std::optional<int> manifest_version;
    std::optional<std::string> manifest_blob;
    std::optional<std::string> manifest_sha256;
    std::optional<std::int64_t> manifest_artifact_id;
    std::optional<std::string> route_kind;
    std::optional<std::string> transition_filename;
    std::optional<std::string> worker_terminal_sha256;
    std::optional<std::string> error_code;
    std::optional<std::string> error_text;
    std::string status;
    types::UtcTimePoint created_at_utc{};
    std::optional<types::UtcTimePoint> completed_at_utc;
};

struct CreateBattleRecordingCommand {
    std::int64_t battle_completion_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::optional<std::int64_t> exec_job_id;
    std::int64_t source_savestate_id = 0;
    std::int64_t source_dtm_artifact_id = 0;
    std::int64_t source_itinerary_artifact_id = 0;
    int source_binding_version = 1;
    std::string source_binding_blob;
    std::string source_binding_sha256;
    int replay_plan_version = 1;
    std::string replay_plan_blob;
    std::string replay_plan_sha256;
    std::string status;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct BindBattleRecordingExecutionJobCommand {
    std::int64_t battle_recording_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t exec_job_id = 0;
};

struct CompleteBattleRecordingCommand {
    std::int64_t battle_recording_id = 0;
    std::string outcome;
    std::optional<std::int64_t> recorded_dtm_artifact_id;
    std::optional<std::int64_t> recorded_itinerary_artifact_id;
    std::optional<std::int64_t> paired_checkpoint_savestate_id;
    std::optional<int> timing_anchor_version;
    std::optional<std::string> timing_anchor_blob;
    std::optional<std::int64_t> tas_movie_tree_id;
    std::optional<std::int64_t> validation_request_id;
    std::optional<std::int64_t> sterilization_request_id;
    std::string worker_terminal_sha256;
    std::string status;
    types::UtcTimePoint completed_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct FailBattleRecordingCommand {
    std::int64_t battle_recording_id = 0;
    std::string error_code;
    std::string error_text;
    std::optional<std::string> worker_terminal_sha256;
    types::UtcTimePoint completed_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct BattleRecordingRecord {
    std::int64_t battle_recording_id = 0;
    std::int64_t battle_completion_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::optional<std::int64_t> exec_job_id;
    std::int64_t source_savestate_id = 0;
    std::int64_t source_dtm_artifact_id = 0;
    std::int64_t source_itinerary_artifact_id = 0;
    int source_binding_version = 0;
    std::string source_binding_blob;
    std::string source_binding_sha256;
    int replay_plan_version = 0;
    std::string replay_plan_blob;
    std::string replay_plan_sha256;
    std::optional<std::string> outcome;
    std::optional<std::int64_t> recorded_dtm_artifact_id;
    std::optional<std::int64_t> recorded_itinerary_artifact_id;
    std::optional<std::int64_t> paired_checkpoint_savestate_id;
    std::optional<int> timing_anchor_version;
    std::optional<std::string> timing_anchor_blob;
    std::optional<std::int64_t> tas_movie_tree_id;
    std::optional<std::int64_t> validation_request_id;
    std::optional<std::int64_t> sterilization_request_id;
    std::optional<std::string> worker_terminal_sha256;
    std::optional<std::string> error_code;
    std::optional<std::string> error_text;
    std::string status;
    types::UtcTimePoint created_at_utc{};
    std::optional<types::UtcTimePoint> completed_at_utc;
};

enum class TasRouteNodeKind {
    Checkpoint = 0,
    Activity = 1,
};

struct EnsureBattleRouteActivityCommand {
    std::int64_t battle_set_id = 0;
    std::int64_t entry_savestate_id = 0;
    std::int64_t battle_plan_id = 0;
    std::string activity_key;
    std::string default_label;
    std::string default_description;
    types::UtcTimePoint created_at_utc{};
};

struct EnsureBattleRouteActivityReceipt {
    std::int64_t root_route_node_id = 0;
    std::int64_t activity_route_node_id = 0;
};

struct EnsurePendingVictoryRouteBranchCommand {
    std::int64_t battle_set_id = 0;
    std::int64_t selected_turn_job_id = 0;
    std::string default_label;
    types::UtcTimePoint created_at_utc{};
};

struct EnsurePendingVictoryRouteBranchReceipt {
    std::int64_t victory_branch_id = 0;
    std::int64_t source_route_node_id = 0;
    std::int64_t checkpoint_route_node_id = 0;
};

struct BindVictoryRouteBranchWorkflowCommand {
    std::int64_t selected_turn_job_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::string status;
    types::UtcTimePoint updated_at_utc{};
};

struct TasRouteNodeSnapshot {
    std::int64_t route_node_id = 0;
    std::optional<std::int64_t> parent_route_node_id;
    TasRouteNodeKind node_kind = TasRouteNodeKind::Checkpoint;
    std::string activity_kind;
    std::string activity_key;
    std::string label;
    std::string description;
    std::optional<std::int64_t> source_dtm_artifact_id;
    std::optional<std::int64_t> source_savestate_id;
    std::optional<std::int64_t> battle_plan_id;
    std::optional<std::int64_t> tas_movie_tree_id;
    std::string status;
    types::UtcTimePoint created_at_utc{};
    types::UtcTimePoint updated_at_utc{};
};

struct VictoryRouteBranchSnapshot {
    std::int64_t victory_branch_id = 0;
    std::int64_t source_route_node_id = 0;
    std::int64_t checkpoint_route_node_id = 0;
    std::int64_t selected_turn_job_id = 0;
    std::optional<std::int64_t> workflow_instance_id;
    std::optional<std::int64_t> battle_recording_id;
    std::optional<std::int64_t> tas_movie_tree_id;
    std::optional<std::int64_t> checkpoint_savestate_id;
    std::optional<std::int64_t> validation_request_id;
    std::string status;
    types::UtcTimePoint created_at_utc{};
    types::UtcTimePoint updated_at_utc{};
};

struct BattleTurnAdvancementExpectedResult {
    std::int64_t wave_id = 0;
    std::int64_t turn_job_id = 0;
    std::int64_t exec_job_id = 0;
    std::string worker_terminal_sha256;
};

struct BattleTurnAdvancementDecisionPlan {
    std::int64_t turn_job_id = 0;
    BattleAdvancementDecisionKind decision_kind = BattleAdvancementDecisionKind::Unknown;
    std::optional<std::string> decision_reason;
};

struct BattleTurnAdvancementPoolPlan {
    std::string pool_name;
    BattleAdvancementCriterionKind criterion_kind = BattleAdvancementCriterionKind::Unknown;
    std::vector<BattleTurnAdvancementDecisionPlan> decisions;
};

struct BattleTurnAdvancementChildPlan {
    std::int64_t parent_wave_id = 0;
    std::int64_t parent_turn_job_id = 0;
    std::int64_t seed_candidate_id = 0;
    std::optional<std::string> pool_name;
};

struct BattleTurnAdvancementWaveStatusPlan {
    std::int64_t wave_id = 0;
    BattleTurnWaveStatus status = BattleTurnWaveStatus::Unknown;
    std::optional<types::UtcTimePoint> completed_at_utc;
};

struct ApplyBattleTurnAdvancementCommand {
    std::int64_t battle_set_id = 0;
    int turn_index = 0;
    std::vector<BattleTurnAdvancementExpectedResult> expected_results;
    std::vector<BattleTurnAdvancementPoolPlan> pools;
    std::vector<BattleTurnAdvancementChildPlan> children;
    std::vector<BattleTurnAdvancementWaveStatusPlan> wave_statuses;
    BattleSetStatus battle_set_status = BattleSetStatus::Unknown;
    std::optional<types::UtcTimePoint> battle_set_completed_at_utc;
    types::UtcTimePoint applied_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

enum class ApplyBattleTurnAdvancementDisposition {
    Applied = 0,
    AlreadyApplied = 1,
};

struct ApplyBattleTurnAdvancementReceipt {
    ApplyBattleTurnAdvancementDisposition disposition =
        ApplyBattleTurnAdvancementDisposition::Applied;
    std::vector<std::int64_t> child_wave_ids;
};

struct BindBattleRecordingValidationCommand {
    std::int64_t battle_recording_id = 0;
    std::int64_t tas_movie_tree_id = 0;
    std::int64_t validation_request_id = 0;
};

struct BindBattleRecordingSterilizationCommand {
    std::int64_t battle_recording_id = 0;
    std::int64_t tas_movie_tree_id = 0;
    std::int64_t sterilization_request_id = 0;
};

struct CreateBattleReplayCommand {
    std::int64_t battle_completion_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::optional<std::int64_t> exec_job_id;
    std::int64_t source_savestate_id = 0;
    std::optional<std::int64_t> source_dtm_artifact_id;
    std::optional<std::int64_t> source_itinerary_artifact_id;
    int source_binding_version = 1;
    std::string source_binding_blob;
    std::string source_binding_sha256;
    int replay_plan_version = 1;
    std::string replay_plan_blob;
    std::string replay_plan_sha256;
    std::string status;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct BindBattleReplayExecutionJobCommand {
    std::int64_t battle_replay_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t exec_job_id = 0;
};

struct CompleteBattleReplayCommand {
    std::int64_t battle_replay_id = 0;
    std::string outcome;
    std::uint32_t mismatch_turn = 0;
    std::uint32_t expected_rng = 0;
    std::uint32_t observed_rng = 0;
    std::optional<std::string> observed_completion_blob;
    std::optional<std::string> observed_completion_sha256;
    std::optional<std::string> observed_transition_blob;
    std::optional<std::string> observed_transition_sha256;
    std::string worker_terminal_sha256;
    std::string status;
    types::UtcTimePoint completed_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct FailBattleReplayCommand {
    std::int64_t battle_replay_id = 0;
    std::string error_code;
    std::string error_text;
    std::optional<std::string> worker_terminal_sha256;
    types::UtcTimePoint completed_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct BattleReplayRecord {
    std::int64_t battle_replay_id = 0;
    std::int64_t battle_completion_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::optional<std::int64_t> exec_job_id;
    std::int64_t source_savestate_id = 0;
    std::optional<std::int64_t> source_dtm_artifact_id;
    std::optional<std::int64_t> source_itinerary_artifact_id;
    int source_binding_version = 0;
    std::string source_binding_blob;
    std::string source_binding_sha256;
    int replay_plan_version = 0;
    std::string replay_plan_blob;
    std::string replay_plan_sha256;
    std::optional<std::string> outcome;
    std::uint32_t mismatch_turn = 0;
    std::uint32_t expected_rng = 0;
    std::uint32_t observed_rng = 0;
    std::optional<std::string> observed_completion_blob;
    std::optional<std::string> observed_completion_sha256;
    std::optional<std::string> observed_transition_blob;
    std::optional<std::string> observed_transition_sha256;
    std::optional<std::string> worker_terminal_sha256;
    std::optional<std::string> error_code;
    std::optional<std::string> error_text;
    std::string status;
    types::UtcTimePoint created_at_utc{};
    std::optional<types::UtcTimePoint> completed_at_utc;
};

// Coordination-owned, idempotent join of one completed SeedProbe run and one
// successful Battle Context capture.  It creates the BattleSet and exactly one
// first-turn wave for every accepted confirmed SeedProbe representative in a
// single AnalysisBattle transaction.
struct EnsureBattleStartCommand {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t probe_run_id = 0;
    std::int64_t context_probe_id = 0;
    std::string battle_set_name;
    std::int64_t entry_savestate_id = 0;
    std::int64_t battle_plan_id = 0;
    std::string battle_plan_fingerprint;
    BattleContinuationMode continuation_mode = BattleContinuationMode::Unknown;
    bool continue_automatic_exploration_after_victory = false;
    int launch_fake_attack_min = 0;
    int launch_fake_attack_max = 0;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct EnsureBattleStartReceipt {
    bool created = false;
    std::int64_t battle_start_id = 0;
    std::int64_t battle_set_id = 0;
    std::vector<std::int64_t> first_wave_ids;
};

struct RecordBattleSingleTurnResultCommand {
    std::int64_t turn_job_id = 0;
    std::int64_t exec_job_id = 0;
    std::string worker_terminal_sha256;
    std::string terminal_kind;
    std::optional<std::string> domain_outcome;
    std::optional<std::string> error_code;
    std::optional<std::string> error_text;
    std::optional<std::int64_t> ending_rng;
    std::optional<std::uint64_t> vi_start;
    std::optional<std::uint64_t> vi_end;
    std::optional<std::uint32_t> pred_passed;
    std::optional<std::uint32_t> pred_total;
    std::optional<std::uint32_t> cumulative_fake_attacks;
    std::optional<std::int64_t> successor_savestate_id;
    std::optional<std::int64_t> battle_context_artifact_id;
    std::optional<std::int64_t> predicate_group_revision_id;
    std::optional<std::string> predicate_group_sha256;
    std::optional<std::string> predicate_execution_package_sha256;
    std::vector<std::uint8_t> predicate_evidence_blob;
    std::optional<std::int64_t> applied_input_artifact_id;
    std::optional<std::int64_t> input_trace_artifact_id;
    types::UtcTimePoint recorded_at_utc{};
};

struct BattleSingleTurnResultSnapshot : RecordBattleSingleTurnResultCommand {
    std::int64_t battle_single_turn_result_id = 0;
};

struct ReplaceFailedBattleSingleTurnResultCommand {
    std::string expected_worker_terminal_sha256;
    std::string replacement_worker_terminal_sha256;
    RecordBattleSingleTurnResultCommand result;
    types::UtcTimePoint superseded_at_utc{};
};

struct BindBattlePredicateExecutionPackageCommand {
    std::optional<std::int64_t> wave_id;
    std::optional<std::int64_t> predicate_group_revision_id;
    std::string predicate_group_sha256;
    std::string execution_package_sha256;
    std::vector<std::uint8_t> execution_package_blob;
    std::int32_t phase_program_kind = 0;
    std::int32_t phase_program_version = 0;
    std::string phase_canonical_id;
    std::uint32_t phase_revision = 0;
    std::string phase_sha256;
    std::string hook_contract_canonical_id;
    std::uint32_t hook_contract_revision = 0;
    std::string hook_contract_sha256;
    types::UtcTimePoint created_at_utc{};
};

struct BattleAdvancementPoolRow {
    std::int64_t battle_advancement_pool_id = 0;
    std::int64_t battle_set_id = 0;
    int turn_index = 0;
    std::string pool_name;
    BattleAdvancementCriterionKind criterion_kind = BattleAdvancementCriterionKind::Unknown;
    types::UtcTimePoint created_at_utc{};
};

struct BattlePredicateExecutionPackageSnapshot {
    std::int64_t predicate_execution_package_id = 0;
    std::optional<std::int64_t> wave_id;
    std::optional<std::int64_t> predicate_group_revision_id;
    std::string predicate_group_sha256;
    std::string execution_package_sha256;
    std::vector<std::uint8_t> execution_package_blob;
    std::int32_t phase_program_kind = 0;
    std::int32_t phase_program_version = 0;
    std::string phase_canonical_id;
    std::uint32_t phase_revision = 0;
    std::string phase_sha256;
    std::string hook_contract_canonical_id;
    std::uint32_t hook_contract_revision = 0;
    std::string hook_contract_sha256;
    types::UtcTimePoint created_at_utc{};
};

enum class TasMovieValidationOperation {
    Unknown = 0,
    EstablishRootCursor,
    Validate,
};

enum class TasMovieValidationSourceKind {
    Unknown = 0,
    DtmArtifact,
    RootEstablishment,
    Tree,
};

enum class TasMovieValidationOutcome {
    Unknown = 0,
    RootCursorEstablished,
    Valid,
    Invalid,
};

enum class TasMovieValidationFailureReason {
    None = 0,
    MovieDesynchronized,
    ExpectedTerminalNotReached,
    Unknown,
};

enum class TasMovieValidationStatus {
    Untested = 0,
    Valid,
    Quarantined,
};

struct CreateTasMovieValidationRequestCommand {
    std::string materialization_key;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::string step_kind;
    TasMovieValidationOperation operation = TasMovieValidationOperation::Unknown;
    TasMovieValidationSourceKind source_kind = TasMovieValidationSourceKind::Unknown;
    std::int64_t source_ref_id = 0;
    std::int64_t source_dtm_artifact_id = 0;
    std::string source_dtm_sha256;
    std::optional<std::int64_t> rtc_value;
    std::string effective_dtm_sha256;
    std::optional<std::int64_t> itinerary_artifact_id;
    std::optional<std::string> itinerary_sha256;
    std::uint32_t required_final_breakpoint_pc = 0;
    bool capture_root_checkpoint = false;
    std::int64_t full_phase_program_kind = 0;
    std::int64_t full_phase_program_version = 0;
    std::string full_phase_canonical_id;
    std::int64_t full_phase_contract_revision = 0;
    std::string full_phase_sha256;
    std::string module_canonical_id;
    std::int64_t module_revision = 0;
    std::string module_sha256;
    types::UtcTimePoint created_at_utc{};
};

struct TasMovieValidationRequestRecord : CreateTasMovieValidationRequestCommand {
    std::int64_t validation_request_id = 0;
};

struct RecordTasMovieValidationAttemptCommand {
    std::int64_t validation_request_id = 0;
    std::int64_t source_job_id = 0;
    std::string worker_terminal_sha256;
    TasMovieValidationOutcome outcome = TasMovieValidationOutcome::Unknown;
    TasMovieValidationFailureReason failure_reason = TasMovieValidationFailureReason::None;
    std::optional<std::uint32_t> expected_pc;
    std::optional<std::uint64_t> expected_input_count;
    std::uint32_t actual_pc = 0;
    std::uint64_t actual_input_count = 0;
    std::optional<std::uint64_t> last_verified_itinerary_index;
    std::optional<std::int64_t> last_known_good_savestate_id;
    std::optional<std::int64_t> candidate_itinerary_artifact_id;
    std::optional<std::string> candidate_itinerary_sha256;
    std::optional<std::int64_t> produced_tas_movie_root_id;
    std::string worker_id;
    std::uint64_t worker_process_generation = 0;
    std::uint64_t workset_epoch = 0;
    types::UtcTimePoint recorded_at_utc{};
};

struct TasMovieValidationAttemptRecord : RecordTasMovieValidationAttemptCommand {
    std::int64_t validation_attempt_id = 0;
};

struct TasMovieValidationStatusRecord {
    std::string effective_dtm_sha256;
    TasMovieValidationStatus status = TasMovieValidationStatus::Untested;
    std::int64_t validation_attempt_id = 0;
    types::UtcTimePoint updated_at_utc{};
};

struct CreateTasMovieCheckpointSterilizationRequestCommand {
    std::string materialization_key;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t source_savestate_id = 0;
    std::int64_t source_savestate_artifact_id = 0;
    std::string source_savestate_sha256;
    std::int64_t source_dtm_artifact_id = 0;
    std::string source_dtm_sha256;
    std::optional<std::int64_t> reused_savestate_id;
    std::int64_t full_phase_program_kind = 0;
    std::int64_t full_phase_program_version = 0;
    std::string full_phase_canonical_id;
    std::int64_t full_phase_contract_revision = 0;
    std::string full_phase_sha256;
    std::string module_canonical_id;
    std::int64_t module_revision = 0;
    std::string module_sha256;
    types::UtcTimePoint created_at_utc{};
};

struct TasMovieCheckpointSterilizationRequestRecord
    : CreateTasMovieCheckpointSterilizationRequestCommand {
    std::int64_t sterilization_request_id = 0;
};

struct RecordTasMovieCheckpointSterilizationAttemptCommand {
    std::int64_t sterilization_request_id = 0;
    std::int64_t source_job_id = 0;
    std::string worker_terminal_sha256;
    std::string candidate_savestate_sha256;
    std::int64_t produced_savestate_id = 0;
    std::string worker_id;
    std::uint64_t worker_process_generation = 0;
    std::uint64_t workset_epoch = 0;
    types::UtcTimePoint recorded_at_utc{};
};

struct TasMovieCheckpointSterilizationAttemptRecord
    : RecordTasMovieCheckpointSterilizationAttemptCommand {
    std::int64_t sterilization_attempt_id = 0;
};

enum class TasMovieRootEstablishmentProducer {
    Unknown = 0,
    Establish,
    Revise,
};

struct RecordTasMovieRootEstablishmentAttemptCommand {
    TasMovieRootEstablishmentProducer producer =
        TasMovieRootEstablishmentProducer::Unknown;
    std::optional<std::int64_t> validation_attempt_id;
    std::optional<std::int64_t> rewrite_request_id;
    std::optional<std::int64_t> parent_root_establishment_attempt_id;
    std::int64_t source_dtm_artifact_id = 0;
    std::string source_dtm_sha256;
    std::int64_t itinerary_artifact_id = 0;
    std::string itinerary_sha256;
    std::uint32_t root_pc = 0;
    std::uint64_t movie_input_cursor = 0;
    std::int64_t source_job_id = 0;
    std::string worker_terminal_sha256;
    types::UtcTimePoint recorded_at_utc{};
};

struct TasMovieRootEstablishmentAttemptRecord
    : RecordTasMovieRootEstablishmentAttemptCommand {
    std::int64_t root_establishment_attempt_id = 0;
};

enum class TasMovieInputEpochAnnotationProducer {
    Unknown = 0,
    Annotate,
    Revise,
};

struct CreateTasMovieInputEpochAnnotationRequestCommand {
    std::string materialization_key;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t source_dtm_artifact_id = 0;
    std::string source_dtm_sha256;
    std::int64_t full_phase_program_kind = 0;
    std::int64_t full_phase_program_version = 0;
    std::string full_phase_canonical_id;
    std::int64_t full_phase_contract_revision = 0;
    std::string full_phase_sha256;
    std::string module_canonical_id;
    std::int64_t module_revision = 0;
    std::string module_sha256;
    types::UtcTimePoint created_at_utc{};
};

struct TasMovieInputEpochAnnotationRequestRecord
    : CreateTasMovieInputEpochAnnotationRequestCommand {
    std::int64_t annotation_request_id = 0;
};

struct RecordTasMovieInputEpochAnnotationAttemptCommand {
    TasMovieInputEpochAnnotationProducer producer =
        TasMovieInputEpochAnnotationProducer::Unknown;
    std::optional<std::int64_t> annotation_request_id;
    std::optional<std::int64_t> rewrite_request_id;
    std::int64_t source_dtm_artifact_id = 0;
    std::string source_dtm_sha256;
    std::int64_t source_job_id = 0;
    std::string worker_terminal_sha256;
    bool succeeded = false;
    std::optional<std::int64_t> schedule_artifact_id;
    std::optional<std::string> schedule_sha256;
    std::uint64_t source_poll_count = 0;
    std::uint64_t epoch_count = 0;
    std::uint64_t final_cursor = 0;
    std::optional<std::uint64_t> divergence_epoch;
    std::optional<std::uint64_t> divergence_cursor;
    std::string failure_code;
    std::string failure_text;
    std::string worker_id;
    std::uint64_t worker_process_generation = 0;
    std::uint64_t workset_epoch = 0;
    types::UtcTimePoint recorded_at_utc{};
};

struct TasMovieInputEpochAnnotationAttemptRecord
    : RecordTasMovieInputEpochAnnotationAttemptCommand {
    std::int64_t annotation_attempt_id = 0;
};

struct CreateTasMovieInputEpochRewriteRequestCommand {
    std::string materialization_key;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t annotation_attempt_id = 0;
    std::int64_t root_establishment_attempt_id = 0;
    std::int64_t source_dtm_artifact_id = 0;
    std::string source_dtm_sha256;
    std::int64_t schedule_artifact_id = 0;
    std::string schedule_sha256;
    std::uint64_t insert_before_epoch = 0;
    std::uint64_t neutral_epoch_count = 1;
    std::string placement_profile;
    std::int64_t full_phase_program_kind = 0;
    std::int64_t full_phase_program_version = 0;
    std::string full_phase_canonical_id;
    std::int64_t full_phase_contract_revision = 0;
    std::string full_phase_sha256;
    std::string module_canonical_id;
    std::int64_t module_revision = 0;
    std::string module_sha256;
    types::UtcTimePoint created_at_utc{};
};

struct TasMovieInputEpochRewriteRequestRecord
    : CreateTasMovieInputEpochRewriteRequestCommand {
    std::int64_t rewrite_request_id = 0;
};

struct RecordTasMovieInputEpochRewriteAttemptCommand {
    std::int64_t rewrite_request_id = 0;
    std::int64_t source_job_id = 0;
    std::string worker_terminal_sha256;
    bool succeeded = false;
    std::optional<std::int64_t> rewritten_dtm_artifact_id;
    std::optional<std::string> rewritten_dtm_sha256;
    std::optional<std::int64_t> endpoint_savestate_id;
    std::optional<std::int64_t> produced_annotation_attempt_id;
    std::optional<std::int64_t> produced_root_establishment_attempt_id;
    std::uint64_t source_epoch_count = 0;
    std::uint64_t rewritten_epoch_count = 0;
    std::uint64_t final_movie_input_count = 0;
    std::optional<std::uint64_t> divergence_epoch;
    std::optional<std::uint64_t> divergence_cursor;
    std::string failure_code;
    std::string failure_text;
    std::string worker_id;
    std::uint64_t worker_process_generation = 0;
    std::uint64_t workset_epoch = 0;
    types::UtcTimePoint recorded_at_utc{};
};

struct TasMovieInputEpochRewriteAttemptRecord
    : RecordTasMovieInputEpochRewriteAttemptCommand {
    std::int64_t rewrite_attempt_id = 0;
};

struct RecordTasMovieInputEpochRewriteCompletionCommand {
    RecordTasMovieInputEpochRewriteAttemptCommand rewrite;
    RecordTasMovieInputEpochAnnotationAttemptCommand annotation;
    RecordTasMovieRootEstablishmentAttemptCommand root_establishment;
};

struct RecordTasMovieInputEpochRewriteCompletionReceipt {
    std::int64_t rewrite_attempt_id = 0;
    std::int64_t annotation_attempt_id = 0;
    std::int64_t root_establishment_attempt_id = 0;
};

struct IAnalysisDb {
    virtual ~IAnalysisDb() = default;

    virtual bool CreateTasMovieValidationRequest(
        const CreateTasMovieValidationRequestCommand& command,
        std::int64_t* validation_request_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual std::optional<TasMovieValidationRequestRecord> GetTasMovieValidationRequest(
        std::int64_t validation_request_id) const = 0;
    virtual std::optional<TasMovieValidationRequestRecord> GetTasMovieValidationRequestForWorkflowStep(
        std::int64_t workflow_step_id) const = 0;
    virtual bool RecordTasMovieValidationAttempt(
        const RecordTasMovieValidationAttemptCommand& command,
        std::int64_t* validation_attempt_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual std::optional<TasMovieValidationAttemptRecord> GetTasMovieValidationAttempt(
        std::int64_t validation_attempt_id) const = 0;
    virtual std::optional<TasMovieValidationAttemptRecord> FindTasMovieValidationAttempt(
        std::int64_t source_job_id,
        std::string_view worker_terminal_sha256) const = 0;
    virtual std::optional<TasMovieValidationStatusRecord> GetTasMovieValidationStatus(
        std::string_view effective_dtm_sha256) const = 0;
    virtual bool CreateTasMovieCheckpointSterilizationRequest(
        const CreateTasMovieCheckpointSterilizationRequestCommand& command,
        std::int64_t* request_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual std::optional<TasMovieCheckpointSterilizationRequestRecord>
    GetTasMovieCheckpointSterilizationRequest(
        std::int64_t request_id) const = 0;
    virtual std::optional<TasMovieCheckpointSterilizationRequestRecord>
    GetTasMovieCheckpointSterilizationRequestForWorkflowStep(
        std::int64_t workflow_step_id) const = 0;
    virtual bool RecordTasMovieCheckpointSterilizationAttempt(
        const RecordTasMovieCheckpointSterilizationAttemptCommand& command,
        std::int64_t* attempt_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual std::optional<TasMovieCheckpointSterilizationAttemptRecord>
    GetTasMovieCheckpointSterilizationAttempt(
        std::int64_t attempt_id) const = 0;
    virtual std::optional<TasMovieCheckpointSterilizationAttemptRecord>
    FindTasMovieCheckpointSterilizationAttempt(
        std::int64_t source_job_id,
        std::string_view worker_terminal_sha256) const = 0;
    virtual std::vector<TasMovieCheckpointSterilizationAttemptRecord>
    ListTasMovieCheckpointSterilizationAttemptsForRequest(
        std::int64_t request_id) const = 0;
    virtual bool RecordTasMovieRootEstablishmentAttempt(
        const RecordTasMovieRootEstablishmentAttemptCommand& command,
        std::int64_t* attempt_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual std::optional<TasMovieRootEstablishmentAttemptRecord>
    GetTasMovieRootEstablishmentAttempt(std::int64_t attempt_id) const = 0;
    virtual std::vector<TasMovieRootEstablishmentAttemptRecord>
    ListTasMovieRootEstablishmentAttempts(int limit) const = 0;
    virtual bool CreateTasMovieInputEpochAnnotationRequest(
        const CreateTasMovieInputEpochAnnotationRequestCommand& command,
        std::int64_t* request_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual std::optional<TasMovieInputEpochAnnotationRequestRecord>
    GetTasMovieInputEpochAnnotationRequest(std::int64_t request_id) const = 0;
    virtual std::optional<TasMovieInputEpochAnnotationRequestRecord>
    GetTasMovieInputEpochAnnotationRequestForWorkflowStep(
        std::int64_t workflow_step_id) const = 0;
    virtual bool RecordTasMovieInputEpochAnnotationAttempt(
        const RecordTasMovieInputEpochAnnotationAttemptCommand& command,
        std::int64_t* attempt_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual std::optional<TasMovieInputEpochAnnotationAttemptRecord>
    GetTasMovieInputEpochAnnotationAttempt(std::int64_t attempt_id) const = 0;
    virtual std::optional<TasMovieInputEpochAnnotationAttemptRecord>
    FindTasMovieInputEpochAnnotationAttempt(
        std::int64_t source_job_id,
        std::string_view worker_terminal_sha256) const = 0;
    virtual bool CreateTasMovieInputEpochRewriteRequest(
        const CreateTasMovieInputEpochRewriteRequestCommand& command,
        std::int64_t* request_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual std::optional<TasMovieInputEpochRewriteRequestRecord>
    GetTasMovieInputEpochRewriteRequest(std::int64_t request_id) const = 0;
    virtual std::optional<TasMovieInputEpochRewriteRequestRecord>
    GetTasMovieInputEpochRewriteRequestForWorkflowStep(
        std::int64_t workflow_step_id) const = 0;
    virtual bool RecordTasMovieInputEpochRewriteAttempt(
        const RecordTasMovieInputEpochRewriteAttemptCommand& command,
        std::int64_t* attempt_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual std::optional<TasMovieInputEpochRewriteAttemptRecord>
    GetTasMovieInputEpochRewriteAttempt(std::int64_t attempt_id) const = 0;
    virtual std::optional<TasMovieInputEpochRewriteAttemptRecord>
    FindTasMovieInputEpochRewriteAttempt(
        std::int64_t source_job_id,
        std::string_view worker_terminal_sha256) const = 0;
    virtual bool RecordTasMovieInputEpochRewriteCompletion(
        const RecordTasMovieInputEpochRewriteCompletionCommand& command,
        RecordTasMovieInputEpochRewriteCompletionReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<std::int64_t> LookupSeedProbeRunSavestateId(std::int64_t probe_run_id) const = 0;
    virtual std::optional<SeedProbeResultRow> GetSeedProbeResult(std::int64_t probe_result_id) const = 0;
    virtual std::optional<SeedProbeResultRow> GetSeedProbeResultForSourceJob(
        std::int64_t source_job_id) const = 0;
    virtual std::vector<SeedProbeResultRow> ListSeedProbeResults(std::int64_t probe_run_id) const = 0;
    virtual std::optional<SeedProbeResultRow> FindConfirmedSeedProbeResultForAcceptedInputSetFrame(
        std::int64_t accepted_input_set_id,
        std::int64_t input_frame_id) const = 0;
    virtual std::optional<AnalysisInputSetFrameRow> GetAnalysisInputFrame(std::int64_t input_frame_id) const = 0;
    virtual std::vector<AnalysisInputSetFrameRow> ListAnalysisInputSetFrames(std::int64_t input_set_id) const = 0;
    virtual bool EnsureSeedProbeInputFrame(
        std::int64_t main_axis_xy_id,
        std::int64_t cstick_axis_xy_id,
        std::int64_t trigger_axis_xy_id,
        std::int64_t* input_frame_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual bool RecordSeedProbeObservation(
        const RecordSeedProbeObservationCommand& command,
        RecordSeedProbeObservationReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual bool TransitionSeedProbeEvidence(
        const TransitionSeedProbeEvidenceCommand& command,
        bool* changed_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool CreateSeedProbeSet(
        const CreateSeedProbeSetCommand& command,
        std::int64_t* probe_set_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool RequestSeedProbeRun(
        const RequestSeedProbeRunCommand& command,
        std::int64_t* probe_run_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<SeedProbeRunSnapshot> GetSeedProbeRun(
        std::int64_t probe_run_id) const = 0;

    virtual bool UpdateSeedProbeRunStatus(
        const UpdateSeedProbeRunStatusCommand& command,
        bool* changed_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual bool ReplaceSeedProbeAcceptedInputFrames(
        const ReplaceSeedProbeAcceptedInputFramesCommand& command,
        std::string* error_out = nullptr) = 0;

    virtual bool SetSeedProbeRunEntrySavestate(
        const SetSeedProbeRunEntrySavestateCommand& command,
        std::string* error_out = nullptr) = 0;

    virtual bool RecordSeedProbeEncounterProjection(
        const RecordSeedProbeEncounterProjectionCommand& command,
        std::int64_t* encounter_projection_id_out = nullptr,
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
    virtual bool EnsureBattleStart(
        const EnsureBattleStartCommand& command,
        EnsureBattleStartReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual bool BindBattlePredicateExecutionPackage(
        const BindBattlePredicateExecutionPackageCommand& command,
        std::int64_t* binding_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual std::optional<BattlePredicateExecutionPackageSnapshot>
    GetBattlePredicateExecutionPackageForWave(std::int64_t wave_id) const = 0;

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
    virtual bool RecordBattleSingleTurnResult(
        const RecordBattleSingleTurnResultCommand& command,
        std::int64_t* result_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual bool ReplaceFailedBattleSingleTurnResult(
        const ReplaceFailedBattleSingleTurnResultCommand& command,
        std::int64_t* result_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual std::optional<BattleSingleTurnResultSnapshot>
    GetBattleSingleTurnResultForExecJob(std::int64_t exec_job_id) const = 0;

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

    virtual bool ApplyBattleTurnAdvancement(
        const ApplyBattleTurnAdvancementCommand& command,
        ApplyBattleTurnAdvancementReceipt* receipt_out = nullptr,
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
    virtual bool EnsureBattleRouteActivity(
        const EnsureBattleRouteActivityCommand&,
        EnsureBattleRouteActivityReceipt* = nullptr,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "TAS route persistence is unavailable";
        return false;
    }
    virtual bool EnsurePendingVictoryRouteBranch(
        const EnsurePendingVictoryRouteBranchCommand&,
        EnsurePendingVictoryRouteBranchReceipt* = nullptr,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "TAS route persistence is unavailable";
        return false;
    }
    virtual bool BindVictoryRouteBranchWorkflow(
        const BindVictoryRouteBranchWorkflowCommand&,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "TAS route persistence is unavailable";
        return false;
    }
    virtual bool RenameTasRouteNode(
        std::int64_t, std::string_view, types::UtcTimePoint,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "TAS route persistence is unavailable";
        return false;
    }
    virtual std::vector<TasRouteNodeSnapshot> ListTasRouteNodes() const { return {}; }
    virtual std::vector<VictoryRouteBranchSnapshot> ListVictoryRouteBranches() const { return {}; }
    virtual std::vector<std::int64_t> ListBattleSetIdsForRouteNode(std::int64_t) const { return {}; }
    virtual std::vector<BattleSeedCandidateRow> ListBattleSeedCandidates(std::int64_t battle_set_id) const = 0;
    virtual std::optional<BattleSeedCandidateRow> GetBattleSeedCandidate(std::int64_t seed_candidate_id) const = 0;
    virtual std::optional<BattleTurnWaveSnapshot> GetBattleTurnWave(std::int64_t wave_id) const = 0;
    virtual std::vector<BattleTurnWaveSnapshot> ListBattleTurnWaves(std::int64_t battle_set_id) const = 0;
    virtual std::vector<BattleTurnWaveSnapshot> ListBattleTurnWavesForContextProbe(std::int64_t context_probe_id) const = 0;
    virtual std::optional<BattleContextProbeSnapshot> GetBattleContextProbe(std::int64_t context_probe_id) const = 0;
    virtual std::optional<BattleContextProbeSnapshot> GetBattleContextProbeForExecJob(std::int64_t exec_job_id) const = 0;
    virtual std::optional<BattleContextProbeSnapshot> GetLatestBattleContextForWave(std::int64_t wave_id) const = 0;
    virtual std::optional<BattleTurnJobSnapshot> GetBattleTurnJob(std::int64_t turn_job_id) const = 0;
    virtual std::optional<BattleTurnJobSnapshot> GetBattleTurnJobForExecJob(std::int64_t exec_job_id) const = 0;
    virtual std::vector<BattleTurnJobSnapshot> ListBattleTurnJobsByOutputSavestateId(
        std::int64_t output_savestate_id) const = 0;
    virtual std::vector<BattleTurnJobSnapshot> ListBattleTurnJobsForWave(std::int64_t wave_id) const = 0;
    virtual std::vector<BattleTurnJobSnapshot> ListBattleTurnJobsForBattleTurn(std::int64_t battle_set_id, int turn_index) const = 0;
    virtual std::vector<BattleAdvancementDecisionRow> ListBattleAdvancementDecisionsForPool(std::int64_t battle_advancement_pool_id) const = 0;
    virtual std::vector<BattleAdvancementPoolRow> ListBattleAdvancementPoolsForBattleTurn(
        std::int64_t battle_set_id, int turn_index) const = 0;
    virtual std::optional<std::int64_t> GetBattleWorkflowInstanceId(
        std::int64_t battle_set_id) const = 0;

    virtual bool CreateBattleCompletion(
        const CreateBattleCompletionCommand& command,
        std::int64_t* battle_completion_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual bool BindBattleCompletionExecutionJob(
        const BindBattleCompletionExecutionJobCommand& command,
        std::string* error_out = nullptr) = 0;
    virtual bool CompleteBattleCompletion(
        const CompleteBattleCompletionCommand& command,
        std::string* error_out = nullptr) = 0;
    virtual bool FailBattleCompletion(
        const FailBattleCompletionCommand& command,
        std::string* error_out = nullptr) = 0;
    virtual std::optional<BattleCompletionRecord> GetBattleCompletion(
        std::int64_t battle_completion_id) const = 0;
    virtual std::optional<BattleCompletionRecord> GetBattleCompletionForExecJob(
        std::int64_t exec_job_id) const = 0;
    virtual std::optional<BattleCompletionRecord> GetBattleCompletionForSelectedTurnJob(
        std::int64_t turn_job_id) const = 0;

    virtual bool CreateBattleRecording(
        const CreateBattleRecordingCommand& command,
        std::int64_t* battle_recording_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual bool BindBattleRecordingExecutionJob(
        const BindBattleRecordingExecutionJobCommand& command,
        std::string* error_out = nullptr) = 0;
    virtual bool BindBattleRecordingValidation(
        const BindBattleRecordingValidationCommand& command,
        std::string* error_out = nullptr) = 0;
    virtual bool BindBattleRecordingSterilization(
        const BindBattleRecordingSterilizationCommand& command,
        std::string* error_out = nullptr) = 0;
    virtual bool CompleteBattleRecording(
        const CompleteBattleRecordingCommand& command,
        std::string* error_out = nullptr) = 0;
    virtual bool FailBattleRecording(
        const FailBattleRecordingCommand& command,
        std::string* error_out = nullptr) = 0;
    virtual std::optional<BattleRecordingRecord> GetBattleRecording(
        std::int64_t battle_recording_id) const = 0;
    virtual std::optional<BattleRecordingRecord> GetBattleRecordingForExecJob(
        std::int64_t exec_job_id) const = 0;
    virtual std::optional<BattleRecordingRecord> GetBattleRecordingForTasMovieTree(
        std::int64_t tas_movie_tree_id) const = 0;
    virtual std::optional<BattleRecordingRecord> GetBattleRecordingForPairedCheckpoint(
        std::int64_t paired_checkpoint_savestate_id) const = 0;
    virtual bool CreateBattleReplay(
        const CreateBattleReplayCommand& command,
        std::int64_t* battle_replay_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual bool BindBattleReplayExecutionJob(
        const BindBattleReplayExecutionJobCommand& command,
        std::string* error_out = nullptr) = 0;
    virtual bool CompleteBattleReplay(
        const CompleteBattleReplayCommand& command,
        std::string* error_out = nullptr) = 0;
    virtual bool FailBattleReplay(
        const FailBattleReplayCommand& command,
        std::string* error_out = nullptr) = 0;
    virtual std::optional<BattleReplayRecord> GetBattleReplay(
        std::int64_t battle_replay_id) const = 0;
    virtual std::optional<BattleReplayRecord> GetBattleReplayForExecJob(
        std::int64_t exec_job_id) const = 0;

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
