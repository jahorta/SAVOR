#pragma once

#include <string>

#include "../Types/UtcTimestamp.h"

namespace simcore::db::events {

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
    std::string event_id;
    std::string stream_id;
    std::string event_type;
    int event_version = 1;
    types::UtcTimePoint occurred_at_utc{};
    std::string payload_json;
};

} // namespace simcore::db::events
