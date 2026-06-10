#pragma once

#include <cstdint>
#include <string>

#include "../Types/UtcTimestamp.h"

namespace savor::db::events {

enum class EventDeliveryStatus {
    Pending = 0,
    Delivered = 1,
    Failed = 2,
};

enum class JobOutcomeStatus {
    Unknown = 0,
    Succeeded = 1,
    Failed = 2,
    Cancelled = 3,
};

struct EventEnvelope {
    std::int64_t outbox_id = 0;
    std::string event_id;
    std::string event_type;
    int event_version = 1;
    std::string context_name;
    std::string aggregate_kind;
    std::string aggregate_id;
    std::string correlation_id;
    std::string causation_id;
    types::UtcTimePoint occurred_at_utc{};
    std::string payload_ref_kind;
    std::int64_t payload_ref_id = 0;
    // Optional debugging mirror; canonical payload is addressed by (payload_ref_kind, payload_ref_id).
    std::string payload_json;
};

} // namespace savor::db::events
