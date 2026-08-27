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

    bool CreateTasMovieValidationRequest(
        const CreateTasMovieValidationRequestCommand& command,
        std::int64_t* validation_request_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<TasMovieValidationRequestRecord> GetTasMovieValidationRequest(
        std::int64_t validation_request_id) const override;
    std::optional<TasMovieValidationRequestRecord> GetTasMovieValidationRequestForWorkflowStep(
        std::int64_t workflow_step_id) const override;
    bool RecordTasMovieValidationAttempt(
        const RecordTasMovieValidationAttemptCommand& command,
        std::int64_t* validation_attempt_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<TasMovieValidationAttemptRecord> GetTasMovieValidationAttempt(
        std::int64_t validation_attempt_id) const override;
    std::optional<TasMovieValidationAttemptRecord> FindTasMovieValidationAttempt(
        std::int64_t source_job_id,
        std::string_view worker_terminal_sha256) const override;
    std::optional<TasMovieValidationStatusRecord> GetTasMovieValidationStatus(
        std::string_view effective_dtm_sha256) const override;
    bool CreateTasMovieCheckpointSterilizationRequest(
        const CreateTasMovieCheckpointSterilizationRequestCommand& command,
        std::int64_t* request_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<TasMovieCheckpointSterilizationRequestRecord>
    GetTasMovieCheckpointSterilizationRequest(
        std::int64_t request_id) const override;
    std::optional<TasMovieCheckpointSterilizationRequestRecord>
    GetTasMovieCheckpointSterilizationRequestForWorkflowStep(
        std::int64_t workflow_step_id) const override;
    bool RecordTasMovieCheckpointSterilizationAttempt(
        const RecordTasMovieCheckpointSterilizationAttemptCommand& command,
        std::int64_t* attempt_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<TasMovieCheckpointSterilizationAttemptRecord>
    GetTasMovieCheckpointSterilizationAttempt(
        std::int64_t attempt_id) const override;
    std::optional<TasMovieCheckpointSterilizationAttemptRecord>
    FindTasMovieCheckpointSterilizationAttempt(
        std::int64_t source_job_id,
        std::string_view worker_terminal_sha256) const override;
    std::vector<TasMovieCheckpointSterilizationAttemptRecord>
    ListTasMovieCheckpointSterilizationAttemptsForRequest(
        std::int64_t request_id) const override;
    bool RecordTasMovieRootEstablishmentAttempt(
        const RecordTasMovieRootEstablishmentAttemptCommand& command,
        std::int64_t* attempt_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<TasMovieRootEstablishmentAttemptRecord>
    GetTasMovieRootEstablishmentAttempt(std::int64_t attempt_id) const override;
    std::vector<TasMovieRootEstablishmentAttemptRecord>
    ListTasMovieRootEstablishmentAttempts(int limit) const override;
    bool CreateTasMovieInputEpochAnnotationRequest(
        const CreateTasMovieInputEpochAnnotationRequestCommand& command,
        std::int64_t* request_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<TasMovieInputEpochAnnotationRequestRecord>
    GetTasMovieInputEpochAnnotationRequest(std::int64_t request_id) const override;
    std::optional<TasMovieInputEpochAnnotationRequestRecord>
    GetTasMovieInputEpochAnnotationRequestForWorkflowStep(
        std::int64_t workflow_step_id) const override;
    bool RecordTasMovieInputEpochAnnotationAttempt(
        const RecordTasMovieInputEpochAnnotationAttemptCommand& command,
        std::int64_t* attempt_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<TasMovieInputEpochAnnotationAttemptRecord>
    GetTasMovieInputEpochAnnotationAttempt(std::int64_t attempt_id) const override;
    std::optional<TasMovieInputEpochAnnotationAttemptRecord>
    FindTasMovieInputEpochAnnotationAttempt(
        std::int64_t source_job_id,
        std::string_view worker_terminal_sha256) const override;
    bool CreateTasMovieInputEpochRewriteRequest(
        const CreateTasMovieInputEpochRewriteRequestCommand& command,
        std::int64_t* request_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<TasMovieInputEpochRewriteRequestRecord>
    GetTasMovieInputEpochRewriteRequest(std::int64_t request_id) const override;
    std::optional<TasMovieInputEpochRewriteRequestRecord>
    GetTasMovieInputEpochRewriteRequestForWorkflowStep(
        std::int64_t workflow_step_id) const override;
    bool RecordTasMovieInputEpochRewriteAttempt(
        const RecordTasMovieInputEpochRewriteAttemptCommand& command,
        std::int64_t* attempt_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<TasMovieInputEpochRewriteAttemptRecord>
    GetTasMovieInputEpochRewriteAttempt(std::int64_t attempt_id) const override;
    std::optional<TasMovieInputEpochRewriteAttemptRecord>
    FindTasMovieInputEpochRewriteAttempt(
        std::int64_t source_job_id,
        std::string_view worker_terminal_sha256) const override;
    bool RecordTasMovieInputEpochRewriteCompletion(
        const RecordTasMovieInputEpochRewriteCompletionCommand& command,
        RecordTasMovieInputEpochRewriteCompletionReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool CreateTasMovieCutsceneRequest(
        const CreateTasMovieCutsceneRequestCommand& command,
        std::int64_t* request_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<TasMovieCutsceneRequestRecord>
    GetTasMovieCutsceneRequest(std::int64_t request_id) const override;
    std::optional<TasMovieCutsceneRequestRecord>
    GetTasMovieCutsceneRequestForWorkflowStep(
        std::int64_t workflow_step_id) const override;
    bool RecordTasMovieCutsceneAttempt(
        const RecordTasMovieCutsceneAttemptCommand& command,
        std::int64_t* attempt_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<TasMovieCutsceneAttemptRecord>
    GetTasMovieCutsceneAttempt(std::int64_t attempt_id) const override;
    std::optional<TasMovieCutsceneAttemptRecord>
    FindTasMovieCutsceneAttempt(std::int64_t source_job_id,
        std::string_view worker_terminal_sha256) const override;

    std::optional<std::int64_t> LookupSeedProbeRunSavestateId(std::int64_t probe_run_id) const override;
    std::optional<SeedProbeResultRow> GetSeedProbeResult(std::int64_t probe_result_id) const override;
    std::optional<SeedProbeResultRow> GetSeedProbeResultForSourceJob(std::int64_t source_job_id) const override;
    std::vector<SeedProbeResultRow> ListSeedProbeResults(std::int64_t probe_run_id) const override;
    std::optional<SeedProbeResultRow> FindConfirmedSeedProbeResultForAcceptedInputSetFrame(
        std::int64_t accepted_input_set_id,
        std::int64_t input_frame_id) const override;
    std::optional<AnalysisInputSetFrameRow> GetAnalysisInputFrame(std::int64_t input_frame_id) const override;
    std::vector<AnalysisInputSetFrameRow> ListAnalysisInputSetFrames(std::int64_t input_set_id) const override;
    bool EnsureSeedProbeInputFrame(
        std::int64_t main_axis_xy_id,
        std::int64_t cstick_axis_xy_id,
        std::int64_t trigger_axis_xy_id,
        std::int64_t* input_frame_id_out = nullptr,
        std::string* error_out = nullptr) override;
    bool RecordSeedProbeObservation(
        const RecordSeedProbeObservationCommand& command,
        RecordSeedProbeObservationReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool TransitionSeedProbeEvidence(
        const TransitionSeedProbeEvidenceCommand& command,
        bool* changed_out = nullptr,
        std::string* error_out = nullptr) override;

    bool CreateSeedProbeSet(
        const CreateSeedProbeSetCommand& command,
        std::int64_t* probe_set_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool RequestSeedProbeRun(
        const RequestSeedProbeRunCommand& command,
        std::int64_t* probe_run_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<SeedProbeRunSnapshot> GetSeedProbeRun(
        std::int64_t probe_run_id) const override;
    bool UpdateSeedProbeRunStatus(
        const UpdateSeedProbeRunStatusCommand& command,
        bool* changed_out = nullptr,
        std::string* error_out = nullptr) override;
    bool ReplaceSeedProbeAcceptedInputFrames(
        const ReplaceSeedProbeAcceptedInputFramesCommand& command,
        std::string* error_out = nullptr) override;
    bool SetSeedProbeRunEntrySavestate(
        const SetSeedProbeRunEntrySavestateCommand& command,
        std::string* error_out = nullptr) override;

    bool RecordSeedProbeEncounterProjection(
        const RecordSeedProbeEncounterProjectionCommand& command,
        std::int64_t* encounter_projection_id_out = nullptr,
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
    bool EnsureBattleStart(
        const EnsureBattleStartCommand& command,
        EnsureBattleStartReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool BindBattlePredicateExecutionPackage(
        const BindBattlePredicateExecutionPackageCommand& command,
        std::int64_t* binding_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<BattlePredicateExecutionPackageSnapshot>
    GetBattlePredicateExecutionPackageForWave(std::int64_t wave_id) const override;
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
    bool RecordBattleSingleTurnResult(
        const RecordBattleSingleTurnResultCommand& command,
        std::int64_t* result_id_out = nullptr,
        std::string* error_out = nullptr) override;
    bool ReplaceFailedBattleSingleTurnResult(
        const ReplaceFailedBattleSingleTurnResultCommand& command,
        std::int64_t* result_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<BattleSingleTurnResultSnapshot>
    GetBattleSingleTurnResultForExecJob(std::int64_t exec_job_id) const override;

    bool CreateBattleAdvancementPool(
        const CreateBattleAdvancementPoolCommand& command,
        std::int64_t* battle_advancement_pool_id_out = nullptr,
        std::string* error_out = nullptr) override;
    bool EnsureBattleAdvancementPool(
        const CreateBattleAdvancementPoolCommand& command,
        std::int64_t* battle_advancement_pool_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool RecordBattleAdvancementDecision(
        const RecordBattleAdvancementDecisionCommand& command,
        std::int64_t* battle_advancement_decision_id_out = nullptr,
        std::string* error_out = nullptr) override;
    bool ApplyBattleTurnAdvancement(
        const ApplyBattleTurnAdvancementCommand& command,
        ApplyBattleTurnAdvancementReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;

    bool UpsertBattleManualFollowup(
        const UpsertBattleManualFollowupCommand& command,
        std::int64_t* manual_followup_id_out = nullptr,
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
    bool EnsureBattleRouteActivity(
        const EnsureBattleRouteActivityCommand& command,
        EnsureBattleRouteActivityReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool EnsurePendingVictoryRouteBranch(
        const EnsurePendingVictoryRouteBranchCommand& command,
        EnsurePendingVictoryRouteBranchReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool BindVictoryRouteBranchWorkflow(
        const BindVictoryRouteBranchWorkflowCommand& command,
        std::string* error_out = nullptr) override;
    bool RenameTasRouteNode(
        std::int64_t route_node_id,
        std::string_view label,
        types::UtcTimePoint updated_at_utc,
        std::string* error_out = nullptr) override;
    std::vector<TasRouteNodeSnapshot> ListTasRouteNodes() const override;
    std::vector<VictoryRouteBranchSnapshot> ListVictoryRouteBranches() const override;
    std::vector<std::int64_t> ListBattleSetIdsForRouteNode(
        std::int64_t route_node_id) const override;
    std::vector<BattleSeedCandidateRow> ListBattleSeedCandidates(std::int64_t battle_set_id) const override;
    std::optional<BattleSeedCandidateRow> GetBattleSeedCandidate(std::int64_t seed_candidate_id) const override;
    std::optional<BattleTurnWaveSnapshot> GetBattleTurnWave(std::int64_t wave_id) const override;
    std::vector<BattleTurnWaveSnapshot> ListBattleTurnWaves(std::int64_t battle_set_id) const override;
    std::vector<BattleTurnWaveSnapshot> ListBattleTurnWavesForContextProbe(std::int64_t context_probe_id) const override;
    std::optional<BattleContextProbeSnapshot> GetBattleContextProbe(std::int64_t context_probe_id) const override;
    std::optional<BattleContextProbeSnapshot> GetBattleContextProbeForExecJob(std::int64_t exec_job_id) const override;
    std::optional<BattleContextProbeSnapshot> GetLatestBattleContextForWave(std::int64_t wave_id) const override;
    std::optional<BattleTurnJobSnapshot> GetBattleTurnJob(std::int64_t turn_job_id) const override;
    std::optional<BattleTurnJobSnapshot> GetBattleTurnJobForExecJob(std::int64_t exec_job_id) const override;
    std::vector<BattleTurnJobSnapshot> ListBattleTurnJobsByOutputSavestateId(
        std::int64_t output_savestate_id) const override;
    std::vector<BattleTurnJobSnapshot> ListBattleTurnJobsForWave(std::int64_t wave_id) const override;
    std::vector<BattleTurnJobSnapshot> ListBattleTurnJobsForBattleTurn(std::int64_t battle_set_id, int turn_index) const override;
    std::vector<BattleAdvancementDecisionRow> ListBattleAdvancementDecisionsForPool(std::int64_t battle_advancement_pool_id) const override;
    std::vector<BattleAdvancementPoolRow> ListBattleAdvancementPoolsForBattleTurn(
        std::int64_t battle_set_id, int turn_index) const override;
    std::optional<std::int64_t> GetBattleWorkflowInstanceId(
        std::int64_t battle_set_id) const override;
    bool CreateBattleCompletion(
        const CreateBattleCompletionCommand& command,
        std::int64_t* battle_completion_id_out = nullptr,
        std::string* error_out = nullptr) override;
    bool BindBattleCompletionExecutionJob(
        const BindBattleCompletionExecutionJobCommand& command,
        std::string* error_out = nullptr) override;
    bool CompleteBattleCompletion(
        const CompleteBattleCompletionCommand& command,
        std::string* error_out = nullptr) override;
    bool FailBattleCompletion(
        const FailBattleCompletionCommand& command,
        std::string* error_out = nullptr) override;
    std::optional<BattleCompletionRecord> GetBattleCompletion(std::int64_t battle_completion_id) const override;
    std::optional<BattleCompletionRecord> GetBattleCompletionForExecJob(std::int64_t exec_job_id) const override;
    std::optional<BattleCompletionRecord> GetBattleCompletionForSelectedTurnJob(
        std::int64_t turn_job_id) const override;
    bool CreateBattleRecording(
        const CreateBattleRecordingCommand& command,
        std::int64_t* battle_recording_id_out = nullptr,
        std::string* error_out = nullptr) override;
    bool BindBattleRecordingExecutionJob(
        const BindBattleRecordingExecutionJobCommand& command,
        std::string* error_out = nullptr) override;
    bool BindBattleRecordingValidation(
        const BindBattleRecordingValidationCommand& command,
        std::string* error_out = nullptr) override;
    bool BindBattleRecordingSterilization(
        const BindBattleRecordingSterilizationCommand& command,
        std::string* error_out = nullptr) override;
    bool CompleteBattleRecording(
        const CompleteBattleRecordingCommand& command,
        std::string* error_out = nullptr) override;
    bool FailBattleRecording(
        const FailBattleRecordingCommand& command,
        std::string* error_out = nullptr) override;
    std::optional<BattleRecordingRecord> GetBattleRecording(std::int64_t battle_recording_id) const override;
    std::optional<BattleRecordingRecord> GetBattleRecordingForExecJob(std::int64_t exec_job_id) const override;
    std::optional<BattleRecordingRecord> GetBattleRecordingForTasMovieTree(std::int64_t tas_movie_tree_id) const override;
    std::optional<BattleRecordingRecord> GetBattleRecordingForPairedCheckpoint(std::int64_t paired_checkpoint_savestate_id) const override;
    bool CreateBattleReplay(
        const CreateBattleReplayCommand& command,
        std::int64_t* battle_replay_id_out = nullptr,
        std::string* error_out = nullptr) override;
    bool BindBattleReplayExecutionJob(
        const BindBattleReplayExecutionJobCommand& command,
        std::string* error_out = nullptr) override;
    bool CompleteBattleReplay(
        const CompleteBattleReplayCommand& command,
        std::string* error_out = nullptr) override;
    bool FailBattleReplay(
        const FailBattleReplayCommand& command,
        std::string* error_out = nullptr) override;
    std::optional<BattleReplayRecord> GetBattleReplay(
        std::int64_t battle_replay_id) const override;
    std::optional<BattleReplayRecord> GetBattleReplayForExecJob(
        std::int64_t exec_job_id) const override;

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
