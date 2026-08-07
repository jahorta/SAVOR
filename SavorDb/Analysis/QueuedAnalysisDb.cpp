#include "QueuedAnalysisDb.h"

#include <utility>

namespace savor::db::analysis {
namespace {

void SetError(std::string* error_out, std::string message) {
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
}

savor::db::core::QueuedDbTelemetrySnapshot BuildTelemetrySnapshot(
    const savor::db::core::QueuedDbLane* write_lane,
    const savor::db::core::QueuedDbLane* read_lane) {
    return savor::db::core::BuildQueuedDbTelemetrySnapshot(write_lane, read_lane);
}

} // namespace

QueuedAnalysisDb::QueuedAnalysisDb(
    savor::db::IAnalysisDb* inner,
    savor::db::core::QueuedDbConfig config)
    : inner_(inner)
    , config_(config)
    , read_lane_(std::make_unique<savor::db::core::QueuedDbLane>("analysis-read", config_.read_capacity))
    , write_lane_(std::make_unique<savor::db::core::QueuedDbLane>("analysis-write", config_.write_capacity)) {
}

QueuedAnalysisDb::~QueuedAnalysisDb() {
    Stop();
}

bool QueuedAnalysisDb::Start(std::string* error_out) {
    if (inner_ == nullptr) {
        SetError(error_out, "analysis db inner database is null");
        return false;
    }
    if (IsRunning()) {
        return true;
    }
    if (!write_lane_->Start(error_out)) {
        return false;
    }
    if (!read_lane_->Start(error_out)) {
        write_lane_->Stop();
        return false;
    }
    return true;
}

void QueuedAnalysisDb::Stop() {
    if (read_lane_) {
        read_lane_->Stop();
    }
    if (write_lane_) {
        write_lane_->Stop();
    }
}

bool QueuedAnalysisDb::IsRunning() const {
    return read_lane_ != nullptr
        && write_lane_ != nullptr
        && read_lane_->IsRunning()
        && write_lane_->IsRunning();
}

savor::db::core::QueuedDbTelemetrySnapshot QueuedAnalysisDb::GetTelemetrySnapshot() const {
    return BuildTelemetrySnapshot(write_lane_.get(), read_lane_.get());
}

bool QueuedAnalysisDb::CreateTasMovieValidationRequest(
    const CreateTasMovieValidationRequestCommand& command,
    std::int64_t* validation_request_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>([this, command, validation_request_id_out, error_out]() {
        return inner_ != nullptr && inner_->CreateTasMovieValidationRequest(command, validation_request_id_out, error_out);
    }, false, error_out);
}

std::optional<TasMovieValidationRequestRecord> QueuedAnalysisDb::GetTasMovieValidationRequest(
    std::int64_t validation_request_id) const {
    return ExecuteRead<std::optional<TasMovieValidationRequestRecord>>(
        [this, validation_request_id]() {
            return inner_ != nullptr ? inner_->GetTasMovieValidationRequest(validation_request_id) : std::nullopt;
        }, std::nullopt);
}

std::optional<TasMovieValidationRequestRecord> QueuedAnalysisDb::GetTasMovieValidationRequestForWorkflowStep(
    std::int64_t workflow_step_id) const {
    return ExecuteRead<std::optional<TasMovieValidationRequestRecord>>(
        [this, workflow_step_id]() {
            return inner_ != nullptr ? inner_->GetTasMovieValidationRequestForWorkflowStep(workflow_step_id) : std::nullopt;
        }, std::nullopt);
}

bool QueuedAnalysisDb::RecordTasMovieValidationAttempt(
    const RecordTasMovieValidationAttemptCommand& command,
    std::int64_t* validation_attempt_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>([this, command, validation_attempt_id_out, error_out]() {
        return inner_ != nullptr && inner_->RecordTasMovieValidationAttempt(command, validation_attempt_id_out, error_out);
    }, false, error_out);
}

std::optional<TasMovieValidationAttemptRecord> QueuedAnalysisDb::GetTasMovieValidationAttempt(
    std::int64_t validation_attempt_id) const {
    return ExecuteRead<std::optional<TasMovieValidationAttemptRecord>>(
        [this, validation_attempt_id]() {
            return inner_ != nullptr ? inner_->GetTasMovieValidationAttempt(validation_attempt_id) : std::nullopt;
        }, std::nullopt);
}

std::optional<TasMovieValidationAttemptRecord> QueuedAnalysisDb::FindTasMovieValidationAttempt(
    std::int64_t source_job_id,
    std::string_view worker_terminal_sha256) const {
    const auto hash = std::string(worker_terminal_sha256);
    return ExecuteRead<std::optional<TasMovieValidationAttemptRecord>>(
        [this, source_job_id, hash]() {
            return inner_ != nullptr ? inner_->FindTasMovieValidationAttempt(source_job_id, hash) : std::nullopt;
        }, std::nullopt);
}

std::optional<TasMovieValidationStatusRecord> QueuedAnalysisDb::GetTasMovieValidationStatus(
    std::string_view effective_dtm_sha256) const {
    const auto hash = std::string(effective_dtm_sha256);
    return ExecuteRead<std::optional<TasMovieValidationStatusRecord>>(
        [this, hash]() {
            return inner_ != nullptr ? inner_->GetTasMovieValidationStatus(hash) : std::nullopt;
        }, std::nullopt);
}

bool QueuedAnalysisDb::CreateTasMovieCheckpointSterilizationRequest(
    const CreateTasMovieCheckpointSterilizationRequestCommand& command,
    std::int64_t* request_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, request_id_out, error_out]() {
            return inner_ != nullptr &&
                inner_->CreateTasMovieCheckpointSterilizationRequest(
                    command, request_id_out, error_out);
        }, false, error_out);
}

std::optional<TasMovieCheckpointSterilizationRequestRecord>
QueuedAnalysisDb::GetTasMovieCheckpointSterilizationRequest(
    std::int64_t request_id) const {
    return ExecuteRead<std::optional<TasMovieCheckpointSterilizationRequestRecord>>(
        [this, request_id]() {
            return inner_ != nullptr
                ? inner_->GetTasMovieCheckpointSterilizationRequest(request_id)
                : std::nullopt;
        }, std::nullopt);
}

std::optional<TasMovieCheckpointSterilizationRequestRecord>
QueuedAnalysisDb::GetTasMovieCheckpointSterilizationRequestForWorkflowStep(
    std::int64_t workflow_step_id) const {
    return ExecuteRead<std::optional<TasMovieCheckpointSterilizationRequestRecord>>(
        [this, workflow_step_id]() {
            return inner_ != nullptr
                ? inner_->GetTasMovieCheckpointSterilizationRequestForWorkflowStep(
                      workflow_step_id)
                : std::nullopt;
        }, std::nullopt);
}

bool QueuedAnalysisDb::RecordTasMovieCheckpointSterilizationAttempt(
    const RecordTasMovieCheckpointSterilizationAttemptCommand& command,
    std::int64_t* attempt_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, attempt_id_out, error_out]() {
            return inner_ != nullptr &&
                inner_->RecordTasMovieCheckpointSterilizationAttempt(
                    command, attempt_id_out, error_out);
        }, false, error_out);
}

std::optional<TasMovieCheckpointSterilizationAttemptRecord>
QueuedAnalysisDb::GetTasMovieCheckpointSterilizationAttempt(
    std::int64_t attempt_id) const {
    return ExecuteRead<std::optional<TasMovieCheckpointSterilizationAttemptRecord>>(
        [this, attempt_id]() {
            return inner_ != nullptr
                ? inner_->GetTasMovieCheckpointSterilizationAttempt(attempt_id)
                : std::nullopt;
        }, std::nullopt);
}

std::optional<TasMovieCheckpointSterilizationAttemptRecord>
QueuedAnalysisDb::FindTasMovieCheckpointSterilizationAttempt(
    std::int64_t source_job_id,
    std::string_view worker_terminal_sha256) const {
    const auto hash = std::string(worker_terminal_sha256);
    return ExecuteRead<std::optional<TasMovieCheckpointSterilizationAttemptRecord>>(
        [this, source_job_id, hash]() {
            return inner_ != nullptr
                ? inner_->FindTasMovieCheckpointSterilizationAttempt(
                      source_job_id, hash)
                : std::nullopt;
        }, std::nullopt);
}

std::optional<std::int64_t> QueuedAnalysisDb::LookupSeedProbeRunSavestateId(std::int64_t probe_run_id) const {
    return ExecuteRead<std::optional<std::int64_t>>(
        [this, probe_run_id]() {
            return inner_ != nullptr ? inner_->LookupSeedProbeRunSavestateId(probe_run_id) : std::nullopt;
        },
        std::nullopt);
}

std::optional<SeedProbeResultRow> QueuedAnalysisDb::GetSeedProbeResult(std::int64_t probe_result_id) const {
    return ExecuteRead<std::optional<SeedProbeResultRow>>(
        [this, probe_result_id]() {
            return inner_ != nullptr ? inner_->GetSeedProbeResult(probe_result_id) : std::nullopt;
        },
        std::nullopt);
}

std::optional<SeedProbeResultRow> QueuedAnalysisDb::GetSeedProbeResultForSourceJob(std::int64_t source_job_id) const {
    return ExecuteRead<std::optional<SeedProbeResultRow>>(
        [this, source_job_id]() {
            return inner_ != nullptr ? inner_->GetSeedProbeResultForSourceJob(source_job_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<SeedProbeResultRow> QueuedAnalysisDb::ListSeedProbeResults(std::int64_t probe_run_id) const {
    return ExecuteRead<std::vector<SeedProbeResultRow>>(
        [this, probe_run_id]() {
            return inner_ != nullptr ? inner_->ListSeedProbeResults(probe_run_id) : std::vector<SeedProbeResultRow>{};
        },
        {});
}

std::optional<SeedProbeResultRow> QueuedAnalysisDb::FindConfirmedSeedProbeResultForAcceptedInputSetFrame(
    std::int64_t accepted_input_set_id,
    std::int64_t input_frame_id) const {
    return ExecuteRead<std::optional<SeedProbeResultRow>>(
        [this, accepted_input_set_id, input_frame_id]() {
            return inner_ != nullptr
                ? inner_->FindConfirmedSeedProbeResultForAcceptedInputSetFrame(
                      accepted_input_set_id,
                      input_frame_id)
                : std::nullopt;
        },
        std::nullopt);
}

std::optional<AnalysisInputSetFrameRow> QueuedAnalysisDb::GetAnalysisInputFrame(std::int64_t input_frame_id) const {
    return ExecuteRead<std::optional<AnalysisInputSetFrameRow>>(
        [this, input_frame_id]() {
            return inner_ != nullptr ? inner_->GetAnalysisInputFrame(input_frame_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<AnalysisInputSetFrameRow> QueuedAnalysisDb::ListAnalysisInputSetFrames(std::int64_t input_set_id) const {
    return ExecuteRead<std::vector<AnalysisInputSetFrameRow>>(
        [this, input_set_id]() {
            return inner_ != nullptr ? inner_->ListAnalysisInputSetFrames(input_set_id) : std::vector<AnalysisInputSetFrameRow>{};
        },
        {});
}

bool QueuedAnalysisDb::EnsureSeedProbeInputFrame(
    std::int64_t main_axis_xy_id,
    std::int64_t cstick_axis_xy_id,
    std::int64_t trigger_axis_xy_id,
    std::int64_t* input_frame_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, main_axis_xy_id, cstick_axis_xy_id, trigger_axis_xy_id, input_frame_id_out, error_out]() {
            return inner_ != nullptr
                ? inner_->EnsureSeedProbeInputFrame(main_axis_xy_id, cstick_axis_xy_id, trigger_axis_xy_id, input_frame_id_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::RecordSeedProbeObservation(
    const RecordSeedProbeObservationCommand& command,
    RecordSeedProbeObservationReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->RecordSeedProbeObservation(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::TransitionSeedProbeEvidence(
    const TransitionSeedProbeEvidenceCommand& command,
    bool* changed_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, changed_out, error_out]() {
            return inner_ != nullptr
                ? inner_->TransitionSeedProbeEvidence(command, changed_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::CreateSeedProbeSet(
    const CreateSeedProbeSetCommand& command,
    std::int64_t* probe_set_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, probe_set_id_out, error_out]() {
            return inner_ != nullptr ? inner_->CreateSeedProbeSet(command, probe_set_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::RequestSeedProbeRun(
    const RequestSeedProbeRunCommand& command,
    std::int64_t* probe_run_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, probe_run_id_out, error_out]() {
            return inner_ != nullptr ? inner_->RequestSeedProbeRun(command, probe_run_id_out, error_out) : false;
        },
        false,
        error_out);
}

std::optional<SeedProbeRunSnapshot> QueuedAnalysisDb::GetSeedProbeRun(std::int64_t probe_run_id) const {
    return ExecuteRead<std::optional<SeedProbeRunSnapshot>>(
        [this, probe_run_id]() {
            return inner_ != nullptr ? inner_->GetSeedProbeRun(probe_run_id) : std::nullopt;
        },
        std::nullopt);
}

bool QueuedAnalysisDb::UpdateSeedProbeRunStatus(
    const UpdateSeedProbeRunStatusCommand& command,
    bool* changed_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, changed_out, error_out]() {
            return inner_ != nullptr ? inner_->UpdateSeedProbeRunStatus(command, changed_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::ReplaceSeedProbeAcceptedInputFrames(
    const ReplaceSeedProbeAcceptedInputFramesCommand& command,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, error_out]() {
            return inner_ != nullptr ? inner_->ReplaceSeedProbeAcceptedInputFrames(command, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::SetSeedProbeRunEntrySavestate(
    const SetSeedProbeRunEntrySavestateCommand& command,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, error_out]() {
            return inner_ != nullptr ? inner_->SetSeedProbeRunEntrySavestate(command, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::RecordSeedProbeEncounterProjection(
    const RecordSeedProbeEncounterProjectionCommand& command,
    std::int64_t* encounter_projection_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, encounter_projection_id_out, error_out]() {
            return inner_ != nullptr
                ? inner_->RecordSeedProbeEncounterProjection(command, encounter_projection_id_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::CreateBattleSet(
    const CreateBattleSetCommand& command,
    std::int64_t* battle_set_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, battle_set_id_out, error_out]() {
            return inner_ != nullptr ? inner_->CreateBattleSet(command, battle_set_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::AddBattleSeedCandidate(
    const AddBattleSeedCandidateCommand& command,
    std::int64_t* seed_candidate_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, seed_candidate_id_out, error_out]() {
            return inner_ != nullptr ? inner_->AddBattleSeedCandidate(command, seed_candidate_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::CreateBattleTurnWave(
    const CreateBattleTurnWaveCommand& command,
    std::int64_t* wave_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, wave_id_out, error_out]() {
            return inner_ != nullptr ? inner_->CreateBattleTurnWave(command, wave_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::CreateBattleContextProbe(
    const CreateBattleContextProbeCommand& command,
    std::int64_t* context_probe_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, context_probe_id_out, error_out]() {
            return inner_ != nullptr ? inner_->CreateBattleContextProbe(command, context_probe_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::SetBattleContextProbeExecJobId(
    std::int64_t context_probe_id,
    std::int64_t exec_job_id,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, context_probe_id, exec_job_id, error_out]() {
            return inner_ != nullptr ? inner_->SetBattleContextProbeExecJobId(context_probe_id, exec_job_id, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::CompleteBattleContextProbe(
    const CompleteBattleContextProbeCommand& command,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, error_out]() {
            return inner_ != nullptr ? inner_->CompleteBattleContextProbe(command, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::RecordBattleTurnJob(
    const RecordBattleTurnJobCommand& command,
    std::int64_t* turn_job_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, turn_job_id_out, error_out]() {
            return inner_ != nullptr ? inner_->RecordBattleTurnJob(command, turn_job_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::SetBattleTurnJobExecJobId(
    std::int64_t turn_job_id,
    std::int64_t exec_job_id,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, turn_job_id, exec_job_id, error_out]() {
            return inner_ != nullptr ? inner_->SetBattleTurnJobExecJobId(turn_job_id, exec_job_id, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::UpdateBattleTurnJobResult(
    const RecordBattleTurnJobCommand& command,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, error_out]() {
            return inner_ != nullptr ? inner_->UpdateBattleTurnJobResult(command, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::CreateBattleAdvancementPool(
    const CreateBattleAdvancementPoolCommand& command,
    std::int64_t* battle_advancement_pool_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, battle_advancement_pool_id_out, error_out]() {
            return inner_ != nullptr ? inner_->CreateBattleAdvancementPool(command, battle_advancement_pool_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::EnsureBattleAdvancementPool(
    const CreateBattleAdvancementPoolCommand& command,
    std::int64_t* battle_advancement_pool_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, battle_advancement_pool_id_out, error_out]() {
            return inner_ != nullptr ? inner_->EnsureBattleAdvancementPool(command, battle_advancement_pool_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::RecordBattleAdvancementDecision(
    const RecordBattleAdvancementDecisionCommand& command,
    std::int64_t* battle_advancement_decision_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, battle_advancement_decision_id_out, error_out]() {
            return inner_ != nullptr ? inner_->RecordBattleAdvancementDecision(command, battle_advancement_decision_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::UpsertBattleManualFollowup(
    const UpsertBattleManualFollowupCommand& command,
    std::int64_t* manual_followup_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, manual_followup_id_out, error_out]() {
            return inner_ != nullptr
                ? inner_->UpsertBattleManualFollowup(command, manual_followup_id_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::UpdateBattleSetStatus(
    std::int64_t battle_set_id,
    BattleSetStatus status,
    std::optional<types::UtcTimePoint> completed_at_utc,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, battle_set_id, status, completed_at_utc, error_out]() {
            return inner_ != nullptr ? inner_->UpdateBattleSetStatus(battle_set_id, status, completed_at_utc, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::UpdateBattleTurnWaveStatus(
    std::int64_t wave_id,
    BattleTurnWaveStatus status,
    std::optional<types::UtcTimePoint> completed_at_utc,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, wave_id, status, completed_at_utc, error_out]() {
            return inner_ != nullptr ? inner_->UpdateBattleTurnWaveStatus(wave_id, status, completed_at_utc, error_out) : false;
        },
        false,
        error_out);
}

std::optional<BattleSetSnapshot> QueuedAnalysisDb::GetBattleSet(std::int64_t battle_set_id) const {
    return ExecuteRead<std::optional<BattleSetSnapshot>>(
        [this, battle_set_id]() {
            return inner_ != nullptr ? inner_->GetBattleSet(battle_set_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<BattleSeedCandidateRow> QueuedAnalysisDb::ListBattleSeedCandidates(std::int64_t battle_set_id) const {
    return ExecuteRead<std::vector<BattleSeedCandidateRow>>(
        [this, battle_set_id]() {
            return inner_ != nullptr ? inner_->ListBattleSeedCandidates(battle_set_id) : std::vector<BattleSeedCandidateRow>{};
        },
        {});
}

std::optional<BattleSeedCandidateRow> QueuedAnalysisDb::GetBattleSeedCandidate(std::int64_t seed_candidate_id) const {
    return ExecuteRead<std::optional<BattleSeedCandidateRow>>(
        [this, seed_candidate_id]() {
            return inner_ != nullptr ? inner_->GetBattleSeedCandidate(seed_candidate_id) : std::nullopt;
        },
        std::nullopt);
}

std::optional<BattleTurnWaveSnapshot> QueuedAnalysisDb::GetBattleTurnWave(std::int64_t wave_id) const {
    return ExecuteRead<std::optional<BattleTurnWaveSnapshot>>(
        [this, wave_id]() {
            return inner_ != nullptr ? inner_->GetBattleTurnWave(wave_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<BattleTurnWaveSnapshot> QueuedAnalysisDb::ListBattleTurnWaves(std::int64_t battle_set_id) const {
    return ExecuteRead<std::vector<BattleTurnWaveSnapshot>>(
        [this, battle_set_id]() {
            return inner_ != nullptr ? inner_->ListBattleTurnWaves(battle_set_id) : std::vector<BattleTurnWaveSnapshot>{};
        },
        {});
}

std::vector<BattleTurnWaveSnapshot> QueuedAnalysisDb::ListBattleTurnWavesForContextProbe(std::int64_t context_probe_id) const {
    return ExecuteRead<std::vector<BattleTurnWaveSnapshot>>(
        [this, context_probe_id]() {
            return inner_ != nullptr ? inner_->ListBattleTurnWavesForContextProbe(context_probe_id) : std::vector<BattleTurnWaveSnapshot>{};
        },
        {});
}

std::optional<BattleContextProbeSnapshot> QueuedAnalysisDb::GetBattleContextProbe(std::int64_t context_probe_id) const {
    return ExecuteRead<std::optional<BattleContextProbeSnapshot>>(
        [this, context_probe_id]() {
            return inner_ != nullptr ? inner_->GetBattleContextProbe(context_probe_id) : std::nullopt;
        },
        std::nullopt);
}

std::optional<BattleContextProbeSnapshot> QueuedAnalysisDb::GetBattleContextProbeForExecJob(std::int64_t exec_job_id) const {
    return ExecuteRead<std::optional<BattleContextProbeSnapshot>>(
        [this, exec_job_id]() {
            return inner_ != nullptr ? inner_->GetBattleContextProbeForExecJob(exec_job_id) : std::nullopt;
        },
        std::nullopt);
}

std::optional<BattleContextProbeSnapshot> QueuedAnalysisDb::GetLatestBattleContextForWave(std::int64_t wave_id) const {
    return ExecuteRead<std::optional<BattleContextProbeSnapshot>>(
        [this, wave_id]() {
            return inner_ != nullptr ? inner_->GetLatestBattleContextForWave(wave_id) : std::nullopt;
        },
        std::nullopt);
}

std::optional<BattleTurnJobSnapshot> QueuedAnalysisDb::GetBattleTurnJob(std::int64_t turn_job_id) const {
    return ExecuteRead<std::optional<BattleTurnJobSnapshot>>(
        [this, turn_job_id]() {
            return inner_ != nullptr ? inner_->GetBattleTurnJob(turn_job_id) : std::nullopt;
        },
        std::nullopt);
}

std::optional<BattleTurnJobSnapshot> QueuedAnalysisDb::GetBattleTurnJobForExecJob(std::int64_t exec_job_id) const {
    return ExecuteRead<std::optional<BattleTurnJobSnapshot>>(
        [this, exec_job_id]() {
            return inner_ != nullptr ? inner_->GetBattleTurnJobForExecJob(exec_job_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<BattleTurnJobSnapshot> QueuedAnalysisDb::ListBattleTurnJobsByOutputSavestateId(
    std::int64_t output_savestate_id) const {
    return ExecuteRead<std::vector<BattleTurnJobSnapshot>>(
        [this, output_savestate_id]() {
            return inner_ != nullptr
                ? inner_->ListBattleTurnJobsByOutputSavestateId(output_savestate_id)
                : std::vector<BattleTurnJobSnapshot>{};
        },
        {});
}

std::vector<BattleTurnJobSnapshot> QueuedAnalysisDb::ListBattleTurnJobsForWave(std::int64_t wave_id) const {
    return ExecuteRead<std::vector<BattleTurnJobSnapshot>>(
        [this, wave_id]() {
            return inner_ != nullptr ? inner_->ListBattleTurnJobsForWave(wave_id) : std::vector<BattleTurnJobSnapshot>{};
        },
        {});
}

std::vector<BattleTurnJobSnapshot> QueuedAnalysisDb::ListBattleTurnJobsForBattleTurn(
    std::int64_t battle_set_id,
    int turn_index) const {
    return ExecuteRead<std::vector<BattleTurnJobSnapshot>>(
        [this, battle_set_id, turn_index]() {
            return inner_ != nullptr
                ? inner_->ListBattleTurnJobsForBattleTurn(battle_set_id, turn_index)
                : std::vector<BattleTurnJobSnapshot>{};
        },
        {});
}

std::vector<BattleAdvancementDecisionRow> QueuedAnalysisDb::ListBattleAdvancementDecisionsForPool(
    std::int64_t battle_advancement_pool_id) const {
    return ExecuteRead<std::vector<BattleAdvancementDecisionRow>>(
        [this, battle_advancement_pool_id]() {
            return inner_ != nullptr
                ? inner_->ListBattleAdvancementDecisionsForPool(battle_advancement_pool_id)
                : std::vector<BattleAdvancementDecisionRow>{};
        },
        {});
}

bool QueuedAnalysisDb::CreateBattleCompletion(
    const CreateBattleCompletionCommand& command,
    std::int64_t* battle_completion_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, battle_completion_id_out, error_out]() {
            return inner_ != nullptr
                ? inner_->CreateBattleCompletion(command, battle_completion_id_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::BindBattleCompletionExecutionJob(
    const BindBattleCompletionExecutionJobCommand& command,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, error_out]() {
            return inner_ != nullptr
                ? inner_->BindBattleCompletionExecutionJob(command, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::CompleteBattleCompletion(
    const CompleteBattleCompletionCommand& command,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, error_out]() {
            return inner_ != nullptr ? inner_->CompleteBattleCompletion(command, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::FailBattleCompletion(
    const FailBattleCompletionCommand& command,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, error_out]() {
            return inner_ != nullptr ? inner_->FailBattleCompletion(command, error_out) : false;
        },
        false,
        error_out);
}

std::optional<BattleCompletionRecord> QueuedAnalysisDb::GetBattleCompletion(
    std::int64_t battle_completion_id) const {
    return ExecuteRead<std::optional<BattleCompletionRecord>>(
        [this, battle_completion_id]() {
            return inner_ != nullptr ? inner_->GetBattleCompletion(battle_completion_id) : std::nullopt;
        },
        std::nullopt);
}

bool QueuedAnalysisDb::CreateBattleResults(
    const CreateBattleResultsCommand& command,
    std::int64_t* battle_results_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, battle_results_id_out, error_out]() {
            return inner_ != nullptr ? inner_->CreateBattleResults(command, battle_results_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::BindBattleResultsExecutionJob(
    const BindBattleResultsExecutionJobCommand& command,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, error_out]() {
            return inner_ != nullptr
                ? inner_->BindBattleResultsExecutionJob(command, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::CompleteBattleResults(
    const CompleteBattleResultsCommand& command,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, error_out]() {
            return inner_ != nullptr ? inner_->CompleteBattleResults(command, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::FailBattleResults(
    const FailBattleResultsCommand& command,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, error_out]() {
            return inner_ != nullptr ? inner_->FailBattleResults(command, error_out) : false;
        },
        false,
        error_out);
}

std::optional<BattleResultsRecord> QueuedAnalysisDb::GetBattleResults(std::int64_t battle_results_id) const {
    return ExecuteRead<std::optional<BattleResultsRecord>>(
        [this, battle_results_id]() {
            return inner_ != nullptr ? inner_->GetBattleResults(battle_results_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<events::EventEnvelope> QueuedAnalysisDb::ReadUnpublishedOutboxBatch(
    std::int64_t after_outbox_id,
    int max_batch_size) {
    return ExecuteRead<std::vector<events::EventEnvelope>>(
        [this, after_outbox_id, max_batch_size]() {
            return inner_ != nullptr
                ? inner_->ReadUnpublishedOutboxBatch(after_outbox_id, max_batch_size)
                : std::vector<events::EventEnvelope>{};
        },
        {});
}

bool QueuedAnalysisDb::MarkOutboxPublished(
    std::int64_t outbox_id,
    types::UtcTimePoint published_at_utc) {
    return ExecuteWrite<bool>(
        [this, outbox_id, published_at_utc]() {
            return inner_ != nullptr ? inner_->MarkOutboxPublished(outbox_id, published_at_utc) : false;
        },
        false);
}

bool QueuedAnalysisDb::MarkOutboxPublishFailure(
    std::int64_t outbox_id,
    std::string_view last_error) {
    const auto last_error_copy = std::string(last_error);
    return ExecuteWrite<bool>(
        [this, outbox_id, last_error_copy]() {
            return inner_ != nullptr ? inner_->MarkOutboxPublishFailure(outbox_id, last_error_copy) : false;
        },
        false);
}

retention::OutboxRetentionPreview QueuedAnalysisDb::PreviewOutboxRetention(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy) const {
    return ExecuteRead<retention::OutboxRetentionPreview>(
        [this, subscriptions, now_utc, policy]() {
            return inner_ != nullptr ? inner_->PreviewOutboxRetention(subscriptions, now_utc, policy) : retention::OutboxRetentionPreview{};
        },
        {});
}

bool QueuedAnalysisDb::PurgeOutboxThroughRetentionFloor(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy,
    int max_rows,
    int* rows_deleted_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, subscriptions, now_utc, policy, max_rows, rows_deleted_out, error_out]() {
            return inner_ != nullptr
                ? inner_->PurgeOutboxThroughRetentionFloor(subscriptions, now_utc, policy, max_rows, rows_deleted_out, error_out)
                : false;
        },
        false,
        error_out);
}

std::optional<SeedProbePayloadRecord> QueuedAnalysisDb::ResolveSeedProbePayload(
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    const auto payload_ref_kind_copy = std::string(payload_ref_kind);
    return ExecuteRead<std::optional<SeedProbePayloadRecord>>(
        [this, event_version, payload_ref_kind_copy, payload_ref_id]() {
            return inner_ != nullptr
                ? inner_->ResolveSeedProbePayload(event_version, payload_ref_kind_copy, payload_ref_id)
                : std::nullopt;
        },
        std::nullopt);
}

std::optional<SeedProbePayloadRecord> QueuedAnalysisDb::ResolveSeedProbePayload(
    const events::EventEnvelope& envelope) const {
    return ExecuteRead<std::optional<SeedProbePayloadRecord>>(
        [this, envelope]() {
            return inner_ != nullptr ? inner_->ResolveSeedProbePayload(envelope) : std::nullopt;
        },
        std::nullopt);
}

std::optional<BattlePayloadRecord> QueuedAnalysisDb::ResolveBattlePayload(
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    const auto payload_ref_kind_copy = std::string(payload_ref_kind);
    return ExecuteRead<std::optional<BattlePayloadRecord>>(
        [this, event_version, payload_ref_kind_copy, payload_ref_id]() {
            return inner_ != nullptr
                ? inner_->ResolveBattlePayload(event_version, payload_ref_kind_copy, payload_ref_id)
                : std::nullopt;
        },
        std::nullopt);
}

std::optional<BattlePayloadRecord> QueuedAnalysisDb::ResolveBattlePayload(
    const events::EventEnvelope& envelope) const {
    return ExecuteRead<std::optional<BattlePayloadRecord>>(
        [this, envelope]() {
            return inner_ != nullptr ? inner_->ResolveBattlePayload(envelope) : std::nullopt;
        },
        std::nullopt);
}

std::optional<SpinePayloadRecord> QueuedAnalysisDb::ResolveSpinePayload(
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    const auto payload_ref_kind_copy = std::string(payload_ref_kind);
    return ExecuteRead<std::optional<SpinePayloadRecord>>(
        [this, event_version, payload_ref_kind_copy, payload_ref_id]() {
            return inner_ != nullptr
                ? inner_->ResolveSpinePayload(event_version, payload_ref_kind_copy, payload_ref_id)
                : std::nullopt;
        },
        std::nullopt);
}

std::optional<SpinePayloadRecord> QueuedAnalysisDb::ResolveSpinePayload(
    const events::EventEnvelope& envelope) const {
    return ExecuteRead<std::optional<SpinePayloadRecord>>(
        [this, envelope]() {
            return inner_ != nullptr ? inner_->ResolveSpinePayload(envelope) : std::nullopt;
        },
        std::nullopt);
}

template <typename Result, typename Fn>
Result QueuedAnalysisDb::ExecuteRead(
    Fn&& fn,
    Result fallback,
    std::string* error_out,
    const std::source_location& location) const {
    return savor::db::core::QueuedDbExecutor::ExecuteQueued<Result>(
        *read_lane_,
        sqlite_call_mtx_,
        savor::db::core::MakeQueuedDbOperationName("Analysis", location),
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

template <typename Result, typename Fn>
Result QueuedAnalysisDb::ExecuteWrite(
    Fn&& fn,
    Result fallback,
    std::string* error_out,
    const std::source_location& location) const {
    return savor::db::core::QueuedDbExecutor::ExecuteQueued<Result>(
        *write_lane_,
        sqlite_call_mtx_,
        savor::db::core::MakeQueuedDbOperationName("Analysis", location),
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

} // namespace savor::db::analysis
