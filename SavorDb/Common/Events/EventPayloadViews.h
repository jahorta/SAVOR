#pragma once

#include <cstdint>

namespace savor::db::events {

// Typed v1 payload view for Execution workflow/job events.
struct ExecutionWorkflowJobPayloadView {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t workflow_edge_id = 0;
    std::int64_t job_set_id = 0;
    std::int64_t workset_id = 0;
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

struct AnalysisSeedProbeResultPayloadView {
    std::int64_t probe_run_id = 0;
    std::int64_t probe_result_id = 0;
};

struct AnalysisSeedProbeEncounterProjectionRecordedPayloadView {
    std::int64_t probe_run_id = 0;
    std::int64_t encounter_projection_id = 0;
};

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

using AnalysisBattleTurnJobResultUpdatedPayloadView = AnalysisBattleTurnJobRecordedPayloadView;
using AnalysisBattleTurnWaveStatusUpdatedPayloadView = AnalysisBattleTurnWaveCreatedPayloadView;
using AnalysisBattleBattleSetStatusUpdatedPayloadView = AnalysisBattleSetCreatedPayloadView;

struct AnalysisBattleBattleAdvancementPoolCreatedPayloadView {
    std::int64_t battle_set_id = 0;
    std::int64_t battle_advancement_pool_id = 0;
};

struct AnalysisBattleBattleAdvancementDecisionRecordedPayloadView {
    std::int64_t battle_set_id = 0;
    std::int64_t battle_advancement_pool_id = 0;
    std::int64_t turn_job_id = 0;
    std::int64_t battle_advancement_decision_id = 0;
};

struct AnalysisBattleManualFollowupUpdatedPayloadView {
    std::int64_t battle_set_id = 0;
    std::int64_t turn_job_id = 0;
    std::int64_t manual_followup_id = 0;
};

struct AnalysisBattleCompletionPayloadView {
    std::int64_t battle_completion_id = 0;
};

struct AnalysisBattleResultsPayloadView {
    std::int64_t battle_completion_id = 0;
    std::int64_t battle_results_id = 0;
};

// Backward-compatible coarse family view.
struct AnalysisBattlePayloadView {
    std::int64_t battle_set_id = 0;
    std::int64_t wave_id = 0;
    std::int64_t turn_job_id = 0;
    std::int64_t battle_completion_id = 0;
    std::int64_t battle_results_id = 0;
};

// AnalysisSpine event-specific typed payload views.
struct AnalysisSpineRunCreatedPayloadView {
    std::int64_t run_id = 0;
};

struct AnalysisSpineStateRefRegisteredPayloadView {
    std::int64_t run_id = 0;
    std::int64_t state_ref_id = 0;
};

struct AnalysisSpineLineageEdgeAddedPayloadView {
    std::int64_t parent_run_id = 0;
    std::int64_t child_run_id = 0;
    std::int64_t lineage_edge_id = 0;
};

struct AnalysisSpineArtifactLinkedPayloadView {
    std::int64_t run_id = 0;
    std::int64_t artifact_ref_id = 0;
    std::int64_t artifact_id = 0;
};

// Backward-compatible coarse family view.
struct AnalysisSpinePayloadView {
    std::int64_t run_id = 0;
    std::int64_t state_ref_id = 0;
    std::int64_t lineage_edge_id = 0;
    std::int64_t artifact_ref_id = 0;
};

// Typed v1 payload view for State artifact events.
struct StateArtifactPayloadView {
    std::int64_t artifact_id = 0;
    std::int64_t savestate_id = 0;
    std::int64_t tas_movie_root_id = 0;
    std::int64_t tas_movie_tree_id = 0;
};

// Typed v1 payload view for Archive package events.
struct ArchivePackagePayloadView {
    std::int64_t archive_package_id = 0;
    std::int64_t archive_item_id = 0;
    std::int64_t rehydrate_request_id = 0;
};

} // namespace savor::db::events
