#pragma once

#include <cstdint>

namespace simcore::db::events {

// Typed v1 payload view for Execution workflow/job events.
struct ExecutionWorkflowJobPayloadView {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t workflow_edge_id = 0;
    std::int64_t job_set_id = 0;
    std::int64_t job_id = 0;
};

// AnalysisSeedProbe event-specific typed payload views.
struct AnalysisSeedProbeSetCreatedPayloadView {
    std::int64_t probe_set_id = 0;
};

struct AnalysisSeedProbeRunRequestedPayloadView {
    std::int64_t probe_set_id = 0;
    std::int64_t probe_run_id = 0;
};

struct AnalysisSeedProbeNeutralSeedRecordedPayloadView {
    std::int64_t probe_run_id = 0;
    std::int64_t probe_result_id = 0;
    std::int64_t neutral_seed_id = 0;
};

struct AnalysisSeedProbeGridSeedRecordedPayloadView {
    std::int64_t probe_run_id = 0;
    std::int64_t probe_result_id = 0;
    std::int64_t grid_seed_id = 0;
};

struct AnalysisSeedProbeUniqueSeedRecordedPayloadView {
    std::int64_t probe_run_id = 0;
    std::int64_t probe_result_id = 0;
    std::int64_t unique_seed_id = 0;
};

struct AnalysisSeedProbeEncounterProjectionRecordedPayloadView {
    std::int64_t probe_run_id = 0;
    std::int64_t encounter_projection_id = 0;
};

struct AnalysisSeedProbeRunCompletedPayloadView {
    std::int64_t probe_run_id = 0;
    std::int64_t probe_result_id = 0;
};

// Backward-compatible coarse family view.
struct AnalysisSeedProbePayloadView {
    std::int64_t probe_set_id = 0;
    std::int64_t probe_run_id = 0;
    std::int64_t probe_result_id = 0;
};

// AnalysisBattle event-specific typed payload views.
struct AnalysisBattleSetCreatedPayloadView {
    std::int64_t battle_set_id = 0;
};

struct AnalysisBattleSeedCandidateAddedPayloadView {
    std::int64_t battle_set_id = 0;
    std::int64_t seed_candidate_id = 0;
};

struct AnalysisBattleTurnWaveCreatedPayloadView {
    std::int64_t battle_set_id = 0;
    std::int64_t wave_id = 0;
};

struct AnalysisBattleTurnJobRecordedPayloadView {
    std::int64_t battle_set_id = 0;
    std::int64_t wave_id = 0;
    std::int64_t turn_job_id = 0;
    std::int64_t exec_job_id = 0;
};

struct AnalysisBattleSelectionPoolCreatedPayloadView {
    std::int64_t battle_set_id = 0;
    std::int64_t selection_pool_id = 0;
};

struct AnalysisBattleSelectionDecisionRecordedPayloadView {
    std::int64_t battle_set_id = 0;
    std::int64_t selection_pool_id = 0;
    std::int64_t turn_job_id = 0;
    std::int64_t selection_decision_id = 0;
};

struct AnalysisBattleTerminalFollowupUpdatedPayloadView {
    std::int64_t battle_set_id = 0;
    std::int64_t turn_job_id = 0;
    std::int64_t terminal_followup_id = 0;
};

// Backward-compatible coarse family view.
struct AnalysisBattlePayloadView {
    std::int64_t battle_set_id = 0;
    std::int64_t wave_id = 0;
    std::int64_t turn_job_id = 0;
};

// Typed v1 payload view for State artifact events.
struct StateArtifactPayloadView {
    std::int64_t artifact_id = 0;
    std::int64_t savestate_id = 0;
    std::int64_t tas_variant_id = 0;
};

// Typed v1 payload view for Archive package events.
struct ArchivePackagePayloadView {
    std::int64_t archive_package_id = 0;
    std::int64_t archive_item_id = 0;
    std::int64_t rehydrate_request_id = 0;
};

} // namespace simcore::db::events
