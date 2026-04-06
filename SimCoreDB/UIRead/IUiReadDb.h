#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../Common/Types/UtcTimestamp.h"

namespace simcore::db {

struct UiProjectionCheckpoint {
    std::string projector_name;
    std::string last_event_id;
    std::int64_t last_outbox_id = 0;
    types::UtcTimePoint updated_at_utc{};
};

struct UiProjectionSubscription {
    std::string projector_name;
    std::string source_context;
    std::string source_outbox_table;
    std::int64_t last_outbox_id = 0;
    std::string last_event_id;
    types::UtcTimePoint updated_at_utc{};
    std::string status = "ACTIVE";
    std::string last_error;
};

struct UiProjectionSubscriptionBatchAudit {
    std::string projector_name;
    std::string source_context;
    std::string source_outbox_table;
    std::int64_t from_outbox_id = 0;
    std::int64_t to_outbox_id = 0;
    std::int64_t processed_count = 0;
    std::int64_t failed_count = 0;
    types::UtcTimePoint recorded_at_utc{};
};

struct IUiReadDb {
    virtual ~IUiReadDb() = default;

    // Gets a projector checkpoint row by projector name.
    virtual std::optional<UiProjectionCheckpoint> GetProjectionCheckpoint(
        const std::string& projector_name) const = 0;

    // Creates or updates a projector checkpoint.
    virtual bool UpsertProjectionCheckpoint(
        const UiProjectionCheckpoint& checkpoint) = 0;

    // Gets a subscription row by exact composite key.
    virtual std::optional<UiProjectionSubscription> GetProjectionSubscription(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table) const = 0;

    // Lists subscriptions for a source stream across all projectors.
    virtual std::vector<UiProjectionSubscription> ListProjectionSubscriptions(
        const std::string& source_context,
        const std::string& source_outbox_table) const = 0;

    // Returns MIN(last_outbox_id) for ACTIVE subscriptions on a source stream.
    virtual std::optional<std::int64_t> ComputeSafeFloorOutboxId(
        const std::string& source_context,
        const std::string& source_outbox_table) const = 0;

    // Creates a subscription row if one does not already exist and returns the row.
    virtual std::optional<UiProjectionSubscription> GetOrCreateProjectionSubscription(
        const UiProjectionSubscription& subscription) = 0;

    // Advances the subscription cursor and moves status to ACTIVE while clearing any error.
    virtual bool AdvanceProjectionSubscriptionCursor(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table,
        std::int64_t last_outbox_id,
        const std::string& last_event_id,
        types::UtcTimePoint updated_at_utc,
        const std::optional<UiProjectionSubscriptionBatchAudit>& batch_audit) = 0;

    // Sets a subscription into ERROR state with a reason.
    virtual bool SetProjectionSubscriptionError(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table,
        const std::string& last_error,
        types::UtcTimePoint updated_at_utc) = 0;

    // Pauses a subscription for operations.
    virtual bool PauseProjectionSubscription(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table,
        types::UtcTimePoint updated_at_utc,
        const std::string& reason) = 0;

    // Resumes a paused or errored subscription.
    virtual bool ResumeProjectionSubscription(
        const std::string& projector_name,
        const std::string& source_context,
        const std::string& source_outbox_table,
        types::UtcTimePoint updated_at_utc) = 0;
};

} // namespace simcore::db
