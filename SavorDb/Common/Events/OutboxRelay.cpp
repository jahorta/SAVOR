#include "OutboxRelay.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "EventPayloadValidation.h"
#include "EventTypeFormat.h"

namespace savor::db::events {

namespace {

struct Statement {
    sqlite3_stmt* st = nullptr;
    Statement() = default;

    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
};

std::int64_t UtcNowMillis() {
    const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
    return now.time_since_epoch().count();
}

} // namespace

OutboxRelay::OutboxRelay(OutboxRelayConfig config)
    : config_(std::move(config)) {
}

std::function<bool(const EventEnvelope&, std::string* error_out)> OutboxRelay::ResolveHandler(
    std::string_view event_type,
    int event_version,
    const std::vector<OutboxRelayDispatchBinding>& bindings) const {
    if (!ValidateEventTypeFormat(event_type, event_version)) {
        return {};
    }

    for (const auto& binding : bindings) {
        if (!ValidateEventTypeFormat(binding.key.event_type, binding.key.event_version)) {
            continue;
        }
        if (binding.key.event_type == event_type && binding.key.event_version == event_version) {
            return binding.handler;
        }
    }

    return {};
}

bool OutboxRelay::MarkPublished(std::int64_t outbox_id, std::string* error_out) const {
    Statement st;
    const std::string sql =
        "UPDATE " + config_.outbox_table + " "
        "SET published_at_utc=?2 "
        "WHERE outbox_id=?1;";
    if (sqlite3_prepare_v2(config_.db, sql.c_str(), -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(config_.db);
        return false;
    }

    sqlite3_bind_int64(st.st, 1, outbox_id);
    sqlite3_bind_int64(st.st, 2, UtcNowMillis());
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(config_.db);
        return false;
    }

    return true;
}

bool OutboxRelay::MarkFailure(
    std::int64_t outbox_id,
    int current_attempt_count,
    std::string_view failure_message,
    bool* dead_lettered_out,
    std::string* error_out) const {
    const auto next_attempt_count = current_attempt_count + 1;
    const bool dead_lettered = next_attempt_count >= std::max(config_.max_attempts, 1);

    std::string last_error(failure_message);
    if (dead_lettered) {
        last_error = "dead-letter: " + last_error;
    }

    Statement st;
    const std::string sql =
        "UPDATE " + config_.outbox_table + " "
        "SET attempt_count=attempt_count+1, last_error=?2 "
        "WHERE outbox_id=?1;";
    if (sqlite3_prepare_v2(config_.db, sql.c_str(), -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(config_.db);
        return false;
    }

    sqlite3_bind_int64(st.st, 1, outbox_id);
    sqlite3_bind_text(st.st, 2, last_error.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(config_.db);
        return false;
    }

    if (dead_lettered_out != nullptr) {
        *dead_lettered_out = dead_lettered;
    }

    return true;
}

bool OutboxRelay::RelayBatch(
    std::int64_t after_outbox_id,
    int max_batch_size,
    const std::vector<OutboxRelayDispatchBinding>& bindings,
    OutboxRelayResult* result_out,
    std::string* error_out) const {
    return RelayBatchInternal(
        RelayMode::ProducerOutbox,
        after_outbox_id,
        max_batch_size,
        bindings,
        result_out,
        error_out);
}

bool OutboxRelay::RelayBatchFromCursor(
    std::int64_t cursor_outbox_id,
    int max_batch_size,
    const std::vector<OutboxRelayDispatchBinding>& bindings,
    OutboxRelayResult* result_out,
    std::string* error_out) const {
    return RelayBatchInternal(
        RelayMode::SubscriptionCursor,
        cursor_outbox_id,
        max_batch_size,
        bindings,
        result_out,
        error_out);
}

bool OutboxRelay::RelayBatchInternal(
    RelayMode mode,
    std::int64_t after_outbox_id,
    int max_batch_size,
    const std::vector<OutboxRelayDispatchBinding>& bindings,
    OutboxRelayResult* result_out,
    std::string* error_out) const {
    if (config_.db == nullptr) {
        if (error_out) *error_out = "OutboxRelay: db is required";
        return false;
    }
    if (config_.outbox_table.empty()) {
        if (error_out) *error_out = "OutboxRelay: outbox_table is required";
        return false;
    }
    if (max_batch_size <= 0) {
        if (error_out) *error_out = "OutboxRelay: max_batch_size must be > 0";
        return false;
    }

    OutboxRelayResult result{};
    result.last_scanned_outbox_id = after_outbox_id;

    Statement st;
    std::string sql =
        "SELECT outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,"
        "correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id,attempt_count "
        "FROM " + config_.outbox_table + " "
        "WHERE outbox_id > ?1 "
        "AND context_name=?2 ";

    int next_param_index = 3;
    if (mode == RelayMode::ProducerOutbox) {
        sql += "AND published_at_utc IS NULL ";
    }
    sql += "AND attempt_count < ?3 ";
    next_param_index = 4;
    if (!config_.aggregate_kind.empty()) {
        sql += "AND aggregate_kind=?" + std::to_string(next_param_index) + " ";
        next_param_index += 1;
    }
    if (!config_.payload_ref_kind.empty()) {
        sql += "AND payload_ref_kind=?" + std::to_string(next_param_index) + " ";
        next_param_index += 1;
    }

    const int limit_param_index = next_param_index;
    sql += "ORDER BY outbox_id ASC LIMIT ?" + std::to_string(limit_param_index) + ";";

    if (sqlite3_prepare_v2(config_.db, sql.c_str(), -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(config_.db);
        return false;
    }

    sqlite3_bind_int64(st.st, 1, after_outbox_id);
    sqlite3_bind_text(st.st, 2, config_.context_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st.st, 3, std::max(config_.max_attempts, 1));

    int next_bind_index = 4;
    if (!config_.aggregate_kind.empty()) {
        sqlite3_bind_text(st.st, next_bind_index, config_.aggregate_kind.c_str(), -1, SQLITE_TRANSIENT);
        next_bind_index += 1;
    }

    if (!config_.payload_ref_kind.empty()) {
        sqlite3_bind_text(st.st, next_bind_index, config_.payload_ref_kind.c_str(), -1, SQLITE_TRANSIENT);
        next_bind_index += 1;
    }

    sqlite3_bind_int(st.st, limit_param_index, max_batch_size);

    while (sqlite3_step(st.st) == SQLITE_ROW) {
        EventEnvelope envelope{};
        const auto outbox_id = sqlite3_column_int64(st.st, 0);
        const auto* event_id = sqlite3_column_text(st.st, 1);
        const auto* event_type = sqlite3_column_text(st.st, 2);
        const auto event_version = sqlite3_column_int(st.st, 3);
        const auto* context_name = sqlite3_column_text(st.st, 4);
        const auto* aggregate_kind = sqlite3_column_text(st.st, 5);
        const auto* aggregate_id = sqlite3_column_text(st.st, 6);
        const auto* correlation_id = sqlite3_column_text(st.st, 7);
        const auto* causation_id = sqlite3_column_text(st.st, 8);
        const auto occurred_at_utc = sqlite3_column_int64(st.st, 9);
        const auto* payload_ref_kind = sqlite3_column_text(st.st, 10);
        const auto payload_ref_id = sqlite3_column_int64(st.st, 11);
        const auto attempt_count = sqlite3_column_int(st.st, 12);

        envelope.outbox_id = outbox_id;
        envelope.event_id = event_id == nullptr ? "" : reinterpret_cast<const char*>(event_id);
        envelope.event_type = event_type == nullptr ? "" : reinterpret_cast<const char*>(event_type);
        envelope.event_version = event_version;
        envelope.context_name = context_name == nullptr ? "" : reinterpret_cast<const char*>(context_name);
        envelope.aggregate_kind = aggregate_kind == nullptr ? "" : reinterpret_cast<const char*>(aggregate_kind);
        envelope.aggregate_id = aggregate_id == nullptr ? "" : reinterpret_cast<const char*>(aggregate_id);
        envelope.correlation_id = correlation_id == nullptr ? "" : reinterpret_cast<const char*>(correlation_id);
        envelope.causation_id = causation_id == nullptr ? "" : reinterpret_cast<const char*>(causation_id);
        envelope.occurred_at_utc = types::UtcTimePoint(std::chrono::milliseconds(occurred_at_utc));
        envelope.payload_ref_kind = payload_ref_kind == nullptr ? "" : reinterpret_cast<const char*>(payload_ref_kind);
        envelope.payload_ref_id = payload_ref_id;

        result.last_scanned_outbox_id = outbox_id;
        result.last_scanned_event_id = envelope.event_id;
        result.scanned_count += 1;

        std::string handler_error;
        if (!ValidateEventPayloadRequiredFieldsV1(envelope, &handler_error)) {
            bool dead_lettered = false;
            if (!MarkFailure(outbox_id, attempt_count, handler_error, &dead_lettered, error_out)) {
                return false;
            }

            if (dead_lettered) {
                result.dead_lettered_count += 1;
            } else {
                result.failure_count += 1;
            }
            continue;
        }

        auto handler = ResolveHandler(envelope.event_type, envelope.event_version, bindings);
        const bool handled = handler && handler(envelope, &handler_error);
        if (handled) {
            if (mode == RelayMode::ProducerOutbox) {
                if (!MarkPublished(outbox_id, error_out)) {
                    return false;
                }
            }
            result.published_count += 1;
            continue;
        }

        if (!handler) {
            handler_error = "no projector handler for (event_type,event_version)";
        } else if (handler_error.empty()) {
            handler_error = "projector handler returned false";
        }

        bool dead_lettered = false;
        if (!MarkFailure(outbox_id, attempt_count, handler_error, &dead_lettered, error_out)) {
            return false;
        }
        if (dead_lettered) {
            result.dead_lettered_count += 1;
        } else {
            result.failure_count += 1;
        }
    }

    if (result_out != nullptr) {
        *result_out = result;
    }

    return true;
}

} // namespace savor::db::events
