#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../Common/QueuedDb.h"
#include "IArchiveDb.h"

namespace savor::db {

class QueuedArchiveDb final : public IArchiveDb, private core::QueuedDbExecutor {
public:
    explicit QueuedArchiveDb(
        IArchiveDb* inner,
        core::QueuedDbConfig config = {});
    ~QueuedArchiveDb() override;

    QueuedArchiveDb(const QueuedArchiveDb&) = delete;
    QueuedArchiveDb& operator=(const QueuedArchiveDb&) = delete;

    bool Start(std::string* error_out = nullptr);
    void Stop();
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] core::QueuedDbTelemetrySnapshot GetTelemetrySnapshot() const;

    bool CreateArchivePackage(
        const CreateArchivePackageCommand& command,
        std::int64_t* archive_package_id_out = nullptr,
        std::string* error_out = nullptr) override;
    bool AddArchiveItem(
        const AddArchiveItemCommand& command,
        std::int64_t* archive_item_id_out = nullptr,
        std::string* error_out = nullptr) override;
    bool RequestRehydrate(
        const RequestRehydrateCommand& command,
        std::int64_t* rehydrate_request_id_out = nullptr,
        std::string* error_out = nullptr) override;
    bool CompleteRehydrate(
        const CompleteRehydrateCommand& command,
        std::string* error_out = nullptr) override;
    bool FailRehydrate(
        const FailRehydrateCommand& command,
        std::string* error_out = nullptr) override;
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
    std::optional<ArchivePayloadRecord> ResolveArchivePayload(
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;
    std::optional<ArchivePayloadRecord> ResolveArchivePayload(
        const events::EventEnvelope& envelope) const override;

private:
    template <typename Result, typename Fn>
    Result ExecuteRead(Fn&& fn, Result fallback, std::string* error_out = nullptr) const;

    template <typename Result, typename Fn>
    Result ExecuteWrite(Fn&& fn, Result fallback, std::string* error_out = nullptr) const;

    IArchiveDb* inner_ = nullptr;
    core::QueuedDbConfig config_{};
    mutable std::mutex sqlite_call_mtx_;
    std::unique_ptr<core::QueuedDbLane> read_lane_;
    std::unique_ptr<core::QueuedDbLane> write_lane_;
};

} // namespace savor::db
