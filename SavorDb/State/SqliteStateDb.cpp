#include "SqliteStateDb.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <string>

#include "../Common/Events/EventPayloadDispatch.h"
#include "../Common/Events/EventPayloadValidation.h"
#include "../Common/Events/OutboxEventIds.h"

namespace savor::db::state {

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

struct ArtifactMaterializationRecord {
    std::string sha256;
    std::string file_ext;
    std::string source_path;
};

std::string NormalizeFileExt(std::string file_ext) {
    if (file_ext.empty()) return file_ext;
    if (file_ext.front() == '.') return file_ext;
    return "." + file_ext;
}

std::optional<ArtifactMaterializationRecord> LoadArtifactMaterializationRecord(
    sqlite3* db,
    std::int64_t artifact_id,
    std::string* error_out) {
    if (db == nullptr || artifact_id <= 0) {
        if (error_out) *error_out = "invalid db handle or artifact_id";
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT sha256,file_ext,filename FROM state_artifact WHERE artifact_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db);
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, artifact_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        if (error_out) *error_out = "artifact_id not found";
        return std::nullopt;
    }

    ArtifactMaterializationRecord record{};
    const auto* sha256 = sqlite3_column_text(st.st, 0);
    const auto* file_ext = sqlite3_column_text(st.st, 1);
    const auto* filename = sqlite3_column_text(st.st, 2);
    record.sha256 = sha256 == nullptr ? "" : reinterpret_cast<const char*>(sha256);
    record.file_ext = file_ext == nullptr ? "" : reinterpret_cast<const char*>(file_ext);
    record.source_path = filename == nullptr ? "" : reinterpret_cast<const char*>(filename);
    if (record.sha256.empty() || record.file_ext.empty() || record.source_path.empty()) {
        if (error_out) *error_out = "artifact row is missing required sha256/file_ext/filename fields";
        return std::nullopt;
    }

    return record;
}

std::optional<std::string> CopyArtifactFromRecord(
    const ArtifactMaterializationRecord& record,
    const std::filesystem::path& destination_path,
    std::string* error_out) {
    try {
        if (destination_path.empty()) {
            if (error_out) *error_out = "destination path is empty";
            return std::nullopt;
        }

        const std::filesystem::path source_path(record.source_path);
        if (!std::filesystem::exists(source_path)) {
            if (error_out) *error_out = "artifact source file does not exist: " + source_path.string();
            return std::nullopt;
        }
        if (!std::filesystem::is_regular_file(source_path)) {
            if (error_out) *error_out = "artifact source path is not a regular file: " + source_path.string();
            return std::nullopt;
        }

        const auto parent = destination_path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }
        std::filesystem::copy_file(
            source_path,
            destination_path,
            std::filesystem::copy_options::overwrite_existing);
        return destination_path.string();
    } catch (const std::exception& ex) {
        if (error_out) *error_out = ex.what();
        return std::nullopt;
    }
}





bool InsertStateOutboxEvent(
    sqlite3* db,
    std::string_view event_type,
    std::string_view aggregate_kind,
    std::string_view aggregate_id,
    std::string_view correlation_id,
    std::string_view causation_id,
    std::int64_t occurred_at_utc,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id,
    std::string* error_out) {
    constexpr int kMaxEventIdAttempts = 5;
    for (int attempt = 0; attempt < kMaxEventIdAttempts; ++attempt) {
        std::string event_id;
        if (!outbox::MakeDbOwnedEventId(
                db,
                "State",
                event_type,
                payload_ref_kind,
                payload_ref_id,
                &event_id,
                error_out)) {
            return false;
        }

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

        sqlite3_bind_text(st.st, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 2, event_type.data(), static_cast<int>(event_type.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 3, aggregate_kind.data(), static_cast<int>(aggregate_kind.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 4, aggregate_id.data(), static_cast<int>(aggregate_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 5, correlation_id.data(), static_cast<int>(correlation_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 6, causation_id.data(), static_cast<int>(causation_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(st.st, 7, occurred_at_utc);
        sqlite3_bind_text(st.st, 8, payload_ref_kind.data(), static_cast<int>(payload_ref_kind.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(st.st, 9, payload_ref_id);
        const auto rc = sqlite3_step(st.st);
        if (rc == SQLITE_DONE) {
            return true;
        }
        if (!outbox::IsUniqueConstraint(db)) {
            if (error_out != nullptr) {
                *error_out = sqlite3_errmsg(db);
            }
            return false;
        }
    }
    if (error_out != nullptr) {
        *error_out = "failed to generate a unique state outbox event id";
    }
    return false;
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

std::optional<events::StateArtifactPayloadView> ResolveTasMovieRootRef(
    sqlite3* db,
    std::int64_t tas_movie_root_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT r.tas_movie_root_id,r.dtm_artifact_id,r.checkpoint_savestate_id "
            "FROM state_tas_movie_root r WHERE r.tas_movie_root_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, tas_movie_root_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::StateArtifactPayloadView view{};
    view.tas_movie_root_id = sqlite3_column_int64(st.st, 0);
    view.artifact_id = sqlite3_column_int64(st.st, 1);
    view.savestate_id = sqlite3_column_int64(st.st, 2);
    return view;
}

std::optional<events::StateArtifactPayloadView> ResolveTasMovieTreeRef(
    sqlite3* db,
    std::int64_t tas_movie_tree_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT t.tas_movie_tree_id,t.dtm_artifact_id,t.checkpoint_savestate_id "
            "FROM state_tas_movie_trees t WHERE t.tas_movie_tree_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, tas_movie_tree_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    events::StateArtifactPayloadView view{};
    view.tas_movie_tree_id = sqlite3_column_int64(st.st, 0);
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
        || command.artifact_kind.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_artifact;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO state_artifact(sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7) "
            "ON CONFLICT(sha256) DO UPDATE SET "
            "size_bytes=excluded.size_bytes,compression_kind=excluded.compression_kind,"
            "filename=excluded.filename,file_ext=excluded.file_ext,artifact_kind=excluded.artifact_kind "
            "RETURNING artifact_id;",
            -1,
            &insert_artifact.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
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

    if (sqlite3_step(insert_artifact.st) != SQLITE_ROW) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto artifact_id = sqlite3_column_int64(insert_artifact.st, 0);
    sqlite3_finalize(insert_artifact.st);
    insert_artifact.st = nullptr;
    const auto aggregate_id = std::to_string(artifact_id);
    if (!InsertStateOutboxEvent(
            db_,
            "State.ArtifactStored.v1",
            "artifact",
            aggregate_id,
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "artifact",
            artifact_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
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
    const bool paired = command.playback_state ==
        SavestatePlaybackState::MoviePaired;
    if (command.artifact_id <= 0 || command.savestate_type.empty()
        || command.playback_state == SavestatePlaybackState::Unknown
        || paired != command.dtm_artifact_id.has_value()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    const auto state_artifact = GetArtifact(command.artifact_id);
    const auto dtm_artifact = command.dtm_artifact_id
        ? GetArtifact(*command.dtm_artifact_id)
        : std::nullopt;
    if (!state_artifact || state_artifact->artifact_kind != "SAV"
        || (paired && (!dtm_artifact || dtm_artifact->artifact_kind != "DTM"))) {
        if (error_out) *error_out = "savestate playback artifacts are invalid";
        return false;
    }

    if (const auto existing = FindSavestateByArtifactId(command.artifact_id); existing.has_value()) {
        if (existing->savestate_type != command.savestate_type
            || existing->note != command.note
            || existing->is_complete != command.is_complete
            || existing->playback_state != command.playback_state
            || existing->dtm_artifact_id != command.dtm_artifact_id) {
            if (error_out) *error_out = "savestate artifact is already bound with different immutable fields";
            return false;
        }
        if (savestate_id_out) *savestate_id_out = existing->savestate_id;
        return true;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_savestate;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO state_savestate(artifact_id,savestate_type,note,is_complete,created_at_utc,playback_state,dtm_artifact_id) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7);",
            -1,
            &insert_savestate.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
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
    const auto playback = ToDbString(command.playback_state);
    sqlite3_bind_text(
        insert_savestate.st, 6, playback.data(),
        static_cast<int>(playback.size()), SQLITE_TRANSIENT);
    if (command.dtm_artifact_id) {
        sqlite3_bind_int64(insert_savestate.st, 7, *command.dtm_artifact_id);
    } else {
        sqlite3_bind_null(insert_savestate.st, 7);
    }

    if (sqlite3_step(insert_savestate.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto savestate_id = sqlite3_last_insert_rowid(db_);
    const auto aggregate_id = std::to_string(savestate_id);
    if (!InsertStateOutboxEvent(
            db_,
            "State.SavestateCreated.v1",
            "savestate",
            aggregate_id,
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "savestate",
            savestate_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
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
        || command.source_context_id <= 0) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement existing_derivation;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT derivation_id FROM state_savestate_derivation "
            "WHERE from_savestate_id=?1 AND to_savestate_id=?2 AND method_kind=?3 "
            "AND source_context_kind=?4 AND source_context_id=?5 "
            "ORDER BY derivation_id ASC LIMIT 2;",
            -1,
            &existing_derivation.st,
            nullptr) != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(existing_derivation.st, 1, command.from_savestate_id);
    sqlite3_bind_int64(existing_derivation.st, 2, command.to_savestate_id);
    sqlite3_bind_text(existing_derivation.st, 3, command.method_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(existing_derivation.st, 4, command.source_context_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(existing_derivation.st, 5, command.source_context_id);
    const auto existing_rc = sqlite3_step(existing_derivation.st);
    if (existing_rc == SQLITE_ROW) {
        const auto existing_id = sqlite3_column_int64(existing_derivation.st, 0);
        if (sqlite3_step(existing_derivation.st) == SQLITE_ROW) {
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            if (error_out) *error_out = "multiple identical savestate derivations already exist";
            return false;
        }
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        if (derivation_id_out) *derivation_id_out = existing_id;
        return true;
    }
    if (existing_rc != SQLITE_DONE) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
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
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_derivation.st, 1, command.from_savestate_id);
    sqlite3_bind_int64(insert_derivation.st, 2, command.to_savestate_id);
    sqlite3_bind_text(insert_derivation.st, 3, command.method_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_derivation.st, 4, command.source_context_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_derivation.st, 5, command.source_context_id);
    sqlite3_bind_int64(insert_derivation.st, 6, command.created_at_utc.time_since_epoch().count());

    if (sqlite3_step(insert_derivation.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto derivation_id = sqlite3_last_insert_rowid(db_);
    const auto aggregate_id = std::to_string(command.to_savestate_id);
    if (!InsertStateOutboxEvent(
            db_,
            "State.SavestateDerived.v1",
            "savestate_derivation",
            aggregate_id,
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "savestate_derivation",
            derivation_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (derivation_id_out) {
        *derivation_id_out = derivation_id;
    }
    return true;
}

std::optional<ArtifactRecord> SqliteStateDb::GetArtifact(
    std::int64_t artifact_id) const {
    if (db_ == nullptr || artifact_id <= 0) return std::nullopt;
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT artifact_id,sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc "
            "FROM state_artifact WHERE artifact_id=?1;",
            -1, &st.st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st.st, 1, artifact_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    ArtifactRecord row{};
    row.artifact_id = sqlite3_column_int64(st.st, 0);
    row.sha256 = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 1));
    row.size_bytes = sqlite3_column_int64(st.st, 2);
    row.compression_kind = sqlite3_column_int(st.st, 3);
    row.filename = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 4));
    row.file_ext = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 5));
    row.artifact_kind = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 6));
    row.created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(sqlite3_column_int64(st.st, 7)));
    return row;
}

std::optional<ArtifactRecord> SqliteStateDb::GetArtifactBySha256(
    std::string_view sha256) const {
    if (db_ == nullptr || sha256.empty()) return std::nullopt;
    Statement st;
    if (sqlite3_prepare_v2(db_, "SELECT artifact_id FROM state_artifact WHERE sha256=?1;", -1, &st.st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(st.st, 1, sha256.data(), static_cast<int>(sha256.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    return GetArtifact(sqlite3_column_int64(st.st, 0));
}

std::optional<SavestateRecord> SqliteStateDb::FindSavestateByArtifactId(
    std::int64_t artifact_id) const {
    if (db_ == nullptr || artifact_id <= 0) return std::nullopt;
    Statement st;
    if (sqlite3_prepare_v2(db_, "SELECT savestate_id FROM state_savestate WHERE artifact_id=?1;", -1, &st.st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st.st, 1, artifact_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    return GetSavestate(sqlite3_column_int64(st.st, 0));
}

std::optional<SavestateRecord> SqliteStateDb::GetSavestate(
    std::int64_t savestate_id) const {
    if (db_ == nullptr || savestate_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT s.savestate_id,s.artifact_id,s.savestate_type,s.note,s.is_complete,s.created_at_utc,"
        "a.sha256,a.size_bytes,a.filename,a.file_ext,a.artifact_kind,"
        "s.playback_state,s.dtm_artifact_id,d.sha256,d.filename "
        "FROM state_savestate s "
        "JOIN state_artifact a ON a.artifact_id=s.artifact_id "
        "LEFT JOIN state_artifact d ON d.artifact_id=s.dtm_artifact_id "
        "WHERE s.savestate_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, savestate_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    SavestateRecord row{};
    row.savestate_id = sqlite3_column_int64(st.st, 0);
    row.artifact_id = sqlite3_column_int64(st.st, 1);
    const auto* type = sqlite3_column_text(st.st, 2);
    const auto* note = sqlite3_column_text(st.st, 3);
    row.savestate_type = type == nullptr ? "" : reinterpret_cast<const char*>(type);
    row.note = note == nullptr ? "" : reinterpret_cast<const char*>(note);
    row.is_complete = sqlite3_column_int(st.st, 4) != 0;
    row.created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(sqlite3_column_int64(st.st, 5)));
    const auto* sha256 = sqlite3_column_text(st.st, 6);
    row.artifact_sha256 = sha256 == nullptr ? "" : reinterpret_cast<const char*>(sha256);
    row.artifact_size_bytes = sqlite3_column_int64(st.st, 7);
    const auto* filename = sqlite3_column_text(st.st, 8);
    const auto* extension = sqlite3_column_text(st.st, 9);
    row.artifact_filename = filename == nullptr ? "" : reinterpret_cast<const char*>(filename);
    row.artifact_file_ext = extension == nullptr ? "" : reinterpret_cast<const char*>(extension);
    const auto* artifact_kind = sqlite3_column_text(st.st, 10);
    row.artifact_kind = artifact_kind == nullptr ? "" : reinterpret_cast<const char*>(artifact_kind);
    const auto* playback = sqlite3_column_text(st.st, 11);
    row.playback_state = ParseSavestatePlaybackState(
        playback == nullptr ? "" : reinterpret_cast<const char*>(playback));
    if (sqlite3_column_type(st.st, 12) != SQLITE_NULL) {
        row.dtm_artifact_id = sqlite3_column_int64(st.st, 12);
    }
    if (const auto* hash = sqlite3_column_text(st.st, 13); hash != nullptr) {
        row.dtm_sha256 = reinterpret_cast<const char*>(hash);
    }
    if (const auto* name = sqlite3_column_text(st.st, 14); name != nullptr) {
        row.dtm_filename = reinterpret_cast<const char*>(name);
    }
    return row;
}

std::vector<SavestateDerivationRecord> SqliteStateDb::ListIncomingSavestateDerivations(
    std::int64_t to_savestate_id) const {
    std::vector<SavestateDerivationRecord> rows;
    if (db_ == nullptr || to_savestate_id <= 0) {
        return rows;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT derivation_id,from_savestate_id,to_savestate_id,method_kind,source_context_kind,source_context_id,created_at_utc "
        "FROM state_savestate_derivation WHERE to_savestate_id=?1 ORDER BY derivation_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, to_savestate_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        SavestateDerivationRecord row{};
        row.derivation_id = sqlite3_column_int64(st.st, 0);
        row.from_savestate_id = sqlite3_column_int64(st.st, 1);
        row.to_savestate_id = sqlite3_column_int64(st.st, 2);
        const auto* method = sqlite3_column_text(st.st, 3);
        const auto* context = sqlite3_column_text(st.st, 4);
        row.method_kind = method == nullptr ? "" : reinterpret_cast<const char*>(method);
        row.source_context_kind = context == nullptr ? "" : reinterpret_cast<const char*>(context);
        row.source_context_id = sqlite3_column_int64(st.st, 5);
        row.created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(sqlite3_column_int64(st.st, 6)));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<SavestateDerivationRecord> SqliteStateDb::ListSavestateDerivationsBySourceContext(
    std::string_view source_context_kind,
    std::int64_t source_context_id) const {
    std::vector<SavestateDerivationRecord> rows;
    if (db_ == nullptr || source_context_kind.empty() || source_context_id <= 0) {
        return rows;
    }

    Statement st;
    constexpr const char* kSql =
        "SELECT derivation_id,from_savestate_id,to_savestate_id,method_kind,"
        "source_context_kind,source_context_id,created_at_utc "
        "FROM state_savestate_derivation "
        "WHERE source_context_kind=?1 AND source_context_id=?2 "
        "ORDER BY derivation_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return rows;
    }

    sqlite3_bind_text(
        st.st,
        1,
        source_context_kind.data(),
        static_cast<int>(source_context_kind.size()),
        SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 2, source_context_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        SavestateDerivationRecord row{};
        row.derivation_id = sqlite3_column_int64(st.st, 0);
        row.from_savestate_id = sqlite3_column_int64(st.st, 1);
        row.to_savestate_id = sqlite3_column_int64(st.st, 2);
        const auto* method = sqlite3_column_text(st.st, 3);
        const auto* context = sqlite3_column_text(st.st, 4);
        row.method_kind = method == nullptr ? "" : reinterpret_cast<const char*>(method);
        row.source_context_kind = context == nullptr ? "" : reinterpret_cast<const char*>(context);
        row.source_context_id = sqlite3_column_int64(st.st, 5);
        row.created_at_utc =
            types::UtcTimePoint(std::chrono::milliseconds(sqlite3_column_int64(st.st, 6)));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::optional<TasMovieRootRecord> SqliteStateDb::GetTasMovieRoot(
    std::int64_t tas_movie_root_id) const {
    if (db_ == nullptr || tas_movie_root_id <= 0) return std::nullopt;
    Statement st;
    constexpr const char* kSql =
        "SELECT tas_movie_root_id,source_dtm_artifact_id,dtm_artifact_id,rtc_value,"
        "itinerary_artifact_id,required_final_breakpoint_pc,checkpoint_savestate_id,"
        "source_context_kind,source_context_id,created_at_utc "
        "FROM state_tas_movie_root WHERE tas_movie_root_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st.st, 1, tas_movie_root_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    TasMovieRootRecord row{};
    row.tas_movie_root_id = sqlite3_column_int64(st.st, 0);
    row.source_dtm_artifact_id = sqlite3_column_int64(st.st, 1);
    row.dtm_artifact_id = sqlite3_column_int64(st.st, 2);
    row.rtc_value = sqlite3_column_int64(st.st, 3);
    row.itinerary_artifact_id = sqlite3_column_int64(st.st, 4);
    row.required_final_breakpoint_pc = static_cast<std::uint32_t>(sqlite3_column_int64(st.st, 5));
    row.checkpoint_savestate_id = sqlite3_column_int64(st.st, 6);
    row.source_context_kind = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 7));
    row.source_context_id = sqlite3_column_int64(st.st, 8);
    row.created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(sqlite3_column_int64(st.st, 9)));
    return row;
}

std::optional<TasMovieRootRecord> SqliteStateDb::FindTasMovieRootBySourceRtc(
    std::int64_t source_dtm_artifact_id,
    std::int64_t rtc_value) const {
    if (db_ == nullptr || source_dtm_artifact_id <= 0 || rtc_value < 0) return std::nullopt;
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT tas_movie_root_id FROM state_tas_movie_root "
            "WHERE source_dtm_artifact_id=?1 AND rtc_value=?2;",
            -1, &st.st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st.st, 1, source_dtm_artifact_id);
    sqlite3_bind_int64(st.st, 2, rtc_value);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    return GetTasMovieRoot(sqlite3_column_int64(st.st, 0));
}

std::optional<TasMovieRootRecord> SqliteStateDb::FindTasMovieRootByDtmArtifactId(
    std::int64_t dtm_artifact_id) const {
    if (db_ == nullptr || dtm_artifact_id <= 0) return std::nullopt;
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT tas_movie_root_id FROM state_tas_movie_root WHERE dtm_artifact_id=?1;",
            -1, &st.st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st.st, 1, dtm_artifact_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    return GetTasMovieRoot(sqlite3_column_int64(st.st, 0));
}

bool SqliteStateDb::CreateTasMovieRoot(
    const CreateTasMovieRootCommand& command,
    std::int64_t* tas_movie_root_id_out,
    std::string* error_out) {
    if (db_ == nullptr || command.source_dtm_artifact_id <= 0 || command.dtm_artifact_id <= 0
        || command.rtc_value < 0 || command.itinerary_artifact_id <= 0
        || command.required_final_breakpoint_pc == 0 || command.checkpoint_savestate_id <= 0 || command.source_context_kind.empty()
        || command.source_context_id <= 0) {
        if (error_out) *error_out = "invalid TAS movie root command";
        return false;
    }
    const auto source_artifact = GetArtifact(command.source_dtm_artifact_id);
    const auto dtm_artifact = GetArtifact(command.dtm_artifact_id);
    const auto itinerary_artifact = GetArtifact(command.itinerary_artifact_id);
    const auto checkpoint = GetSavestate(command.checkpoint_savestate_id);
    if (!source_artifact || source_artifact->artifact_kind != "DTM"
        || !dtm_artifact || dtm_artifact->artifact_kind != "DTM"
        || !itinerary_artifact || itinerary_artifact->artifact_kind != "TAS_MOVIE_ITINERARY"
        || !checkpoint || checkpoint->artifact_kind != "SAV"
        || checkpoint->playback_state != SavestatePlaybackState::MoviePaired
        || checkpoint->dtm_artifact_id != command.dtm_artifact_id) {
        if (error_out) *error_out = "TAS movie root artifacts do not satisfy their immutable kinds";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto existing = FindTasMovieRootBySourceRtc(command.source_dtm_artifact_id, command.rtc_value);
    if (existing.has_value()) {
        const bool same = existing->dtm_artifact_id == command.dtm_artifact_id
            && existing->itinerary_artifact_id == command.itinerary_artifact_id
            && existing->required_final_breakpoint_pc == command.required_final_breakpoint_pc
            && existing->checkpoint_savestate_id == command.checkpoint_savestate_id;
        if (!same) {
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            if (error_out) *error_out = "source DTM and RTC already identify a different immutable root";
            return false;
        }
        (void)sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
        if (tas_movie_root_id_out) *tas_movie_root_id_out = existing->tas_movie_root_id;
        return true;
    }
    Statement st;
    constexpr const char* kSql =
        "INSERT INTO state_tas_movie_root(source_dtm_artifact_id,dtm_artifact_id,rtc_value,"
        "itinerary_artifact_id,required_final_breakpoint_pc,checkpoint_savestate_id,"
        "source_context_kind,source_context_id,created_at_utc) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, command.source_dtm_artifact_id);
    sqlite3_bind_int64(st.st, 2, command.dtm_artifact_id);
    sqlite3_bind_int64(st.st, 3, command.rtc_value);
    sqlite3_bind_int64(st.st, 4, command.itinerary_artifact_id);
    sqlite3_bind_int64(st.st, 5, command.required_final_breakpoint_pc);
    sqlite3_bind_int64(st.st, 6, command.checkpoint_savestate_id);
    sqlite3_bind_text(st.st, 7, command.source_context_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 8, command.source_context_id);
    sqlite3_bind_int64(st.st, 9, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    const auto id = sqlite3_last_insert_rowid(db_);
    if (!InsertStateOutboxEvent(db_, "State.TasMovieRootCreated.v1", "tas_movie_root",
            std::to_string(id), command.correlation_id, command.causation_id,
            command.created_at_utc.time_since_epoch().count(), "tas_movie_root", id, error_out)
        || sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out && error_out->empty()) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    if (tas_movie_root_id_out) *tas_movie_root_id_out = id;
    return true;
}

bool SqliteStateDb::CreateOrGetSterilizedCheckpoint(
    const CreateOrGetSterilizedCheckpointCommand& command,
    CreateOrGetSterilizedCheckpointReceipt* receipt_out,
    std::string* error_out) {
    if (db_ == nullptr || command.from_savestate_id <= 0
        || command.artifact.sha256.empty()
        || command.artifact.filename.empty()
        || command.artifact.file_ext != ".sav"
        || command.artifact.artifact_kind != "SAV"
        || command.savestate_type.empty()
        || command.method_kind != "tasmovie.checkpoint_sterilize.v1"
        || command.source_context_kind.empty()
        || command.source_context_id <= 0) {
        if (error_out) *error_out = "sterilized checkpoint command is incomplete";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto rollback = [&]() {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    };
    const auto fail = [&](std::string message) {
        rollback();
        if (error_out) *error_out = std::move(message);
        return false;
    };

    Statement existing;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT d.derivation_id,d.to_savestate_id,s.artifact_id "
            "FROM state_savestate_derivation d "
            "JOIN state_savestate s ON s.savestate_id=d.to_savestate_id "
            "WHERE d.from_savestate_id=?1 AND d.method_kind=?2;",
            -1, &existing.st, nullptr) != SQLITE_OK) {
        return fail(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(existing.st, 1, command.from_savestate_id);
    sqlite3_bind_text(existing.st, 2, command.method_kind.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(existing.st) == SQLITE_ROW) {
        CreateOrGetSterilizedCheckpointReceipt receipt{
            .savestate_id = sqlite3_column_int64(existing.st, 1),
            .artifact_id = sqlite3_column_int64(existing.st, 2),
            .derivation_id = sqlite3_column_int64(existing.st, 0),
            .created = false,
        };
        if (sqlite3_step(existing.st) == SQLITE_ROW) {
            return fail("multiple canonical sterilizations exist for one source checkpoint");
        }
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            return fail(sqlite3_errmsg(db_));
        }
        if (receipt_out) *receipt_out = receipt;
        if (error_out) error_out->clear();
        return true;
    }

    Statement source;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT 1 FROM state_savestate s JOIN state_artifact d "
            "ON d.artifact_id=s.dtm_artifact_id "
            "WHERE s.savestate_id=?1 AND s.is_complete=1 "
            "AND s.playback_state='MOVIE_PAIRED' AND d.artifact_kind='DTM';",
            -1, &source.st, nullptr) != SQLITE_OK) {
        return fail(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(source.st, 1, command.from_savestate_id);
    if (sqlite3_step(source.st) != SQLITE_ROW) {
        return fail("sterilization source is not a complete movie-paired checkpoint");
    }

    Statement artifact;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO state_artifact(sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7) "
            "ON CONFLICT(sha256) DO UPDATE SET "
            "size_bytes=excluded.size_bytes,compression_kind=excluded.compression_kind,"
            "filename=excluded.filename,file_ext=excluded.file_ext,artifact_kind=excluded.artifact_kind "
            "RETURNING artifact_id;",
            -1, &artifact.st, nullptr) != SQLITE_OK) {
        return fail(sqlite3_errmsg(db_));
    }
    sqlite3_bind_text(artifact.st, 1, command.artifact.sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(artifact.st, 2, command.artifact.size_bytes);
    sqlite3_bind_int(artifact.st, 3, command.artifact.compression_kind);
    sqlite3_bind_text(artifact.st, 4, command.artifact.filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(artifact.st, 5, command.artifact.file_ext.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(artifact.st, 6, command.artifact.artifact_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(artifact.st, 7, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(artifact.st) != SQLITE_ROW) return fail(sqlite3_errmsg(db_));
    const auto artifact_id = sqlite3_column_int64(artifact.st, 0);
    sqlite3_finalize(artifact.st);
    artifact.st = nullptr;

    Statement insert_state;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO state_savestate(artifact_id,savestate_type,note,is_complete,created_at_utc,playback_state,dtm_artifact_id) "
            "VALUES(?1,?2,?3,1,?4,'MOVIE_INACTIVE',NULL) "
            "ON CONFLICT(artifact_id) DO NOTHING;",
            -1, &insert_state.st, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(insert_state.st, 1, artifact_id);
    sqlite3_bind_text(insert_state.st, 2, command.savestate_type.c_str(), -1, SQLITE_TRANSIENT);
    if (command.note.empty()) sqlite3_bind_null(insert_state.st, 3);
    else sqlite3_bind_text(insert_state.st, 3, command.note.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_state.st, 4, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_state.st) != SQLITE_DONE) return fail(sqlite3_errmsg(db_));
    const bool state_created = sqlite3_changes(db_) != 0;

    Statement select_state;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT savestate_id,savestate_type,COALESCE(note,''),is_complete,playback_state,dtm_artifact_id "
            "FROM state_savestate WHERE artifact_id=?1;",
            -1, &select_state.st, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(select_state.st, 1, artifact_id);
    if (sqlite3_step(select_state.st) != SQLITE_ROW) return fail("sterilized savestate row was not created");
    const auto savestate_id = sqlite3_column_int64(select_state.st, 0);
    const std::string type = reinterpret_cast<const char*>(sqlite3_column_text(select_state.st, 1));
    const std::string note = reinterpret_cast<const char*>(sqlite3_column_text(select_state.st, 2));
    const std::string playback = reinterpret_cast<const char*>(sqlite3_column_text(select_state.st, 4));
    if (type != command.savestate_type || note != command.note
        || sqlite3_column_int(select_state.st, 3) != 1
        || playback != "MOVIE_INACTIVE"
        || sqlite3_column_type(select_state.st, 5) != SQLITE_NULL) {
        return fail("sterilized artifact is already bound to incompatible immutable state");
    }

    Statement derivation;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO state_savestate_derivation(from_savestate_id,to_savestate_id,method_kind,source_context_kind,source_context_id,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6);",
            -1, &derivation.st, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
    sqlite3_bind_int64(derivation.st, 1, command.from_savestate_id);
    sqlite3_bind_int64(derivation.st, 2, savestate_id);
    sqlite3_bind_text(derivation.st, 3, command.method_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(derivation.st, 4, command.source_context_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(derivation.st, 5, command.source_context_id);
    sqlite3_bind_int64(derivation.st, 6, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(derivation.st) != SQLITE_DONE) return fail(sqlite3_errmsg(db_));
    const auto derivation_id = sqlite3_last_insert_rowid(db_);

    if (!InsertStateOutboxEvent(
            db_, "State.ArtifactStored.v1", "artifact",
            std::to_string(artifact_id), command.correlation_id,
            command.causation_id, command.created_at_utc.time_since_epoch().count(),
            "artifact", artifact_id, error_out)) {
        rollback();
        return false;
    }
    if (state_created && !InsertStateOutboxEvent(
            db_, "State.SavestateCreated.v1", "savestate",
            std::to_string(savestate_id), command.correlation_id,
            command.causation_id, command.created_at_utc.time_since_epoch().count(),
            "savestate", savestate_id, error_out)) {
        rollback();
        return false;
    }

    if (!InsertStateOutboxEvent(
            db_, "State.SavestateDerived.v1", "savestate_derivation",
            std::to_string(savestate_id), command.correlation_id,
            command.causation_id, command.created_at_utc.time_since_epoch().count(),
            "savestate_derivation", derivation_id, error_out)) {
        rollback();
        return false;
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return fail(sqlite3_errmsg(db_));
    }
    if (receipt_out) {
        *receipt_out = {
            .savestate_id = savestate_id,
            .artifact_id = artifact_id,
            .derivation_id = derivation_id,
            .created = true,
        };
    }
    if (error_out) error_out->clear();
    return true;
}

std::optional<TasMovieTreeRecord> SqliteStateDb::GetTasMovieTree(
    std::int64_t tas_movie_tree_id) const {
    if (db_ == nullptr || tas_movie_tree_id <= 0) return std::nullopt;
    Statement st;
    constexpr const char* kSql =
        "SELECT tas_movie_tree_id,tas_movie_root_id,parent_tas_movie_tree_id,dtm_artifact_id,"
        "itinerary_artifact_id,required_final_breakpoint_pc,checkpoint_savestate_id,"
        "source_context_kind,source_context_id,created_at_utc FROM state_tas_movie_trees "
        "WHERE tas_movie_tree_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st.st, 1, tas_movie_tree_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    TasMovieTreeRecord row{};
    row.tas_movie_tree_id = sqlite3_column_int64(st.st, 0);
    row.tas_movie_root_id = sqlite3_column_int64(st.st, 1);
    if (sqlite3_column_type(st.st, 2) != SQLITE_NULL) row.parent_tas_movie_tree_id = sqlite3_column_int64(st.st, 2);
    row.dtm_artifact_id = sqlite3_column_int64(st.st, 3);
    row.itinerary_artifact_id = sqlite3_column_int64(st.st, 4);
    row.required_final_breakpoint_pc = static_cast<std::uint32_t>(sqlite3_column_int64(st.st, 5));
    row.checkpoint_savestate_id = sqlite3_column_int64(st.st, 6);
    row.source_context_kind = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 7));
    row.source_context_id = sqlite3_column_int64(st.st, 8);
    row.created_at_utc = types::UtcTimePoint(std::chrono::milliseconds(sqlite3_column_int64(st.st, 9)));
    return row;
}

std::optional<TasMovieTreeRecord> SqliteStateDb::FindTasMovieTreeByDtmArtifactId(
    std::int64_t dtm_artifact_id) const {
    if (db_ == nullptr || dtm_artifact_id <= 0) return std::nullopt;
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT tas_movie_tree_id FROM state_tas_movie_trees WHERE dtm_artifact_id=?1;",
            -1, &st.st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(st.st, 1, dtm_artifact_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    return GetTasMovieTree(sqlite3_column_int64(st.st, 0));
}

bool SqliteStateDb::CreateTasMovieTree(
    const CreateTasMovieTreeCommand& command,
    std::int64_t* tas_movie_tree_id_out,
    std::string* error_out) {
    if (db_ == nullptr || command.tas_movie_root_id <= 0 || command.dtm_artifact_id <= 0
        || command.itinerary_artifact_id <= 0 || command.required_final_breakpoint_pc == 0
        || command.checkpoint_savestate_id <= 0
        || command.source_context_kind.empty() || command.source_context_id <= 0) {
        if (error_out) *error_out = "invalid TAS movie tree command";
        return false;
    }
    if (!GetTasMovieRoot(command.tas_movie_root_id).has_value()) {
        if (error_out) *error_out = "TAS movie root does not exist";
        return false;
    }
    const auto dtm_artifact = GetArtifact(command.dtm_artifact_id);
    const auto itinerary_artifact = GetArtifact(command.itinerary_artifact_id);
    const auto checkpoint = GetSavestate(command.checkpoint_savestate_id);
    if (!dtm_artifact || dtm_artifact->artifact_kind != "DTM"
        || !itinerary_artifact || itinerary_artifact->artifact_kind != "TAS_MOVIE_ITINERARY"
        || !checkpoint || checkpoint->artifact_kind != "SAV"
        || checkpoint->playback_state != SavestatePlaybackState::MoviePaired
        || checkpoint->dtm_artifact_id != command.dtm_artifact_id) {
        if (error_out) *error_out = "TAS movie tree artifacts do not satisfy their immutable kinds";
        return false;
    }
    if (command.parent_tas_movie_tree_id.has_value()) {
        const auto parent = GetTasMovieTree(*command.parent_tas_movie_tree_id);
        if (!parent.has_value() || parent->tas_movie_root_id != command.tas_movie_root_id) {
            if (error_out) *error_out = "parent TAS movie tree must belong to the same root";
            return false;
        }
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    Statement find;
    if (sqlite3_prepare_v2(db_, "SELECT tas_movie_tree_id FROM state_tas_movie_trees WHERE dtm_artifact_id=?1;", -1, &find.st, nullptr) != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(find.st, 1, command.dtm_artifact_id);
    if (sqlite3_step(find.st) == SQLITE_ROW) {
        const auto existing = GetTasMovieTree(sqlite3_column_int64(find.st, 0));
        const bool same = existing.has_value()
            && existing->tas_movie_root_id == command.tas_movie_root_id
            && existing->parent_tas_movie_tree_id == command.parent_tas_movie_tree_id
            && existing->itinerary_artifact_id == command.itinerary_artifact_id
            && existing->required_final_breakpoint_pc == command.required_final_breakpoint_pc
            && existing->checkpoint_savestate_id == command.checkpoint_savestate_id
            && existing->source_context_kind == command.source_context_kind
            && existing->source_context_id == command.source_context_id;
        if (!same) {
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            if (error_out) *error_out = "DTM already identifies a different immutable TAS movie tree";
            return false;
        }
        (void)sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
        if (tas_movie_tree_id_out) *tas_movie_tree_id_out = existing->tas_movie_tree_id;
        return true;
    }
    Statement st;
    constexpr const char* kSql =
        "INSERT INTO state_tas_movie_trees(tas_movie_root_id,parent_tas_movie_tree_id,dtm_artifact_id,"
        "itinerary_artifact_id,required_final_breakpoint_pc,checkpoint_savestate_id,source_context_kind,"
        "source_context_id,created_at_utc) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, command.tas_movie_root_id);
    if (command.parent_tas_movie_tree_id) sqlite3_bind_int64(st.st, 2, *command.parent_tas_movie_tree_id); else sqlite3_bind_null(st.st, 2);
    sqlite3_bind_int64(st.st, 3, command.dtm_artifact_id);
    sqlite3_bind_int64(st.st, 4, command.itinerary_artifact_id);
    sqlite3_bind_int64(st.st, 5, command.required_final_breakpoint_pc);
    sqlite3_bind_int64(st.st, 6, command.checkpoint_savestate_id);
    sqlite3_bind_text(st.st, 7, command.source_context_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 8, command.source_context_id);
    sqlite3_bind_int64(st.st, 9, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    const auto id = sqlite3_last_insert_rowid(db_);
    if (!InsertStateOutboxEvent(db_, "State.TasMovieTreeCreated.v1", "tas_movie_tree",
            std::to_string(id), command.correlation_id, command.causation_id,
            command.created_at_utc.time_since_epoch().count(), "tas_movie_tree", id, error_out)
        || sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out && error_out->empty()) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    if (tas_movie_tree_id_out) *tas_movie_tree_id_out = id;
    return true;
}

std::vector<TasMovieTreeRecord> SqliteStateDb::ListTasMovieTreeLineage(
    std::int64_t tas_movie_tree_id) const {
    std::vector<TasMovieTreeRecord> rows;
    if (db_ == nullptr || tas_movie_tree_id <= 0) return rows;
    Statement st;
    constexpr const char* kSql =
        "WITH RECURSIVE lineage(id,depth) AS (SELECT ?1,0 UNION ALL "
        "SELECT t.parent_tas_movie_tree_id,depth+1 FROM state_tas_movie_trees t JOIN lineage l "
        "ON t.tas_movie_tree_id=l.id WHERE t.parent_tas_movie_tree_id IS NOT NULL) "
        "SELECT id FROM lineage ORDER BY depth DESC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) return rows;
    sqlite3_bind_int64(st.st, 1, tas_movie_tree_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        if (auto row = GetTasMovieTree(sqlite3_column_int64(st.st, 0)); row.has_value()) rows.push_back(*row);
    }
    return rows;
}

std::optional<SavestateDerivationRecord>
SqliteStateDb::FindSavestateDerivationBySourceAndMethod(
    std::int64_t from_savestate_id,
    std::string_view method_kind) const {
    if (db_ == nullptr || from_savestate_id <= 0 || method_kind.empty()) {
        return std::nullopt;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT derivation_id,from_savestate_id,to_savestate_id,method_kind,"
        "source_context_kind,source_context_id,created_at_utc "
        "FROM state_savestate_derivation "
        "WHERE from_savestate_id=?1 AND method_kind=?2 "
        "ORDER BY derivation_id ASC LIMIT 2;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, from_savestate_id);
    sqlite3_bind_text(
        st.st, 2, method_kind.data(), static_cast<int>(method_kind.size()),
        SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_ROW) return std::nullopt;
    SavestateDerivationRecord row{};
    row.derivation_id = sqlite3_column_int64(st.st, 0);
    row.from_savestate_id = sqlite3_column_int64(st.st, 1);
    row.to_savestate_id = sqlite3_column_int64(st.st, 2);
    row.method_kind = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 3));
    row.source_context_kind = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 4));
    row.source_context_id = sqlite3_column_int64(st.st, 5);
    row.created_at_utc = types::UtcTimePoint(
        std::chrono::milliseconds(sqlite3_column_int64(st.st, 6)));
    if (sqlite3_step(st.st) == SQLITE_ROW) return std::nullopt;
    return row;
}

std::optional<std::string> SqliteStateDb::MaterializeArtifactToDirectory(
    std::int64_t artifact_id,
    std::string_view output_directory,
    std::string* error_out) const {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return std::nullopt;
    }
    if (artifact_id <= 0 || output_directory.empty()) {
        if (error_out) *error_out = "artifact_id and output_directory are required";
        return std::nullopt;
    }

    auto record = LoadArtifactMaterializationRecord(db_, artifact_id, error_out);
    if (!record.has_value()) {
        return std::nullopt;
    }

    const auto output_filename = record->sha256 + NormalizeFileExt(record->file_ext);
    const auto destination_path = std::filesystem::path(output_directory) / output_filename;
    return CopyArtifactFromRecord(record.value(), destination_path, error_out);
}

std::optional<std::string> SqliteStateDb::MaterializeArtifactToPath(
    std::int64_t artifact_id,
    std::string_view output_path,
    std::string* error_out) const {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return std::nullopt;
    }
    if (artifact_id <= 0 || output_path.empty()) {
        if (error_out) *error_out = "artifact_id and output_path are required";
        return std::nullopt;
    }

    auto record = LoadArtifactMaterializationRecord(db_, artifact_id, error_out);
    if (!record.has_value()) {
        return std::nullopt;
    }

    return CopyArtifactFromRecord(
        record.value(),
        std::filesystem::path(output_path),
        error_out);
}

std::optional<std::string> SqliteStateDb::MaterializeSavestateToPath(
    std::int64_t savestate_id,
    std::string_view output_path,
    std::string* error_out) const {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return std::nullopt;
    }
    if (savestate_id <= 0 || output_path.empty()) {
        if (error_out) *error_out = "savestate_id and output_path are required";
        return std::nullopt;
    }

    const auto savestate_ref = ResolveSavestateRef(db_, savestate_id);
    if (!savestate_ref.has_value() || savestate_ref->artifact_id <= 0) {
        if (error_out) *error_out = "savestate_id not found";
        return std::nullopt;
    }

    return MaterializeArtifactToPath(savestate_ref->artifact_id, output_path, error_out);
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

retention::OutboxRetentionPreview SqliteStateDb::PreviewOutboxRetention(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy) const {
    std::int64_t max_outbox_id = 0;
    Statement st;
    if (db_ != nullptr
        && sqlite3_prepare_v2(db_, "SELECT COALESCE(MAX(outbox_id), 0) FROM state_outbox_message;", -1, &st.st, nullptr)
            == SQLITE_OK
        && sqlite3_step(st.st) == SQLITE_ROW) {
        max_outbox_id = sqlite3_column_int64(st.st, 0);
    }

    return retention::BuildOutboxRetentionPreview(max_outbox_id, subscriptions, now_utc, policy);
}

bool SqliteStateDb::PurgeOutboxThroughRetentionFloor(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy,
    int max_rows,
    int* rows_deleted_out,
    std::string* error_out) {
    if (rows_deleted_out) *rows_deleted_out = 0;
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (max_rows <= 0) {
        if (error_out) *error_out = "max_rows must be > 0";
        return false;
    }

    const auto preview = PreviewOutboxRetention(subscriptions, now_utc, policy);
    if (preview.IsPurgeBlocked()) {
        if (error_out) *error_out = "purge blocked by required paused/error subscriptions";
        return false;
    }
    if (!preview.safe_purge_floor_outbox_id.has_value()) {
        if (error_out) *error_out = "safe purge floor unavailable";
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "DELETE FROM state_outbox_message "
            "WHERE outbox_id IN ("
            "  SELECT outbox_id FROM state_outbox_message "
            "  WHERE published_at_utc IS NOT NULL AND outbox_id < ?1 "
            "  ORDER BY outbox_id ASC LIMIT ?2"
            ");",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(st.st, 1, preview.safe_purge_floor_outbox_id.value());
    sqlite3_bind_int(st.st, 2, max_rows);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    if (rows_deleted_out) *rows_deleted_out = sqlite3_changes(db_);
    return true;
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
    } else if (payload_ref_kind == "tas_movie_root") {
        dispatch_event_type = "State.TasMovieRootCreated.v1";
    } else if (payload_ref_kind == "tas_movie_tree") {
        dispatch_event_type = "State.TasMovieTreeCreated.v1";
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
    if (payload_ref_kind == "tas_movie_root") {
        return ResolveTasMovieRootRef(db_, payload_ref_id);
    }
    if (payload_ref_kind == "tas_movie_tree") {
        return ResolveTasMovieTreeRef(db_, payload_ref_id);
    }

    return std::nullopt;
}

} // namespace savor::db::state
