#include "SqliteStateDb.h"

#include <chrono>
#include <string>

#include "../Common/Events/EventPayloadDispatch.h"
#include "../Common/Events/EventPayloadValidation.h"

namespace simcore::db::state {

namespace {

struct Statement {
    Statement() {}
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* st = nullptr;
};

bool StepDone(sqlite3* db, sqlite3_stmt* st, std::string* error_out) {
    if (sqlite3_step(st) == SQLITE_DONE) {
        return true;
    }
    if (error_out) {
        *error_out = sqlite3_errmsg(db);
    }
    return false;
}

bool BeginImmediate(sqlite3* db, std::string* error_out) {
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK) {
        return true;
    }
    if (error_out) {
        *error_out = sqlite3_errmsg(db);
    }
    return false;
}

void Rollback(sqlite3* db) {
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
}

bool Commit(sqlite3* db, std::string* error_out) {
    if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK) {
        return true;
    }
    if (error_out) {
        *error_out = sqlite3_errmsg(db);
    }
    return false;
}

bool InsertStateOutboxEvent(
    sqlite3* db,
    std::string_view event_id,
    std::string_view event_type,
    std::string_view aggregate_kind,
    std::string_view aggregate_id,
    std::string_view correlation_id,
    std::string_view causation_id,
    std::int64_t occurred_at_utc,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id,
    std::string* error_out) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO state_outbox_message("
            "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
            "VALUES(?1,?2,1,'State',?3,?4,?5,?6,?7,?8,?9);",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) {
            *error_out = sqlite3_errmsg(db);
        }
        return false;
    }

    sqlite3_bind_text(st.st, 1, event_id.data(), static_cast<int>(event_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 2, event_type.data(), static_cast<int>(event_type.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 3, aggregate_kind.data(), static_cast<int>(aggregate_kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 4, aggregate_id.data(), static_cast<int>(aggregate_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 5, correlation_id.data(), static_cast<int>(correlation_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 6, causation_id.data(), static_cast<int>(causation_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 7, occurred_at_utc);
    sqlite3_bind_text(st.st, 8, payload_ref_kind.data(), static_cast<int>(payload_ref_kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 9, payload_ref_id);
    return StepDone(db, st.st, error_out);
}

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

std::optional<events::StateArtifactPayloadView> ResolveSavestateDerivationRef(
    sqlite3* db,
    std::int64_t derivation_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT d.to_savestate_id, s.artifact_id "
            "FROM state_savestate_derivation d "
            "JOIN state_savestate s ON s.savestate_id = d.to_savestate_id "
            "WHERE d.derivation_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, derivation_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::StateArtifactPayloadView view{};
    view.savestate_id = sqlite3_column_int64(st.st, 0);
    view.artifact_id = sqlite3_column_int64(st.st, 1);
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

bool SqliteStateDb::StoreArtifact(
    const StoreArtifactCommand& command,
    std::int64_t* artifact_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.sha256.empty()
        || command.filename.empty()
        || command.file_ext.empty()
        || command.artifact_kind.empty()
        || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (!BeginImmediate(db_, error_out)) {
        return false;
    }

    Statement insert_artifact;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO state_artifact(sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7);",
            -1,
            &insert_artifact.st,
            nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(insert_artifact.st, 1, command.sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_artifact.st, 2, command.size_bytes);
    sqlite3_bind_int(insert_artifact.st, 3, command.compression_kind);
    sqlite3_bind_text(insert_artifact.st, 4, command.filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_artifact.st, 5, command.file_ext.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_artifact.st, 6, command.artifact_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_artifact.st, 7, command.created_at_utc.time_since_epoch().count());

    if (!StepDone(db_, insert_artifact.st, error_out)) {
        Rollback(db_);
        return false;
    }

    const auto artifact_id = sqlite3_last_insert_rowid(db_);
    const auto aggregate_id = std::to_string(artifact_id);
    if (!InsertStateOutboxEvent(
            db_,
            command.event_id,
            "State.ArtifactStored.v1",
            "artifact",
            aggregate_id,
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "artifact",
            artifact_id,
            error_out)) {
        Rollback(db_);
        return false;
    }

    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }

    if (artifact_id_out) {
        *artifact_id_out = artifact_id;
    }
    return true;
}

bool SqliteStateDb::CreateSavestate(
    const CreateSavestateCommand& command,
    std::int64_t* savestate_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.artifact_id <= 0 || command.savestate_type.empty() || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (!BeginImmediate(db_, error_out)) {
        return false;
    }

    Statement insert_savestate;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO state_savestate(artifact_id,savestate_type,note,is_complete,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5);",
            -1,
            &insert_savestate.st,
            nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_savestate.st, 1, command.artifact_id);
    sqlite3_bind_text(insert_savestate.st, 2, command.savestate_type.c_str(), -1, SQLITE_TRANSIENT);
    if (command.note.empty()) {
        sqlite3_bind_null(insert_savestate.st, 3);
    } else {
        sqlite3_bind_text(insert_savestate.st, 3, command.note.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(insert_savestate.st, 4, command.is_complete ? 1 : 0);
    sqlite3_bind_int64(insert_savestate.st, 5, command.created_at_utc.time_since_epoch().count());

    if (!StepDone(db_, insert_savestate.st, error_out)) {
        Rollback(db_);
        return false;
    }

    const auto savestate_id = sqlite3_last_insert_rowid(db_);
    const auto aggregate_id = std::to_string(savestate_id);
    if (!InsertStateOutboxEvent(
            db_,
            command.event_id,
            "State.SavestateCreated.v1",
            "savestate",
            aggregate_id,
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "savestate",
            savestate_id,
            error_out)) {
        Rollback(db_);
        return false;
    }

    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }

    if (savestate_id_out) {
        *savestate_id_out = savestate_id;
    }
    return true;
}

bool SqliteStateDb::DeriveSavestate(
    const DeriveSavestateCommand& command,
    std::int64_t* derivation_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.from_savestate_id <= 0
        || command.to_savestate_id <= 0
        || command.method_kind.empty()
        || command.source_context_kind.empty()
        || command.source_context_id <= 0
        || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (!BeginImmediate(db_, error_out)) {
        return false;
    }

    Statement insert_derivation;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO state_savestate_derivation(from_savestate_id,to_savestate_id,method_kind,source_context_kind,source_context_id,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6);",
            -1,
            &insert_derivation.st,
            nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_derivation.st, 1, command.from_savestate_id);
    sqlite3_bind_int64(insert_derivation.st, 2, command.to_savestate_id);
    sqlite3_bind_text(insert_derivation.st, 3, command.method_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_derivation.st, 4, command.source_context_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_derivation.st, 5, command.source_context_id);
    sqlite3_bind_int64(insert_derivation.st, 6, command.created_at_utc.time_since_epoch().count());

    if (!StepDone(db_, insert_derivation.st, error_out)) {
        Rollback(db_);
        return false;
    }

    const auto derivation_id = sqlite3_last_insert_rowid(db_);
    const auto aggregate_id = std::to_string(command.to_savestate_id);
    if (!InsertStateOutboxEvent(
            db_,
            command.event_id,
            "State.SavestateDerived.v1",
            "savestate_derivation",
            aggregate_id,
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "savestate_derivation",
            derivation_id,
            error_out)) {
        Rollback(db_);
        return false;
    }

    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }

    if (derivation_id_out) {
        *derivation_id_out = derivation_id;
    }
    return true;
}

bool SqliteStateDb::CreateTasVariant(
    const CreateTasVariantCommand& command,
    std::int64_t* tas_variant_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.name.empty()
        || command.base_dtm_artifact_id <= 0
        || command.mutation_mode.empty()
        || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (!BeginImmediate(db_, error_out)) {
        return false;
    }

    Statement insert_variant;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO state_tas_movie_variant("
            "name,base_dtm_artifact_id,dtmini_artifact_id,mutation_mode,rtc_value,bookmark_name,insert_frame_count,parent_tas_variant_id,produced_savestate_id,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);",
            -1,
            &insert_variant.st,
            nullptr)
        != SQLITE_OK) {
        Rollback(db_);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(insert_variant.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_variant.st, 2, command.base_dtm_artifact_id);
    if (command.dtmini_artifact_id.has_value()) sqlite3_bind_int64(insert_variant.st, 3, command.dtmini_artifact_id.value());
    else sqlite3_bind_null(insert_variant.st, 3);
    sqlite3_bind_text(insert_variant.st, 4, command.mutation_mode.c_str(), -1, SQLITE_TRANSIENT);
    if (command.rtc_value.has_value()) sqlite3_bind_int64(insert_variant.st, 5, command.rtc_value.value());
    else sqlite3_bind_null(insert_variant.st, 5);
    if (command.bookmark_name.has_value()) sqlite3_bind_text(insert_variant.st, 6, command.bookmark_name->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_variant.st, 6);
    if (command.insert_frame_count.has_value()) sqlite3_bind_int64(insert_variant.st, 7, command.insert_frame_count.value());
    else sqlite3_bind_null(insert_variant.st, 7);
    if (command.parent_tas_variant_id.has_value()) sqlite3_bind_int64(insert_variant.st, 8, command.parent_tas_variant_id.value());
    else sqlite3_bind_null(insert_variant.st, 8);
    if (command.produced_savestate_id.has_value()) sqlite3_bind_int64(insert_variant.st, 9, command.produced_savestate_id.value());
    else sqlite3_bind_null(insert_variant.st, 9);
    sqlite3_bind_int64(insert_variant.st, 10, command.created_at_utc.time_since_epoch().count());

    if (!StepDone(db_, insert_variant.st, error_out)) {
        Rollback(db_);
        return false;
    }

    const auto tas_variant_id = sqlite3_last_insert_rowid(db_);
    const auto aggregate_id = std::to_string(tas_variant_id);
    if (!InsertStateOutboxEvent(
            db_,
            command.event_id,
            "State.TasVariantCreated.v1",
            "tas_variant",
            aggregate_id,
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "tas_variant",
            tas_variant_id,
            error_out)) {
        Rollback(db_);
        return false;
    }

    if (!Commit(db_, error_out)) {
        Rollback(db_);
        return false;
    }

    if (tas_variant_id_out) {
        *tas_variant_id_out = tas_variant_id;
    }
    return true;
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
    } else if (payload_ref_kind == "savestate_derivation" || payload_ref_kind == "derivation") {
        dispatch_event_type = "State.SavestateDerived.v1";
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
    if (payload_ref_kind == "savestate_derivation" || payload_ref_kind == "derivation") {
        return ResolveSavestateDerivationRef(db_, payload_ref_id);
    }
    if (payload_ref_kind == "tas_variant" || payload_ref_kind == "tas-variant") {
        return ResolveTasVariantRef(db_, payload_ref_id);
    }

    return std::nullopt;
}

} // namespace simcore::db::state
