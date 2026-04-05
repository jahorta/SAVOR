#include "SqliteStateDb.h"

#include <chrono>

#include "../Common/Events/EventPayloadDispatch.h"
#include "../Common/Events/EventPayloadValidation.h"

namespace simcore::db::state {

namespace {

struct Statement {
    sqlite3_stmt* st = nullptr;

    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
};

std::optional<events::StateArtifactPayloadView> ResolveArtifactRef(sqlite3* db, std::int64_t artifact_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT artifact_id FROM state_artifact WHERE artifact_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, artifact_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::StateArtifactPayloadView view{};
    view.artifact_id = sqlite3_column_int64(st.st, 0);
    return view;
}

std::optional<events::StateArtifactPayloadView> ResolveSavestateRef(sqlite3* db, std::int64_t savestate_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT savestate_id, artifact_id FROM state_savestate WHERE savestate_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, savestate_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::StateArtifactPayloadView view{};
    view.savestate_id = sqlite3_column_int64(st.st, 0);
    view.artifact_id = sqlite3_column_int64(st.st, 1);
    return view;
}

std::optional<events::StateArtifactPayloadView> ResolveTasVariantRef(sqlite3* db, std::int64_t tas_variant_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT tas_variant_id, base_dtm_artifact_id, COALESCE(produced_savestate_id, 0) "
            "FROM state_tas_movie_variant WHERE tas_variant_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, tas_variant_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::StateArtifactPayloadView view{};
    view.tas_variant_id = sqlite3_column_int64(st.st, 0);
    view.artifact_id = sqlite3_column_int64(st.st, 1);
    view.savestate_id = sqlite3_column_int64(st.st, 2);
    return view;
}

} // namespace

SqliteStateDb::SqliteStateDb(sqlite3* db)
    : db_(db) {
}

std::vector<events::EventEnvelope> SqliteStateDb::ReadUnpublishedOutboxBatch(
    std::int64_t after_outbox_id,
    int max_batch_size) {
    std::vector<events::EventEnvelope> batch;
    if (db_ == nullptr || max_batch_size <= 0) {
        return batch;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,"
            "correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id "
            "FROM state_outbox_message "
            "WHERE outbox_id > ?1 "
            "AND published_at_utc IS NULL "
            "ORDER BY outbox_id ASC "
            "LIMIT ?2;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return batch;
    }

    sqlite3_bind_int64(st.st, 1, after_outbox_id);
    sqlite3_bind_int(st.st, 2, max_batch_size);

    batch.reserve(static_cast<std::size_t>(max_batch_size));
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        events::EventEnvelope envelope{};
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

        batch.push_back(std::move(envelope));
    }

    return batch;
}

bool SqliteStateDb::MarkOutboxPublished(
    std::int64_t outbox_id,
    types::UtcTimePoint published_at_utc) {
    if (db_ == nullptr || outbox_id <= 0) {
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE state_outbox_message "
            "SET published_at_utc=?2 "
            "WHERE outbox_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st.st, 1, outbox_id);
    sqlite3_bind_int64(st.st, 2, published_at_utc.time_since_epoch().count());

    if (sqlite3_step(st.st) != SQLITE_DONE) {
        return false;
    }

    return sqlite3_changes(db_) > 0;
}

bool SqliteStateDb::MarkOutboxPublishFailure(
    std::int64_t outbox_id,
    std::string_view last_error) {
    if (db_ == nullptr || outbox_id <= 0) {
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE state_outbox_message "
            "SET attempt_count=attempt_count+1, last_error=?2 "
            "WHERE outbox_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st.st, 1, outbox_id);
    sqlite3_bind_text(st.st, 2, last_error.data(), static_cast<int>(last_error.size()), SQLITE_TRANSIENT);

    if (sqlite3_step(st.st) != SQLITE_DONE) {
        return false;
    }

    return sqlite3_changes(db_) > 0;
}

std::optional<ArtifactPayloadRecord> SqliteStateDb::ResolveArtifactPayload(const events::EventEnvelope& envelope) const {
    if (!events::ValidateV1EnvelopeBasics(envelope)) {
        return std::nullopt;
    }

    return ResolveArtifactPayloadForEvent(
        envelope.event_type,
        envelope.event_version,
        envelope.payload_ref_kind,
        envelope.payload_ref_id);
}

std::optional<ArtifactPayloadRecord> SqliteStateDb::ResolveArtifactPayload(
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr || payload_ref_id <= 0) {
        return std::nullopt;
    }

    std::string_view dispatch_event_type;
    if (payload_ref_kind == "artifact") {
        dispatch_event_type = "State.ArtifactStored.v1";
    } else if (payload_ref_kind == "savestate") {
        dispatch_event_type = "State.SavestateCreated.v1";
    } else if (payload_ref_kind == "tas_variant" || payload_ref_kind == "tas-variant") {
        dispatch_event_type = "State.TasVariantCreated.v1";
    } else {
        return std::nullopt;
    }

    return ResolveArtifactPayloadForEvent(dispatch_event_type, event_version, payload_ref_kind, payload_ref_id);
}

std::optional<ArtifactPayloadRecord> SqliteStateDb::ResolveArtifactPayloadForEvent(
    std::string_view event_type,
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    const auto contract = events::ResolvePayloadResolverContract(event_type, event_version);
    if (!contract.has_value() || contract.value() != events::PayloadResolverContract::StateArtifactV1) {
        return std::nullopt;
    }

    if (payload_ref_kind == "artifact") {
        return ResolveArtifactRef(db_, payload_ref_id);
    }
    if (payload_ref_kind == "savestate") {
        return ResolveSavestateRef(db_, payload_ref_id);
    }
    if (payload_ref_kind == "tas_variant" || payload_ref_kind == "tas-variant") {
        return ResolveTasVariantRef(db_, payload_ref_id);
    }

    return std::nullopt;
}

} // namespace simcore::db::state
