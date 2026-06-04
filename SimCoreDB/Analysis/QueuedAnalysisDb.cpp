#include "QueuedAnalysisDb.h"

#include <utility>

namespace simcore::db::analysis {
namespace {

void SetError(std::string* error_out, std::string message) {
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
}

simcore::db::core::QueuedDbTelemetrySnapshot BuildTelemetrySnapshot(
    const simcore::db::core::QueuedDbLane* write_lane,
    const simcore::db::core::QueuedDbLane* read_lane) {
    simcore::db::core::QueuedDbTelemetrySnapshot snapshot{};
    if (write_lane != nullptr) {
        const auto lane = write_lane->GetTelemetrySnapshot();
        snapshot.write_depth = lane.depth;
        snapshot.write_enqueued = lane.enqueued;
        snapshot.write_rejected = lane.rejected;
        snapshot.write_completed = lane.completed;
        snapshot.write_failed = lane.failed;
    }
    if (read_lane != nullptr) {
        const auto lane = read_lane->GetTelemetrySnapshot();
        snapshot.read_depth = lane.depth;
        snapshot.read_enqueued = lane.enqueued;
        snapshot.read_rejected = lane.rejected;
        snapshot.read_completed = lane.completed;
        snapshot.read_failed = lane.failed;
    }
    return snapshot;
}

} // namespace

QueuedAnalysisDb::QueuedAnalysisDb(
    simcore::db::IAnalysisDb* inner,
    simcore::db::core::QueuedDbConfig config)
    : inner_(inner)
    , config_(config)
    , read_lane_(std::make_unique<simcore::db::core::QueuedDbLane>("analysis-read", config_.read_capacity))
    , write_lane_(std::make_unique<simcore::db::core::QueuedDbLane>("analysis-write", config_.write_capacity)) {
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

simcore::db::core::QueuedDbTelemetrySnapshot QueuedAnalysisDb::GetTelemetrySnapshot() const {
    return BuildTelemetrySnapshot(write_lane_.get(), read_lane_.get());
}

std::optional<std::int64_t> QueuedAnalysisDb::LookupSeedProbeRunSavestateId(std::int64_t probe_run_id) const {
    return ExecuteRead<std::optional<std::int64_t>>(
        [this, probe_run_id]() {
            return inner_ != nullptr ? inner_->LookupSeedProbeRunSavestateId(probe_run_id) : std::nullopt;
        },
        std::nullopt);
}

std::optional<std::int64_t> QueuedAnalysisDb::LookupSeedProbeResultId(std::int64_t probe_run_id) const {
    return ExecuteRead<std::optional<std::int64_t>>(
        [this, probe_run_id]() {
            return inner_ != nullptr ? inner_->LookupSeedProbeResultId(probe_run_id) : std::nullopt;
        },
        std::nullopt);
}

std::optional<std::int64_t> QueuedAnalysisDb::LookupSeedProbeNeutralSeed(std::int64_t probe_run_id) const {
    return ExecuteRead<std::optional<std::int64_t>>(
        [this, probe_run_id]() {
            return inner_ != nullptr ? inner_->LookupSeedProbeNeutralSeed(probe_run_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<SeedProbeGridSeedRow> QueuedAnalysisDb::ListSeedProbeGridSeeds(std::int64_t probe_run_id) const {
    return ExecuteRead<std::vector<SeedProbeGridSeedRow>>(
        [this, probe_run_id]() {
            return inner_ != nullptr ? inner_->ListSeedProbeGridSeeds(probe_run_id) : std::vector<SeedProbeGridSeedRow>{};
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

bool QueuedAnalysisDb::EnsureSeedProbeUniqueSeedDelta(
    const RecordSeedProbeUniqueSeedCommand& command,
    bool* inserted_out,
    std::int64_t* unique_seed_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, inserted_out, unique_seed_id_out, error_out]() {
            return inner_ != nullptr
                ? inner_->EnsureSeedProbeUniqueSeedDelta(command, inserted_out, unique_seed_id_out, error_out)
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

bool QueuedAnalysisDb::CreateSeedProbeRunForSet(
    std::int64_t probe_set_id,
    std::int64_t* probe_run_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, probe_set_id, probe_run_id_out, error_out]() {
            return inner_ != nullptr ? inner_->CreateSeedProbeRunForSet(probe_set_id, probe_run_id_out, error_out) : false;
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

bool QueuedAnalysisDb::SetSeedProbeRunNeutralSeed(
    std::int64_t probe_run_id,
    std::int64_t neutral_seed_value,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, probe_run_id, neutral_seed_value, error_out]() {
            return inner_ != nullptr ? inner_->SetSeedProbeRunNeutralSeed(probe_run_id, neutral_seed_value, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::RecordSeedProbeNeutralSeed(
    const RecordSeedProbeNeutralSeedCommand& command,
    std::int64_t* neutral_seed_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, neutral_seed_id_out, error_out]() {
            return inner_ != nullptr ? inner_->RecordSeedProbeNeutralSeed(command, neutral_seed_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::RecordSeedProbeGridSeed(
    const RecordSeedProbeGridSeedCommand& command,
    std::int64_t* grid_seed_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, grid_seed_id_out, error_out]() {
            return inner_ != nullptr ? inner_->RecordSeedProbeGridSeed(command, grid_seed_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::RecordSeedProbeUniqueSeed(
    const RecordSeedProbeUniqueSeedCommand& command,
    std::int64_t* unique_seed_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, unique_seed_id_out, error_out]() {
            return inner_ != nullptr ? inner_->RecordSeedProbeUniqueSeed(command, unique_seed_id_out, error_out) : false;
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

bool QueuedAnalysisDb::CompleteSeedProbeRun(
    const CompleteSeedProbeRunCommand& command,
    std::int64_t* probe_result_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, probe_result_id_out, error_out]() {
            return inner_ != nullptr ? inner_->CompleteSeedProbeRun(command, probe_result_id_out, error_out) : false;
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

bool QueuedAnalysisDb::CreateBattleSelectionPool(
    const CreateBattleSelectionPoolCommand& command,
    std::int64_t* selection_pool_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, selection_pool_id_out, error_out]() {
            return inner_ != nullptr ? inner_->CreateBattleSelectionPool(command, selection_pool_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::RecordBattleSelectionDecision(
    const RecordBattleSelectionDecisionCommand& command,
    std::int64_t* selection_decision_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, selection_decision_id_out, error_out]() {
            return inner_ != nullptr ? inner_->RecordBattleSelectionDecision(command, selection_decision_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAnalysisDb::UpsertBattleTerminalFollowup(
    const UpsertBattleTerminalFollowupCommand& command,
    std::int64_t* terminal_followup_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, terminal_followup_id_out, error_out]() {
            return inner_ != nullptr
                ? inner_->UpsertBattleTerminalFollowup(command, terminal_followup_id_out, error_out)
                : false;
        },
        false,
        error_out);
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
Result QueuedAnalysisDb::ExecuteRead(Fn&& fn, Result fallback, std::string* error_out) const {
    return simcore::db::core::QueuedDbExecutor::ExecuteQueued<Result>(
        *read_lane_,
        sqlite_call_mtx_,
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

template <typename Result, typename Fn>
Result QueuedAnalysisDb::ExecuteWrite(Fn&& fn, Result fallback, std::string* error_out) const {
    return simcore::db::core::QueuedDbExecutor::ExecuteQueued<Result>(
        *write_lane_,
        sqlite_call_mtx_,
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

} // namespace simcore::db::analysis
