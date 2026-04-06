#include "ProjectorContract.h"

#include <string_view>

#include "../SqliteUiReadDb.h"

namespace simcore::db::uiread::projectors {
namespace {

bool HasProcessedEvent(
    sqlite3* db,
    const std::string& checkpoint_name,
    const std::string& event_id,
    bool* processed_out,
    std::string* error_out) {
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT 1 FROM ui_projection_handled_event "
        "WHERE projector_name=?1 AND event_id=?2 LIMIT 1;";

    if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }

    sqlite3_bind_text(st, 1, checkpoint_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, event_id.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }

    *processed_out = (rc == SQLITE_ROW);
    return true;
}

bool MarkEventProcessed(
    sqlite3* db,
    const std::string& checkpoint_name,
    const events::EventEnvelope& envelope,
    std::string* error_out) {
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "INSERT INTO ui_projection_handled_event(projector_name,event_id,last_outbox_id,handled_at_utc) "
        "VALUES(?1,?2,?3,?4) "
        "ON CONFLICT(projector_name,event_id) DO UPDATE SET "
        "last_outbox_id=excluded.last_outbox_id,handled_at_utc=excluded.handled_at_utc;";

    if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }

    sqlite3_bind_text(st, 1, checkpoint_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, envelope.event_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, envelope.outbox_id);
    sqlite3_bind_int64(st, 4, simcore::db::types::UtcNow().time_since_epoch().count());

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }

    return true;
}

bool UpsertCheckpoint(
    sqlite3* db,
    const std::string& checkpoint_name,
    std::int64_t last_outbox_id,
    std::string_view last_event_id,
    std::string* error_out) {
    simcore::db::SqliteUiReadDb ui_read_db(db);
    if (!ui_read_db.UpsertProjectionCheckpoint({
            checkpoint_name,
            std::string(last_event_id),
            last_outbox_id,
            simcore::db::types::UtcNow(),
        })) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return false;
    }

    return true;
}

} // namespace

bool ValidateProjectorContractInputs(
    const std::string& projector_name,
    int max_batch_size,
    int max_attempts,
    std::string* error_out) {
    if (projector_name.empty()) {
        if (error_out) *error_out = "projector_name is required";
        return false;
    }
    if (max_batch_size <= 0) {
        if (error_out) *error_out = "max_batch_size must be > 0";
        return false;
    }
    if (max_attempts <= 0) {
        if (error_out) *error_out = "max_attempts must be > 0";
        return false;
    }

    return true;
}

std::int64_t GetProjectorCheckpoint(
    sqlite3* db,
    const std::string& projector_name,
    std::string* error_out) {
    if (projector_name.empty()) {
        if (error_out) *error_out = "projector_name is required";
        return 0;
    }

    simcore::db::SqliteUiReadDb ui_read_db(db);
    const auto checkpoint = ui_read_db.GetProjectionCheckpoint(projector_name);
    return checkpoint.has_value() ? checkpoint->last_outbox_id : 0;
}

bool RunProjectorRelay(
    sqlite3* db,
    const std::string& checkpoint_name,
    const events::OutboxRelayConfig& relay_config,
    const std::vector<events::OutboxRelayDispatchBinding>& bindings,
    int max_batch_size,
    std::string* error_out) {
    const auto checkpoint = GetProjectorCheckpoint(db, checkpoint_name, error_out);

    std::vector<events::OutboxRelayDispatchBinding> wrapped_bindings;
    wrapped_bindings.reserve(bindings.size());

    for (const auto& binding : bindings) {
        wrapped_bindings.push_back(events::OutboxRelayDispatchBinding{
            .key = binding.key,
            .handler = [db, checkpoint_name, handler = binding.handler](const events::EventEnvelope& envelope, std::string* handler_error) {
                if (envelope.event_id.empty()) {
                    if (handler_error) *handler_error = "event_id is required for projector idempotency";
                    return false;
                }

                bool processed = false;
                if (!HasProcessedEvent(db, checkpoint_name, envelope.event_id, &processed, handler_error)) {
                    return false;
                }
                if (processed) {
                    return true;
                }

                if (!handler(envelope, handler_error)) {
                    return false;
                }

                return MarkEventProcessed(db, checkpoint_name, envelope, handler_error);
            },
        });
    }

    events::OutboxRelay relay(relay_config);
    events::OutboxRelayResult relay_result{};
    if (!relay.RelayBatch(checkpoint, max_batch_size, wrapped_bindings, &relay_result, error_out)) {
        return false;
    }

    if (relay_result.last_scanned_outbox_id <= checkpoint) {
        return true;
    }

    return UpsertCheckpoint(
        db,
        checkpoint_name,
        relay_result.last_scanned_outbox_id,
        relay_result.last_scanned_event_id,
        error_out);
}

} // namespace simcore::db::uiread::projectors
