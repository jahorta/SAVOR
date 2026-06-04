#include "QueuedAuthoringDb.h"

#include <utility>

namespace simcore::db {
namespace {

void SetError(std::string* error_out, std::string message) {
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
}

core::QueuedDbTelemetrySnapshot BuildTelemetrySnapshot(
    const core::QueuedDbLane* write_lane,
    const core::QueuedDbLane* read_lane) {
    core::QueuedDbTelemetrySnapshot snapshot{};
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

QueuedAuthoringDb::QueuedAuthoringDb(
    IAuthoringDb* inner,
    core::QueuedDbConfig config)
    : inner_(inner)
    , config_(config)
    , read_lane_(std::make_unique<core::QueuedDbLane>("authoring-read", config_.read_capacity))
    , write_lane_(std::make_unique<core::QueuedDbLane>("authoring-write", config_.write_capacity)) {
}

QueuedAuthoringDb::~QueuedAuthoringDb() {
    Stop();
}

bool QueuedAuthoringDb::Start(std::string* error_out) {
    if (inner_ == nullptr) {
        SetError(error_out, "authoring db inner database is null");
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

void QueuedAuthoringDb::Stop() {
    if (read_lane_) {
        read_lane_->Stop();
    }
    if (write_lane_) {
        write_lane_->Stop();
    }
}

bool QueuedAuthoringDb::IsRunning() const {
    return read_lane_ != nullptr
        && write_lane_ != nullptr
        && read_lane_->IsRunning()
        && write_lane_->IsRunning();
}

core::QueuedDbTelemetrySnapshot QueuedAuthoringDb::GetTelemetrySnapshot() const {
    return BuildTelemetrySnapshot(write_lane_.get(), read_lane_.get());
}

bool QueuedAuthoringDb::SaveSeedProbeSpec(
    const SaveSeedProbeSpecCommand& command,
    std::int64_t* seed_probe_spec_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, seed_probe_spec_id_out, error_out]() {
            return inner_ != nullptr ? inner_->SaveSeedProbeSpec(command, seed_probe_spec_id_out, error_out) : false;
        },
        false,
        error_out);
}

std::optional<SeedProbeSpecSnapshot> QueuedAuthoringDb::GetSeedProbeSpec(
    std::int64_t seed_probe_spec_id) const {
    return ExecuteRead<std::optional<SeedProbeSpecSnapshot>>(
        [this, seed_probe_spec_id]() {
            return inner_ != nullptr ? inner_->GetSeedProbeSpec(seed_probe_spec_id) : std::nullopt;
        },
        std::nullopt);
}

bool QueuedAuthoringDb::SaveTasSpec(
    const SaveTasSpecCommand& command,
    std::int64_t* tas_spec_id_out,
    std::int64_t* tas_spec_base_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, tas_spec_id_out, tas_spec_base_id_out, error_out]() {
            return inner_ != nullptr ? inner_->SaveTasSpec(command, tas_spec_id_out, tas_spec_base_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAuthoringDb::SaveBattleRunSpec(
    const SaveBattleRunSpecCommand& command,
    std::int64_t* battle_run_spec_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, battle_run_spec_id_out, error_out]() {
            return inner_ != nullptr ? inner_->SaveBattleRunSpec(command, battle_run_spec_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAuthoringDb::SavePlan(
    const SavePlanCommand& command,
    std::int64_t* plan_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, plan_id_out, error_out]() {
            return inner_ != nullptr ? inner_->SavePlan(command, plan_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAuthoringDb::SavePredicateSpec(
    const SavePredicateSpecCommand& command,
    std::int64_t* predicate_spec_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, predicate_spec_id_out, error_out]() {
            return inner_ != nullptr ? inner_->SavePredicateSpec(command, predicate_spec_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAuthoringDb::SaveExplorerSettings(
    const SaveExplorerSettingsCommand& command,
    std::int64_t* explorer_settings_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, explorer_settings_id_out, error_out]() {
            return inner_ != nullptr ? inner_->SaveExplorerSettings(command, explorer_settings_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAuthoringDb::SaveTemplate(
    const SaveTemplateCommand& command,
    std::int64_t* template_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, template_id_out, error_out]() {
            return inner_ != nullptr ? inner_->SaveTemplate(command, template_id_out, error_out) : false;
        },
        false,
        error_out);
}

std::vector<events::EventEnvelope> QueuedAuthoringDb::ReadUnpublishedOutboxBatch(
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

bool QueuedAuthoringDb::MarkOutboxPublished(
    std::int64_t outbox_id,
    types::UtcTimePoint published_at_utc) {
    return ExecuteWrite<bool>(
        [this, outbox_id, published_at_utc]() {
            return inner_ != nullptr ? inner_->MarkOutboxPublished(outbox_id, published_at_utc) : false;
        },
        false);
}

bool QueuedAuthoringDb::MarkOutboxPublishFailure(
    std::int64_t outbox_id,
    std::string_view last_error) {
    const auto last_error_copy = std::string(last_error);
    return ExecuteWrite<bool>(
        [this, outbox_id, last_error_copy]() {
            return inner_ != nullptr ? inner_->MarkOutboxPublishFailure(outbox_id, last_error_copy) : false;
        },
        false);
}

retention::OutboxRetentionPreview QueuedAuthoringDb::PreviewOutboxRetention(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy) const {
    return ExecuteRead<retention::OutboxRetentionPreview>(
        [this, subscriptions, now_utc, policy]() {
            return inner_ != nullptr ? inner_->PreviewOutboxRetention(subscriptions, now_utc, policy) : retention::OutboxRetentionPreview{};
        },
        {});
}

bool QueuedAuthoringDb::PurgeOutboxThroughRetentionFloor(
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

std::optional<AuthoringPayloadRecord> QueuedAuthoringDb::ResolveAuthoringPayload(
    const events::EventEnvelope& envelope) const {
    return ExecuteRead<std::optional<AuthoringPayloadRecord>>(
        [this, envelope]() {
            return inner_ != nullptr ? inner_->ResolveAuthoringPayload(envelope) : std::nullopt;
        },
        std::nullopt);
}

std::optional<AuthoringPayloadRecord> QueuedAuthoringDb::ResolveAuthoringPayload(
    std::string_view event_type,
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    const auto event_type_copy = std::string(event_type);
    const auto payload_ref_kind_copy = std::string(payload_ref_kind);
    return ExecuteRead<std::optional<AuthoringPayloadRecord>>(
        [this, event_type_copy, event_version, payload_ref_kind_copy, payload_ref_id]() {
            return inner_ != nullptr
                ? inner_->ResolveAuthoringPayload(event_type_copy, event_version, payload_ref_kind_copy, payload_ref_id)
                : std::nullopt;
        },
        std::nullopt);
}

template <typename Result, typename Fn>
Result QueuedAuthoringDb::ExecuteRead(Fn&& fn, Result fallback, std::string* error_out) const {
    return core::QueuedDbExecutor::ExecuteQueued<Result>(
        *read_lane_,
        sqlite_call_mtx_,
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

template <typename Result, typename Fn>
Result QueuedAuthoringDb::ExecuteWrite(Fn&& fn, Result fallback, std::string* error_out) const {
    return core::QueuedDbExecutor::ExecuteQueued<Result>(
        *write_lane_,
        sqlite_call_mtx_,
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

} // namespace simcore::db
