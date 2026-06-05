#include "ArtifactProjector.h"

#include "ProjectorContract.h"

#include <filesystem>
#include <string>

namespace simcore::db::uiread::projectors {
namespace {

void BasenameSqlFunction(sqlite3_context* context, int argc, sqlite3_value** argv) {
    if (argc != 1 || argv == nullptr || argv[0] == nullptr || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_text(context, "", -1, SQLITE_TRANSIENT);
        return;
    }

    const auto* raw_text = sqlite3_value_text(argv[0]);
    const std::string value = raw_text == nullptr ? std::string{} : reinterpret_cast<const char*>(raw_text);
    auto filename = std::filesystem::path(value).filename().string();
    if (filename.empty()) {
        filename = value;
    }
    sqlite3_result_text(context, filename.c_str(), -1, SQLITE_TRANSIENT);
}

bool RegisterBasenameFunction(sqlite3* db, std::string* error_out) {
    if (db == nullptr) {
        if (error_out != nullptr) {
            *error_out = "database handle is null";
        }
        return false;
    }

    if (sqlite3_create_function(
            db,
            "simcore_basename",
            1,
            SQLITE_UTF8 | SQLITE_DETERMINISTIC,
            nullptr,
            &BasenameSqlFunction,
            nullptr,
            nullptr)
        == SQLITE_OK) {
        return true;
    }

    if (error_out != nullptr) {
        *error_out = sqlite3_errmsg(db);
    }
    return false;
}

} // namespace

ArtifactProjector::ArtifactProjector(sqlite3* db)
    : db_(db) {
    EnsureSqlFunctions(nullptr);
}

bool ArtifactProjector::EnsureSqlFunctions(std::string* error_out) const {
    return RegisterBasenameFunction(db_, error_out);
}

bool ArtifactProjector::ProjectAll(std::string* error_out) {
    char* err = nullptr;
    constexpr const char* kSql =
        "BEGIN IMMEDIATE;"
        "INSERT INTO ui_artifact_browser(artifact_id,sha256,size_bytes,artifact_kind,filename,created_at_utc) "
        "SELECT artifact_id,sha256,size_bytes,artifact_kind,simcore_basename(filename),created_at_utc FROM state_artifact "
        "WHERE 1 "
        "ON CONFLICT(artifact_id) DO UPDATE SET "
        "sha256=excluded.sha256,size_bytes=excluded.size_bytes,artifact_kind=excluded.artifact_kind,"
        "filename=excluded.filename,created_at_utc=excluded.created_at_utc;"
        "COMMIT;";
    if (sqlite3_exec(db_, kSql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (error_out) *error_out = err ? err : sqlite3_errmsg(db_);
        sqlite3_free(err);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    return true;
}

bool ArtifactProjector::ProjectFromOutbox(const std::string& projector_name, int max_batch_size, std::string* error_out, int max_attempts) {
    if (!ValidateProjectorContractInputs(projector_name, max_batch_size, max_attempts, error_out)) {
        return false;
    }
    if (!EnsureSqlFunctions(error_out)) {
        return false;
    }

    const auto project_all = [this](const events::EventEnvelope&, std::string* handler_error) {
        return ProjectAll(handler_error);
    };

    const std::vector<events::OutboxRelayDispatchBinding> bindings{
        { { "State.ArtifactStored.v1", 1 }, project_all },
    };

    return RunProjectorRelay(
        db_,
        projector_name,
        "State",
        "state_outbox_message",
        {
            .db = db_,
            .outbox_table = "state_outbox_message",
            .context_name = "State",
            .aggregate_kind = "artifact",
            .payload_ref_kind = "artifact",
            .max_attempts = max_attempts,
        },
        bindings,
        max_batch_size,
        error_out);
}

} // namespace simcore::db::uiread::projectors
