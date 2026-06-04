#include "QueuedUiReadDb.h"

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

QueuedUiReadDb::QueuedUiReadDb(
    IUiReadDb* inner,
    core::QueuedDbConfig config)
    : inner_(inner)
    , config_(config)
    , read_lane_(std::make_unique<core::QueuedDbLane>("ui-read-read", config_.read_capacity))
    , write_lane_(std::make_unique<core::QueuedDbLane>("ui-read-write", config_.write_capacity)) {
}

QueuedUiReadDb::~QueuedUiReadDb() {
    Stop();
}

bool QueuedUiReadDb::Start(std::string* error_out) {
    if (inner_ == nullptr) {
        SetError(error_out, "ui read db inner database is null");
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

void QueuedUiReadDb::Stop() {
    if (read_lane_) {
        read_lane_->Stop();
    }
    if (write_lane_) {
        write_lane_->Stop();
    }
}

bool QueuedUiReadDb::IsRunning() const {
    return read_lane_ != nullptr
        && write_lane_ != nullptr
        && read_lane_->IsRunning()
        && write_lane_->IsRunning();
}

core::QueuedDbTelemetrySnapshot QueuedUiReadDb::GetTelemetrySnapshot() const {
    return BuildTelemetrySnapshot(write_lane_.get(), read_lane_.get());
}

std::optional<UiProjectionSubscription> QueuedUiReadDb::GetProjectionSubscription(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table) const {
    return ExecuteRead<std::optional<UiProjectionSubscription>>(
        [this, projector_name, source_context, source_outbox_table]() {
            return inner_ != nullptr
                ? inner_->GetProjectionSubscription(projector_name, source_context, source_outbox_table)
                : std::nullopt;
        },
        std::nullopt);
}

std::vector<UiProjectionSubscription> QueuedUiReadDb::ListProjectionSubscriptions(
    const std::string& source_context,
    const std::string& source_outbox_table) const {
    return ExecuteRead<std::vector<UiProjectionSubscription>>(
        [this, source_context, source_outbox_table]() {
            return inner_ != nullptr
                ? inner_->ListProjectionSubscriptions(source_context, source_outbox_table)
                : std::vector<UiProjectionSubscription>{};
        },
        {});
}

std::optional<std::int64_t> QueuedUiReadDb::ComputeSafeFloorOutboxId(
    const std::string& source_context,
    const std::string& source_outbox_table) const {
    return ExecuteRead<std::optional<std::int64_t>>(
        [this, source_context, source_outbox_table]() {
            return inner_ != nullptr
                ? inner_->ComputeSafeFloorOutboxId(source_context, source_outbox_table)
                : std::nullopt;
        },
        std::nullopt);
}

std::optional<UiProjectionSubscription> QueuedUiReadDb::GetOrCreateProjectionSubscription(
    const UiProjectionSubscription& subscription) {
    return ExecuteWrite<std::optional<UiProjectionSubscription>>(
        [this, subscription]() {
            return inner_ != nullptr ? inner_->GetOrCreateProjectionSubscription(subscription) : std::nullopt;
        },
        std::nullopt);
}

bool QueuedUiReadDb::AdvanceProjectionSubscriptionCursor(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    std::int64_t last_outbox_id,
    const std::string& last_event_id,
    types::UtcTimePoint updated_at_utc,
    const std::optional<UiProjectionSubscriptionBatchAudit>& batch_audit) {
    return ExecuteWrite<bool>(
        [this, projector_name, source_context, source_outbox_table, last_outbox_id, last_event_id, updated_at_utc, batch_audit]() {
            return inner_ != nullptr
                ? inner_->AdvanceProjectionSubscriptionCursor(
                    projector_name,
                    source_context,
                    source_outbox_table,
                    last_outbox_id,
                    last_event_id,
                    updated_at_utc,
                    batch_audit)
                : false;
        },
        false);
}

bool QueuedUiReadDb::SetProjectionSubscriptionError(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    const std::string& last_error,
    types::UtcTimePoint updated_at_utc) {
    return ExecuteWrite<bool>(
        [this, projector_name, source_context, source_outbox_table, last_error, updated_at_utc]() {
            return inner_ != nullptr
                ? inner_->SetProjectionSubscriptionError(projector_name, source_context, source_outbox_table, last_error, updated_at_utc)
                : false;
        },
        false);
}

bool QueuedUiReadDb::PauseProjectionSubscription(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    types::UtcTimePoint updated_at_utc,
    const std::string& reason) {
    return ExecuteWrite<bool>(
        [this, projector_name, source_context, source_outbox_table, updated_at_utc, reason]() {
            return inner_ != nullptr
                ? inner_->PauseProjectionSubscription(projector_name, source_context, source_outbox_table, updated_at_utc, reason)
                : false;
        },
        false);
}

bool QueuedUiReadDb::ResumeProjectionSubscription(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    types::UtcTimePoint updated_at_utc) {
    return ExecuteWrite<bool>(
        [this, projector_name, source_context, source_outbox_table, updated_at_utc]() {
            return inner_ != nullptr
                ? inner_->ResumeProjectionSubscription(projector_name, source_context, source_outbox_table, updated_at_utc)
                : false;
        },
        false);
}

template <typename Result, typename Fn>
Result QueuedUiReadDb::ExecuteRead(Fn&& fn, Result fallback, std::string* error_out) const {
    return core::QueuedDbExecutor::ExecuteQueued<Result>(
        *read_lane_,
        sqlite_call_mtx_,
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

template <typename Result, typename Fn>
Result QueuedUiReadDb::ExecuteWrite(Fn&& fn, Result fallback, std::string* error_out) const {
    return core::QueuedDbExecutor::ExecuteQueued<Result>(
        *write_lane_,
        sqlite_call_mtx_,
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

} // namespace simcore::db
