#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "EventEnvelope.h"

namespace simcore::db::events {

struct OutboxRelayDispatchKey {
    std::string_view event_type;
    int event_version = 1;
};

struct OutboxRelayDispatchBinding {
    OutboxRelayDispatchKey key;
    std::function<bool(const EventEnvelope&, std::string* error_out)> handler;
};

struct OutboxRelayConfig {
    sqlite3* db = nullptr;
    std::string outbox_table;
    std::string context_name;
    std::string aggregate_kind;
    std::string payload_ref_kind;
    int max_attempts = 5;
};

struct OutboxRelayResult {
    std::int64_t last_scanned_outbox_id = 0;
    std::string last_scanned_event_id;
    int scanned_count = 0;
    int published_count = 0;
    int failure_count = 0;
    int dead_lettered_count = 0;
};

class OutboxRelay {
public:
    explicit OutboxRelay(OutboxRelayConfig config);

    bool RelayBatch(
        std::int64_t after_outbox_id,
        int max_batch_size,
        const std::vector<OutboxRelayDispatchBinding>& bindings,
        OutboxRelayResult* result_out,
        std::string* error_out) const;
    bool RelayBatchFromCursor(
        std::int64_t cursor_outbox_id,
        int max_batch_size,
        const std::vector<OutboxRelayDispatchBinding>& bindings,
        OutboxRelayResult* result_out,
        std::string* error_out) const;

private:
    enum class RelayMode {
        ProducerOutbox,
        SubscriptionCursor
    };

    bool RelayBatchInternal(
        RelayMode mode,
        std::int64_t after_outbox_id,
        int max_batch_size,
        const std::vector<OutboxRelayDispatchBinding>& bindings,
        OutboxRelayResult* result_out,
        std::string* error_out) const;

    std::function<bool(const EventEnvelope&, std::string* error_out)> ResolveHandler(
        std::string_view event_type,
        int event_version,
        const std::vector<OutboxRelayDispatchBinding>& bindings) const;

    bool MarkPublished(std::int64_t outbox_id, std::string* error_out) const;
    bool MarkFailure(
        std::int64_t outbox_id,
        int current_attempt_count,
        std::string_view failure_message,
        bool* dead_lettered_out,
        std::string* error_out) const;

    OutboxRelayConfig config_{};
};

} // namespace simcore::db::events
