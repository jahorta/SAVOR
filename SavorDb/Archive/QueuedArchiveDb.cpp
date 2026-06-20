#include "QueuedArchiveDb.h"

#include <utility>

namespace savor::db {
namespace {

void SetError(std::string* error_out, std::string message) {
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
}

core::QueuedDbTelemetrySnapshot BuildTelemetrySnapshot(
    const core::QueuedDbLane* write_lane,
    const core::QueuedDbLane* read_lane) {
    return core::BuildQueuedDbTelemetrySnapshot(write_lane, read_lane);
}

} // namespace

QueuedArchiveDb::QueuedArchiveDb(
    IArchiveDb* inner,
    core::QueuedDbConfig config)
    : inner_(inner)
    , config_(config)
    , read_lane_(std::make_unique<core::QueuedDbLane>("archive-read", config_.read_capacity))
    , write_lane_(std::make_unique<core::QueuedDbLane>("archive-write", config_.write_capacity)) {
}

QueuedArchiveDb::~QueuedArchiveDb() {
    Stop();
}

bool QueuedArchiveDb::Start(std::string* error_out) {
    if (inner_ == nullptr) {
        SetError(error_out, "archive db inner database is null");
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

void QueuedArchiveDb::Stop() {
    if (read_lane_) {
        read_lane_->Stop();
    }
    if (write_lane_) {
        write_lane_->Stop();
    }
}

bool QueuedArchiveDb::IsRunning() const {
    return read_lane_ != nullptr
        && write_lane_ != nullptr
        && read_lane_->IsRunning()
        && write_lane_->IsRunning();
}

core::QueuedDbTelemetrySnapshot QueuedArchiveDb::GetTelemetrySnapshot() const {
    return BuildTelemetrySnapshot(write_lane_.get(), read_lane_.get());
}

bool QueuedArchiveDb::CreateArchivePackage(
    const CreateArchivePackageCommand& command,
    std::int64_t* archive_package_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, archive_package_id_out, error_out]() {
            return inner_ != nullptr ? inner_->CreateArchivePackage(command, archive_package_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedArchiveDb::AddArchiveItem(
    const AddArchiveItemCommand& command,
    std::int64_t* archive_item_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, archive_item_id_out, error_out]() {
            return inner_ != nullptr ? inner_->AddArchiveItem(command, archive_item_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedArchiveDb::AddArchiveWorkflowPackageMember(
    const AddArchiveWorkflowPackageMemberCommand& command,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, error_out]() {
            return inner_ != nullptr ? inner_->AddArchiveWorkflowPackageMember(command, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedArchiveDb::RequestRehydrate(
    const RequestRehydrateCommand& command,
    std::int64_t* rehydrate_request_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, rehydrate_request_id_out, error_out]() {
            return inner_ != nullptr ? inner_->RequestRehydrate(command, rehydrate_request_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedArchiveDb::CompleteRehydrate(
    const CompleteRehydrateCommand& command,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, error_out]() {
            return inner_ != nullptr ? inner_->CompleteRehydrate(command, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedArchiveDb::FailRehydrate(
    const FailRehydrateCommand& command,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, error_out]() {
            return inner_ != nullptr ? inner_->FailRehydrate(command, error_out) : false;
        },
        false,
        error_out);
}

std::vector<events::EventEnvelope> QueuedArchiveDb::ReadUnpublishedOutboxBatch(
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

bool QueuedArchiveDb::MarkOutboxPublished(
    std::int64_t outbox_id,
    types::UtcTimePoint published_at_utc) {
    return ExecuteWrite<bool>(
        [this, outbox_id, published_at_utc]() {
            return inner_ != nullptr ? inner_->MarkOutboxPublished(outbox_id, published_at_utc) : false;
        },
        false);
}

bool QueuedArchiveDb::MarkOutboxPublishFailure(
    std::int64_t outbox_id,
    std::string_view last_error) {
    const auto last_error_copy = std::string(last_error);
    return ExecuteWrite<bool>(
        [this, outbox_id, last_error_copy]() {
            return inner_ != nullptr ? inner_->MarkOutboxPublishFailure(outbox_id, last_error_copy) : false;
        },
        false);
}

retention::OutboxRetentionPreview QueuedArchiveDb::PreviewOutboxRetention(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy) const {
    return ExecuteRead<retention::OutboxRetentionPreview>(
        [this, subscriptions, now_utc, policy]() {
            return inner_ != nullptr ? inner_->PreviewOutboxRetention(subscriptions, now_utc, policy) : retention::OutboxRetentionPreview{};
        },
        {});
}

bool QueuedArchiveDb::PurgeOutboxThroughRetentionFloor(
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

std::optional<ArchivePayloadRecord> QueuedArchiveDb::ResolveArchivePayload(
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    const auto payload_ref_kind_copy = std::string(payload_ref_kind);
    return ExecuteRead<std::optional<ArchivePayloadRecord>>(
        [this, event_version, payload_ref_kind_copy, payload_ref_id]() {
            return inner_ != nullptr
                ? inner_->ResolveArchivePayload(event_version, payload_ref_kind_copy, payload_ref_id)
                : std::nullopt;
        },
        std::nullopt);
}

std::optional<ArchivePayloadRecord> QueuedArchiveDb::ResolveArchivePayload(
    const events::EventEnvelope& envelope) const {
    return ExecuteRead<std::optional<ArchivePayloadRecord>>(
        [this, envelope]() {
            return inner_ != nullptr ? inner_->ResolveArchivePayload(envelope) : std::nullopt;
        },
        std::nullopt);
}

template <typename Result, typename Fn>
Result QueuedArchiveDb::ExecuteRead(
    Fn&& fn,
    Result fallback,
    std::string* error_out,
    const std::source_location& location) const {
    return core::QueuedDbExecutor::ExecuteQueued<Result>(
        *read_lane_,
        sqlite_call_mtx_,
        core::MakeQueuedDbOperationName("Archive", location),
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

template <typename Result, typename Fn>
Result QueuedArchiveDb::ExecuteWrite(
    Fn&& fn,
    Result fallback,
    std::string* error_out,
    const std::source_location& location) const {
    return core::QueuedDbExecutor::ExecuteQueued<Result>(
        *write_lane_,
        sqlite_call_mtx_,
        core::MakeQueuedDbOperationName("Archive", location),
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

} // namespace savor::db
