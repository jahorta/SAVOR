#include "SqliteAuthoringDb.h"

#include <chrono>

#include "../Common/Events/EventPayloadDispatch.h"
#include "../Common/Events/EventPayloadValidation.h"

namespace simcore::db {

namespace {

std::int64_t ToEpochMillis(types::UtcTimePoint value) {
    return value.time_since_epoch().count();
}

types::UtcTimePoint FromEpochMillis(std::int64_t value) {
    return types::UtcTimePoint{ std::chrono::milliseconds(value) };
}

bool TryReadTemplatePayload(sqlite3* db, std::int64_t template_id, AuthoringPayloadRecord* out) {
    if (db == nullptr || out == nullptr || template_id <= 0) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT template_id, COALESCE(seed_probe_spec_id, 0), COALESCE(battle_run_spec_id, 0) "
        "FROM au_template "
        "WHERE template_id=?1;";
    if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st, 1, template_id);

    const auto rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        out->template_id = sqlite3_column_int64(st, 0);
        out->seed_probe_spec_id = sqlite3_column_int64(st, 1);
        out->battle_run_spec_id = sqlite3_column_int64(st, 2);
        sqlite3_finalize(st);
        return true;
    }

    sqlite3_finalize(st);
    return false;
}

bool TryReadTemplateBySeedProbeSpec(sqlite3* db, std::int64_t seed_probe_spec_id, AuthoringPayloadRecord* out) {
    if (db == nullptr || out == nullptr || seed_probe_spec_id <= 0) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT template_id, COALESCE(seed_probe_spec_id, 0), COALESCE(battle_run_spec_id, 0) "
        "FROM au_template "
        "WHERE seed_probe_spec_id=?1 "
        "ORDER BY template_id DESC "
        "LIMIT 1;";
    if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st, 1, seed_probe_spec_id);

    const auto rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        out->template_id = sqlite3_column_int64(st, 0);
        out->seed_probe_spec_id = sqlite3_column_int64(st, 1);
        out->battle_run_spec_id = sqlite3_column_int64(st, 2);
        sqlite3_finalize(st);
        return true;
    }

    sqlite3_finalize(st);

    st = nullptr;
    constexpr const char* kExistsSql =
        "SELECT seed_probe_spec_id "
        "FROM au_seed_probe_spec "
        "WHERE seed_probe_spec_id=?1;";
    if (sqlite3_prepare_v2(db, kExistsSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st, 1, seed_probe_spec_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        out->template_id = 0;
        out->seed_probe_spec_id = seed_probe_spec_id;
        out->battle_run_spec_id = 0;
        sqlite3_finalize(st);
        return true;
    }

    sqlite3_finalize(st);
    return false;
}

bool TryReadTemplateByBattleRunSpec(sqlite3* db, std::int64_t battle_run_spec_id, AuthoringPayloadRecord* out) {
    if (db == nullptr || out == nullptr || battle_run_spec_id <= 0) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT template_id, COALESCE(seed_probe_spec_id, 0), COALESCE(battle_run_spec_id, 0) "
        "FROM au_template "
        "WHERE battle_run_spec_id=?1 "
        "ORDER BY template_id DESC "
        "LIMIT 1;";
    if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st, 1, battle_run_spec_id);

    const auto rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        out->template_id = sqlite3_column_int64(st, 0);
        out->seed_probe_spec_id = sqlite3_column_int64(st, 1);
        out->battle_run_spec_id = sqlite3_column_int64(st, 2);
        sqlite3_finalize(st);
        return true;
    }

    sqlite3_finalize(st);

    st = nullptr;
    constexpr const char* kExistsSql =
        "SELECT battle_run_spec_id "
        "FROM au_battle_run_spec "
        "WHERE battle_run_spec_id=?1;";
    if (sqlite3_prepare_v2(db, kExistsSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st, 1, battle_run_spec_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        out->template_id = 0;
        out->seed_probe_spec_id = 0;
        out->battle_run_spec_id = battle_run_spec_id;
        sqlite3_finalize(st);
        return true;
    }

    sqlite3_finalize(st);
    return false;
}

} // namespace

SqliteAuthoringDb::SqliteAuthoringDb(sqlite3* db)
    : db_(db) {
}

std::vector<events::EventEnvelope> SqliteAuthoringDb::ReadUnpublishedOutboxBatch(
    std::int64_t after_outbox_id,
    int max_batch_size) {
    if (db_ == nullptr || max_batch_size <= 0) {
        return {};
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT outbox_id, event_id, event_type, event_version, context_name, aggregate_kind, aggregate_id, "
        "COALESCE(correlation_id, ''), COALESCE(causation_id, ''), occurred_at_utc, payload_ref_kind, payload_ref_id "
        "FROM au_event_outbox "
        "WHERE outbox_id>?1 AND published_at_utc IS NULL "
        "ORDER BY outbox_id ASC "
        "LIMIT ?2;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return {};
    }

    sqlite3_bind_int64(st, 1, after_outbox_id);
    sqlite3_bind_int(st, 2, max_batch_size);

    std::vector<events::EventEnvelope> batch;
    batch.reserve(static_cast<std::size_t>(max_batch_size));

    while (sqlite3_step(st) == SQLITE_ROW) {
        events::EventEnvelope envelope;

        const auto* event_id = sqlite3_column_text(st, 1);
        const auto* event_type = sqlite3_column_text(st, 2);
        const auto* context_name = sqlite3_column_text(st, 4);
        const auto* aggregate_kind = sqlite3_column_text(st, 5);
        const auto* aggregate_id = sqlite3_column_text(st, 6);
        const auto* correlation_id = sqlite3_column_text(st, 7);
        const auto* causation_id = sqlite3_column_text(st, 8);
        const auto* payload_ref_kind = sqlite3_column_text(st, 10);

        envelope.event_id = event_id == nullptr ? std::string{} : reinterpret_cast<const char*>(event_id);
        envelope.event_type = event_type == nullptr ? std::string{} : reinterpret_cast<const char*>(event_type);
        envelope.event_version = sqlite3_column_int(st, 3);
        envelope.context_name = context_name == nullptr ? std::string{} : reinterpret_cast<const char*>(context_name);
        envelope.aggregate_kind = aggregate_kind == nullptr ? std::string{} : reinterpret_cast<const char*>(aggregate_kind);
        envelope.aggregate_id = aggregate_id == nullptr ? std::string{} : reinterpret_cast<const char*>(aggregate_id);
        envelope.correlation_id = correlation_id == nullptr ? std::string{} : reinterpret_cast<const char*>(correlation_id);
        envelope.causation_id = causation_id == nullptr ? std::string{} : reinterpret_cast<const char*>(causation_id);
        envelope.occurred_at_utc = FromEpochMillis(sqlite3_column_int64(st, 9));
        envelope.payload_ref_kind = payload_ref_kind == nullptr ? std::string{} : reinterpret_cast<const char*>(payload_ref_kind);
        envelope.payload_ref_id = sqlite3_column_int64(st, 11);

        batch.push_back(std::move(envelope));
    }

    sqlite3_finalize(st);
    return batch;
}

bool SqliteAuthoringDb::MarkOutboxPublished(
    std::int64_t outbox_id,
    types::UtcTimePoint published_at_utc) {
    if (db_ == nullptr || outbox_id <= 0) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "UPDATE au_event_outbox "
        "SET published_at_utc=?2, delivery_status=?3 "
        "WHERE outbox_id=?1;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st, 1, outbox_id);
    sqlite3_bind_int64(st, 2, ToEpochMillis(published_at_utc));
    sqlite3_bind_int(st, 3, static_cast<int>(events::EventDeliveryStatus::Delivered));

    const auto rc = sqlite3_step(st);
    sqlite3_finalize(st);

    return rc == SQLITE_DONE && sqlite3_changes(db_) > 0;
}

bool SqliteAuthoringDb::MarkOutboxPublishFailure(
    std::int64_t outbox_id,
    std::string_view last_error) {
    if (db_ == nullptr || outbox_id <= 0) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "UPDATE au_event_outbox "
        "SET attempt_count=attempt_count+1, last_error=?2, delivery_status=?3 "
        "WHERE outbox_id=?1;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st, 1, outbox_id);
    sqlite3_bind_text(st, 2, last_error.data(), static_cast<int>(last_error.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, static_cast<int>(events::EventDeliveryStatus::Failed));

    const auto rc = sqlite3_step(st);
    sqlite3_finalize(st);

    return rc == SQLITE_DONE && sqlite3_changes(db_) > 0;
}

std::optional<AuthoringPayloadRecord> SqliteAuthoringDb::ResolveAuthoringPayload(
    const events::EventEnvelope& envelope) const {
    if (!events::ValidateV1PayloadRef(envelope, "Authoring", "authoring_event")) {
        return std::nullopt;
    }

    return ResolveAuthoringPayload(
        envelope.event_type,
        envelope.event_version,
        envelope.payload_ref_kind,
        envelope.payload_ref_id);
}

std::optional<AuthoringPayloadRecord> SqliteAuthoringDb::ResolveAuthoringPayload(
    std::string_view event_type,
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr) {
        return std::nullopt;
    }

    const auto contract = events::ResolvePayloadResolverContract(event_type, event_version);
    if (!contract.has_value() || contract.value() != events::PayloadResolverContract::AuthoringV1) {
        return std::nullopt;
    }

    if (payload_ref_kind.empty() || payload_ref_id <= 0) {
        return std::nullopt;
    }

    std::string_view effective_ref_kind = payload_ref_kind;
    if (payload_ref_kind == "authoring_event") {
        if (event_type == "Authoring.TemplateSaved.v1") {
            effective_ref_kind = "template";
        }
        else if (event_type == "Authoring.SeedProbeSpecSaved.v1") {
            effective_ref_kind = "seed_probe_spec";
        }
        else if (event_type == "Authoring.BattleRunSpecSaved.v1") {
            effective_ref_kind = "battle_run_spec";
        }
        else {
            effective_ref_kind = "template";
        }
    }

    AuthoringPayloadRecord payload{};
    if (effective_ref_kind == "template") {
        if (!TryReadTemplatePayload(db_, payload_ref_id, &payload)) {
            return std::nullopt;
        }
        return payload;
    }

    if (effective_ref_kind == "seed_probe_spec") {
        if (!TryReadTemplateBySeedProbeSpec(db_, payload_ref_id, &payload)) {
            return std::nullopt;
        }
        return payload;
    }

    if (effective_ref_kind == "battle_run_spec") {
        if (!TryReadTemplateByBattleRunSpec(db_, payload_ref_id, &payload)) {
            return std::nullopt;
        }
        return payload;
    }

    return std::nullopt;
}

} // namespace simcore::db
