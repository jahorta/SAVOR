#pragma once

#include <sqlite3.h>

#include "IUiReadDb.h"

namespace simcore::db {

class SqliteUiReadDb final : public IUiReadDb {
public:
    explicit SqliteUiReadDb(sqlite3* db);

    std::optional<UiProjectionSubscription> GetProjectionSubscription(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table) const override;
    std::vector<UiProjectionSubscription> ListProjectionSubscriptions(
        const std::string& source_context,
        const std::string& source_outbox_table) const override;
    std::optional<std::int64_t> ComputeSafeFloorOutboxId(
        const std::string& source_context,
        const std::string& source_outbox_table) const override;
    std::optional<UiProjectionSubscription> GetOrCreateProjectionSubscription(
        const UiProjectionSubscription& subscription) override;
    bool AdvanceProjectionSubscriptionCursor(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table,
        std::int64_t last_outbox_id,
        const std::string& last_event_id,
        types::UtcTimePoint updated_at_utc,
        const std::optional<UiProjectionSubscriptionBatchAudit>& batch_audit) override;
    bool SetProjectionSubscriptionError(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table,
        const std::string& last_error,
        types::UtcTimePoint updated_at_utc) override;
    bool PauseProjectionSubscription(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table,
        types::UtcTimePoint updated_at_utc,
        const std::string& reason) override;
    bool ResumeProjectionSubscription(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table,
        types::UtcTimePoint updated_at_utc) override;

private:
    sqlite3* db_ = nullptr;
};

} // namespace simcore::db
