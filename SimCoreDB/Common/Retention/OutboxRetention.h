#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../Types/UtcTimestamp.h"

namespace simcore::db::retention {

struct OutboxRetentionPolicy {
    std::chrono::milliseconds paused_or_error_block_threshold{ std::chrono::minutes(30) };
    bool block_when_no_active_subscriptions = true;
};

struct OutboxSubscriptionSnapshot {
    std::string projector_name;
    std::int64_t last_outbox_id = 0;
    types::UtcTimePoint updated_at_utc{};
    std::string status = "ACTIVE";
    std::string last_error;
    bool required = true;
};

struct OutboxSubscriptionLag {
    OutboxSubscriptionSnapshot subscription;
    std::int64_t lag_outbox_rows = 0;
};

struct OutboxRetentionPreview {
    std::vector<OutboxSubscriptionSnapshot> active_subscriptions;
    std::vector<OutboxSubscriptionLag> lag_per_subscription;
    std::optional<std::int64_t> safe_purge_floor_outbox_id;
    std::vector<OutboxSubscriptionSnapshot> blocking_required_subscriptions;
    std::int64_t source_max_outbox_id = 0;

    [[nodiscard]] bool IsPurgeBlocked() const {
        return !blocking_required_subscriptions.empty();
    }
};

inline OutboxRetentionPreview BuildOutboxRetentionPreview(
    std::int64_t source_max_outbox_id,
    const std::vector<OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const OutboxRetentionPolicy& policy) {
    OutboxRetentionPreview preview{};
    preview.source_max_outbox_id = source_max_outbox_id;

    for (const auto& sub : subscriptions) {
        OutboxSubscriptionLag lag{};
        lag.subscription = sub;
        lag.lag_outbox_rows = (std::max<std::int64_t>)(0, source_max_outbox_id - sub.last_outbox_id);
        preview.lag_per_subscription.push_back(std::move(lag));

        if (sub.status == "ACTIVE") {
            preview.active_subscriptions.push_back(sub);
            if (!preview.safe_purge_floor_outbox_id.has_value()) {
                preview.safe_purge_floor_outbox_id = sub.last_outbox_id;
            }
            else {
                preview.safe_purge_floor_outbox_id = (std::min)(
                    preview.safe_purge_floor_outbox_id.value(),
                    sub.last_outbox_id);
            }
            continue;
        }

        if (!sub.required || (sub.status != "PAUSED" && sub.status != "ERROR")) {
            continue;
        }

        const auto stalled_for = now_utc - sub.updated_at_utc;
        if (stalled_for >= policy.paused_or_error_block_threshold) {
            preview.blocking_required_subscriptions.push_back(sub);
        }
    }

    if (policy.block_when_no_active_subscriptions
        && !preview.safe_purge_floor_outbox_id.has_value()) {
        OutboxSubscriptionSnapshot synthetic_block{};
        synthetic_block.projector_name = "<no-active-subscriptions>";
        synthetic_block.status = "PAUSED";
        synthetic_block.required = true;
        preview.blocking_required_subscriptions.push_back(std::move(synthetic_block));
    }

    return preview;
}

} // namespace simcore::db::retention
