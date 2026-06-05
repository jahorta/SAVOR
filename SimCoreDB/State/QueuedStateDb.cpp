#include "QueuedStateDb.h"

#include <utility>

namespace simcore::db::state {
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

QueuedStateDb::QueuedStateDb(
    simcore::db::IStateDb* inner,
    simcore::db::core::QueuedDbConfig config)
    : inner_(inner)
    , config_(config)
    , read_lane_(std::make_unique<simcore::db::core::QueuedDbLane>("state-read", config_.read_capacity))
    , write_lane_(std::make_unique<simcore::db::core::QueuedDbLane>("state-write", config_.write_capacity)) {
}

QueuedStateDb::~QueuedStateDb() {
    Stop();
}

bool QueuedStateDb::Start(std::string* error_out) {
    if (inner_ == nullptr) {
        SetError(error_out, "state db inner database is null");
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

void QueuedStateDb::Stop() {
    if (read_lane_) {
        read_lane_->Stop();
    }
    if (write_lane_) {
        write_lane_->Stop();
    }
}

bool QueuedStateDb::IsRunning() const {
    return read_lane_ != nullptr
        && write_lane_ != nullptr
        && read_lane_->IsRunning()
        && write_lane_->IsRunning();
}

simcore::db::core::QueuedDbTelemetrySnapshot QueuedStateDb::GetTelemetrySnapshot() const {
    return BuildTelemetrySnapshot(write_lane_.get(), read_lane_.get());
}

bool QueuedStateDb::StoreArtifact(
    const StoreArtifactCommand& command,
    std::int64_t* artifact_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, artifact_id_out, error_out]() {
            return inner_ != nullptr ? inner_->StoreArtifact(command, artifact_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedStateDb::CreateSavestate(
    const CreateSavestateCommand& command,
    std::int64_t* savestate_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, savestate_id_out, error_out]() {
            return inner_ != nullptr ? inner_->CreateSavestate(command, savestate_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedStateDb::DeriveSavestate(
    const DeriveSavestateCommand& command,
    std::int64_t* derivation_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, derivation_id_out, error_out]() {
            return inner_ != nullptr ? inner_->DeriveSavestate(command, derivation_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedStateDb::CreateTasVariant(
    const CreateTasVariantCommand& command,
    std::int64_t* tas_variant_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, tas_variant_id_out, error_out]() {
            return inner_ != nullptr ? inner_->CreateTasVariant(command, tas_variant_id_out, error_out) : false;
        },
        false,
        error_out);
}

std::optional<TasVariantRecord> QueuedStateDb::GetTasVariant(
    std::int64_t tas_variant_id) const {
    return ExecuteRead<std::optional<TasVariantRecord>>(
        [this, tas_variant_id]() {
            return inner_ != nullptr ? inner_->GetTasVariant(tas_variant_id) : std::nullopt;
        },
        std::nullopt);
}

bool QueuedStateDb::UpdateTasVariantProducedSavestate(
    const UpdateTasVariantProducedSavestateCommand& command,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, error_out]() {
            return inner_ != nullptr ? inner_->UpdateTasVariantProducedSavestate(command, error_out) : false;
        },
        false,
        error_out);
}

std::optional<std::string> QueuedStateDb::MaterializeArtifactToDirectory(
    std::int64_t artifact_id,
    std::string_view output_directory,
    std::string* error_out) const {
    const auto output_directory_copy = std::string(output_directory);
    return ExecuteRead<std::optional<std::string>>(
        [this, artifact_id, output_directory_copy, error_out]() {
            return inner_ != nullptr ? inner_->MaterializeArtifactToDirectory(artifact_id, output_directory_copy, error_out) : std::nullopt;
        },
        std::nullopt,
        error_out);
}

std::optional<std::string> QueuedStateDb::MaterializeArtifactToPath(
    std::int64_t artifact_id,
    std::string_view output_path,
    std::string* error_out) const {
    const auto output_path_copy = std::string(output_path);
    return ExecuteRead<std::optional<std::string>>(
        [this, artifact_id, output_path_copy, error_out]() {
            return inner_ != nullptr ? inner_->MaterializeArtifactToPath(artifact_id, output_path_copy, error_out) : std::nullopt;
        },
        std::nullopt,
        error_out);
}

std::optional<std::string> QueuedStateDb::MaterializeSavestateToPath(
    std::int64_t savestate_id,
    std::string_view output_path,
    std::string* error_out) const {
    const auto output_path_copy = std::string(output_path);
    return ExecuteRead<std::optional<std::string>>(
        [this, savestate_id, output_path_copy, error_out]() {
            return inner_ != nullptr ? inner_->MaterializeSavestateToPath(savestate_id, output_path_copy, error_out) : std::nullopt;
        },
        std::nullopt,
        error_out);
}

std::vector<events::EventEnvelope> QueuedStateDb::ReadUnpublishedOutboxBatch(
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

bool QueuedStateDb::MarkOutboxPublished(
    std::int64_t outbox_id,
    types::UtcTimePoint published_at_utc) {
    return ExecuteWrite<bool>(
        [this, outbox_id, published_at_utc]() {
            return inner_ != nullptr ? inner_->MarkOutboxPublished(outbox_id, published_at_utc) : false;
        },
        false);
}

bool QueuedStateDb::MarkOutboxPublishFailure(
    std::int64_t outbox_id,
    std::string_view last_error) {
    const auto last_error_copy = std::string(last_error);
    return ExecuteWrite<bool>(
        [this, outbox_id, last_error_copy]() {
            return inner_ != nullptr ? inner_->MarkOutboxPublishFailure(outbox_id, last_error_copy) : false;
        },
        false);
}

retention::OutboxRetentionPreview QueuedStateDb::PreviewOutboxRetention(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy) const {
    return ExecuteRead<retention::OutboxRetentionPreview>(
        [this, subscriptions, now_utc, policy]() {
            return inner_ != nullptr ? inner_->PreviewOutboxRetention(subscriptions, now_utc, policy) : retention::OutboxRetentionPreview{};
        },
        {});
}

bool QueuedStateDb::PurgeOutboxThroughRetentionFloor(
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

std::optional<ArtifactPayloadRecord> QueuedStateDb::ResolveArtifactPayload(
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    const auto payload_ref_kind_copy = std::string(payload_ref_kind);
    return ExecuteRead<std::optional<ArtifactPayloadRecord>>(
        [this, event_version, payload_ref_kind_copy, payload_ref_id]() {
            return inner_ != nullptr
                ? inner_->ResolveArtifactPayload(event_version, payload_ref_kind_copy, payload_ref_id)
                : std::nullopt;
        },
        std::nullopt);
}

std::optional<ArtifactPayloadRecord> QueuedStateDb::ResolveArtifactPayload(
    const events::EventEnvelope& envelope) const {
    return ExecuteRead<std::optional<ArtifactPayloadRecord>>(
        [this, envelope]() {
            return inner_ != nullptr ? inner_->ResolveArtifactPayload(envelope) : std::nullopt;
        },
        std::nullopt);
}

template <typename Result, typename Fn>
Result QueuedStateDb::ExecuteRead(Fn&& fn, Result fallback, std::string* error_out) const {
    return simcore::db::core::QueuedDbExecutor::ExecuteQueued<Result>(
        *read_lane_,
        sqlite_call_mtx_,
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

template <typename Result, typename Fn>
Result QueuedStateDb::ExecuteWrite(Fn&& fn, Result fallback, std::string* error_out) const {
    return simcore::db::core::QueuedDbExecutor::ExecuteQueued<Result>(
        *write_lane_,
        sqlite_call_mtx_,
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

} // namespace simcore::db::state
