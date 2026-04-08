#include "ValidationPhase3.h"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "Common/Events/OutboxRelay.h"
#include "Common/Migrations/MigrationRunner.h"
#include "Execution/Workflow/SqliteExecutionDb.h"
#include "Runner/Parallel/SimCoreDB/WorkflowCoordinatorBridge.h"

namespace {

using simcore::db::events::EventEnvelope;
using simcore::db::events::OutboxRelay;
using simcore::db::events::OutboxRelayDispatchBinding;
using simcore::db::events::OutboxRelayResult;

bool ExecSql(sqlite3* db, const char* sql, std::string* error_out) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc == SQLITE_OK) {
        return true;
    }

    if (error_out != nullptr) {
        *error_out = err != nullptr ? err : "sqlite3_exec failed";
    }
    sqlite3_free(err);
    return false;
}

std::string EscapeIdentifier(const std::string& identifier) {
    std::string escaped;
    escaped.reserve(identifier.size() + 4);
    escaped.push_back('"');
    for (char c : identifier) {
        if (c == '"') {
            escaped += "\"\"";
        } else {
            escaped.push_back(c);
        }
    }
    escaped.push_back('"');
    return escaped;
}

std::string EscapeJsonPathKey(const std::string& key) {
    std::string escaped;
    escaped.reserve(key.size() + 4);
    escaped += "$.\"";
    for (char c : key) {
        if (c == '"' || c == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

bool CollectObjectKeys(sqlite3* db, const std::string& object_json, std::vector<std::string>* keys_out, std::string* error_out) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT key FROM json_each(?1) WHERE key IS NOT NULL;", -1, &st, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db);
        }
        return false;
    }
    sqlite3_bind_text(st, 1, object_json.c_str(), static_cast<int>(object_json.size()), SQLITE_TRANSIENT);
    keys_out->clear();
    while (sqlite3_step(st) == SQLITE_ROW) {
        const auto* key = sqlite3_column_text(st, 0);
        if (key != nullptr) {
            keys_out->emplace_back(reinterpret_cast<const char*>(key));
        }
    }
    const int rc = sqlite3_errcode(db);
    sqlite3_finalize(st);
    if (rc != SQLITE_OK && rc != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db);
        }
        return false;
    }
    return true;
}

class DbPreparer {
public:
    DbPreparer(sqlite3* db, std::filesystem::path migration_root)
        : db_(db), migration_root_(std::move(migration_root)) {}

    bool InitializeRequiredMigrations(std::string* error_out) const {
        using namespace simcore::db::migrations;
        const MigrationSourceOptions options{ .source_kind = MigrationSourceKind::Filesystem, .filesystem_root = migration_root_ };
        return ApplyContextMigrations(db_, MigrationContext::Execution, options, error_out)
            && ApplyContextMigrations(db_, MigrationContext::Authoring, options, error_out)
            && ApplyContextMigrations(db_, MigrationContext::State, options, error_out);
    }

    bool SeedRowsFromJsonObject(const std::string& json_object, std::string* error_out) const {
        sqlite3_stmt* table_stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT key, value FROM json_each(?1) WHERE type='array';", -1, &table_stmt, nullptr) != SQLITE_OK) {
            if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_text(table_stmt, 1, json_object.c_str(), static_cast<int>(json_object.size()), SQLITE_TRANSIENT);
        while (sqlite3_step(table_stmt) == SQLITE_ROW) {
            const auto* table_text = sqlite3_column_text(table_stmt, 0);
            const auto* rows_json_text = sqlite3_column_text(table_stmt, 1);
            if (table_text == nullptr || rows_json_text == nullptr) {
                continue;
            }
            const std::string table_name = reinterpret_cast<const char*>(table_text);
            const std::string rows_json = reinterpret_cast<const char*>(rows_json_text);
            if (!SeedRowsForTableArray(table_name, rows_json, error_out)) {
                sqlite3_finalize(table_stmt);
                return false;
            }
        }
        const int rc = sqlite3_errcode(db_);
        sqlite3_finalize(table_stmt);
        if (rc != SQLITE_OK && rc != SQLITE_DONE) {
            if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        return true;
    }

    bool SeedRowsFromJsonlDirectory(const std::filesystem::path& folder, std::string* error_out) const {
        if (!std::filesystem::exists(folder)) {
            if (error_out != nullptr) *error_out = "jsonl folder does not exist: " + folder.string();
            return false;
        }
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            std::string ext = entry.path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".jsonl") {
                continue;
            }
            const std::string table_name = entry.path().stem().string();
            std::ifstream in(entry.path());
            if (!in) {
                if (error_out != nullptr) *error_out = "failed to open jsonl file: " + entry.path().string();
                return false;
            }
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) {
                    continue;
                }
                if (!InsertJsonObjectRow(table_name, line, error_out)) {
                    if (error_out != nullptr && error_out->empty()) {
                        *error_out = "failed inserting jsonl row for table " + table_name;
                    }
                    return false;
                }
            }
        }
        return true;
    }

    bool SeedSavestateArtifactAndOverride(const std::filesystem::path& savestate_file, std::string* error_out) {
        const std::uint64_t hash = std::hash<std::string>{}(savestate_file.generic_string());
        const std::int64_t artifact_id = 900000 + static_cast<std::int64_t>(hash % 100000);
        const std::int64_t savestate_id = artifact_id;
        const std::string filename = savestate_file.filename().string();
        const std::string extension = savestate_file.extension().string();
        const std::int64_t size_bytes = std::filesystem::exists(savestate_file)
            ? static_cast<std::int64_t>(std::filesystem::file_size(savestate_file))
            : 0;
        std::string pseudo_sha = "phase3-savestate-";
        pseudo_sha += std::to_string(hash);

        sqlite3_stmt* artifact_stmt = nullptr;
        if (sqlite3_prepare_v2(db_,
                "INSERT OR REPLACE INTO state_artifact(artifact_id,sha256,size_bytes,compression_kind,filename,file_ext,artifact_kind,created_at_utc)"
                " VALUES(?1,?2,?3,0,?4,?5,'SAV',unixepoch()*1000);",
                -1,
                &artifact_stmt,
                nullptr)
            != SQLITE_OK) {
            if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(artifact_stmt, 1, artifact_id);
        sqlite3_bind_text(artifact_stmt, 2, pseudo_sha.c_str(), static_cast<int>(pseudo_sha.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(artifact_stmt, 3, size_bytes);
        sqlite3_bind_text(artifact_stmt, 4, filename.c_str(), static_cast<int>(filename.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(artifact_stmt, 5, extension.c_str(), static_cast<int>(extension.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(artifact_stmt) != SQLITE_DONE) {
            if (error_out != nullptr) *error_out = "failed to insert savestate artifact row: " + std::string(sqlite3_errmsg(db_));
            sqlite3_finalize(artifact_stmt);
            return false;
        }
        sqlite3_finalize(artifact_stmt);

        sqlite3_stmt* savestate_stmt = nullptr;
        if (sqlite3_prepare_v2(db_,
                "INSERT OR REPLACE INTO state_savestate(savestate_id,artifact_id,savestate_type,note,is_complete,created_at_utc)"
                " VALUES(?1,?2,'TRANSITION',?3,1,unixepoch()*1000);",
                -1,
                &savestate_stmt,
                nullptr)
            != SQLITE_OK) {
            if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(savestate_stmt, 1, savestate_id);
        sqlite3_bind_int64(savestate_stmt, 2, artifact_id);
        sqlite3_bind_text(savestate_stmt, 3, savestate_file.generic_string().c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(savestate_stmt) != SQLITE_DONE) {
            if (error_out != nullptr) *error_out = "failed to insert savestate row: " + std::string(sqlite3_errmsg(db_));
            sqlite3_finalize(savestate_stmt);
            return false;
        }
        sqlite3_finalize(savestate_stmt);

        savestate_override_id_ = savestate_id;
        savestate_override_artifact_id_ = artifact_id;
        return true;
    }

private:
    bool SeedRowsForTableArray(const std::string& table_name, const std::string& rows_json, std::string* error_out) const {
        sqlite3_stmt* row_stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT value FROM json_each(?1) WHERE type='object';", -1, &row_stmt, nullptr) != SQLITE_OK) {
            if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_text(row_stmt, 1, rows_json.c_str(), static_cast<int>(rows_json.size()), SQLITE_TRANSIENT);
        while (sqlite3_step(row_stmt) == SQLITE_ROW) {
            const auto* row_json_text = sqlite3_column_text(row_stmt, 0);
            if (row_json_text == nullptr) {
                continue;
            }
            const std::string row_json = reinterpret_cast<const char*>(row_json_text);
            if (!InsertJsonObjectRow(table_name, row_json, error_out)) {
                sqlite3_finalize(row_stmt);
                return false;
            }
        }
        const int rc = sqlite3_errcode(db_);
        sqlite3_finalize(row_stmt);
        if (rc != SQLITE_OK && rc != SQLITE_DONE) {
            if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        return true;
    }

    bool InsertJsonObjectRow(const std::string& table_name, const std::string& row_json, std::string* error_out) const {
        std::vector<std::string> keys;
        if (!CollectObjectKeys(db_, row_json, &keys, error_out)) {
            return false;
        }
        if (keys.empty()) {
            return true;
        }

        std::ostringstream sql;
        sql << "INSERT INTO " << EscapeIdentifier(table_name) << "(";
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i > 0) sql << ',';
            sql << EscapeIdentifier(keys[i]);
        }
        sql << ") SELECT ";
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i > 0) {
                sql << ',';
            }
            const auto* override_value = OverrideValueForColumn(table_name, keys[i]);
            if (override_value != nullptr) {
                sql << *override_value;
            } else {
                sql << "json_extract(?1, ?" << (i + 2) << ")";
            }
        }
        sql << ';';

        sqlite3_stmt* insert_stmt = nullptr;
        const auto sql_text = sql.str();
        if (sqlite3_prepare_v2(db_, sql_text.c_str(), -1, &insert_stmt, nullptr) != SQLITE_OK) {
            if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_text(insert_stmt, 1, row_json.c_str(), static_cast<int>(row_json.size()), SQLITE_TRANSIENT);
        for (size_t i = 0; i < keys.size(); ++i) {
            const auto path = EscapeJsonPathKey(keys[i]);
            sqlite3_bind_text(insert_stmt, static_cast<int>(i + 2), path.c_str(), static_cast<int>(path.size()), SQLITE_TRANSIENT);
        }
        if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
            if (error_out != nullptr) {
                *error_out = "insert failed for table " + table_name + ": " + sqlite3_errmsg(db_);
            }
            sqlite3_finalize(insert_stmt);
            return false;
        }
        sqlite3_finalize(insert_stmt);
        return true;
    }

    const std::string* OverrideValueForColumn(const std::string& table_name, const std::string& column_name) const {
        if (savestate_override_id_.has_value() && IsSavestateIdColumn(column_name)) {
            savestate_override_cache_ = std::to_string(savestate_override_id_.value());
            return &savestate_override_cache_;
        }
        if (savestate_override_artifact_id_.has_value() && table_name == "state_savestate" && column_name == "artifact_id") {
            artifact_override_cache_ = std::to_string(savestate_override_artifact_id_.value());
            return &artifact_override_cache_;
        }
        return nullptr;
    }

    bool IsSavestateIdColumn(const std::string& column_name) const {
        constexpr const char* suffix = "savestate_id";
        if (column_name == suffix) {
            return true;
        }
        if (column_name.size() <= std::char_traits<char>::length(suffix)) {
            return false;
        }
        return column_name.compare(column_name.size() - std::char_traits<char>::length(suffix), std::char_traits<char>::length(suffix), suffix) == 0;
    }

    sqlite3* db_;
    std::filesystem::path migration_root_;
    std::optional<std::int64_t> savestate_override_id_;
    std::optional<std::int64_t> savestate_override_artifact_id_;
    mutable std::string savestate_override_cache_;
    mutable std::string artifact_override_cache_;
};

constexpr const char* kDefaultPhase3SeedRowsJson = R"JSON({
  "au_seed_probe_spec": [
    {
      "seed_probe_spec_id": 1,
      "name": "phase3-placeholder-seedprobe-spec",
      "base_dtm_artifact_id": 101,
      "notes": "TODO: replace with real fixture payload",
      "created_by": "validation",
      "created_at_utc": 1743465600000,
      "updated_at_utc": 1743465600000
    }
  ],
  "state_artifact": [
    {
      "artifact_id": 101,
      "sha256": "phase3-placeholder-sha256",
      "size_bytes": 1024,
      "artifact_kind": "SAV",
      "filename": "placeholder_phase3.sav",
      "created_by": "validation",
      "created_at_utc": 1743465600000
    }
  ],
  "state_savestate": [
    {
      "savestate_id": 201,
      "artifact_id": 101,
      "savestate_type": "TRANSITION",
      "created_by": "validation",
      "created_at_utc": 1743465600000
    }
  ],
  "exec_outbox_message": [
    {
      "outbox_id": 3901,
      "event_id": "evt-p3-r1",
      "event_type": "Execution.WorkflowStepReady.v1",
      "event_version": 1,
      "context_name": "Execution",
      "aggregate_kind": "workflow_step",
      "aggregate_id": "901",
      "correlation_id": "corr-p3",
      "causation_id": "cause-p3",
      "occurred_at_utc": 1743465600000,
      "payload_ref_kind": "workflow_event",
      "payload_ref_id": 1
    }
  ]
})JSON";

} // namespace

ValidationResult ValidatePhase3ReplayRobustness(const std::filesystem::path& migration_root, const Phase3DbSeedOptions& seed_options) {
    using namespace simcore::db;
    using namespace simcore::db::events;

    ValidationResult result{ .name = "phase3.replay_robustness" };
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        result.message = "failed to open sqlite memory db";
        if (db != nullptr) sqlite3_close(db);
        return result;
    }
    auto close_db = [&]() { if (db != nullptr) sqlite3_close(db); db = nullptr; };

    std::string err;
    DbPreparer preparer(db, migration_root);
    if (!preparer.InitializeRequiredMigrations(&err)) {
        result.message = "failed applying execution/authoring/state migrations: " + err;
        close_db();
        return result;
    }
    if (seed_options.savestate_file.has_value() && !seed_options.savestate_file->empty()) {
        if (!preparer.SeedSavestateArtifactAndOverride(seed_options.savestate_file.value(), &err)) {
            result.message = "failed seeding --savestate-file artifact rows: " + err;
            close_db();
            return result;
        }
    }

    const auto& default_rows_json = seed_options.default_rows_json.value_or(kDefaultPhase3SeedRowsJson);
    if (!preparer.SeedRowsFromJsonObject(default_rows_json, &err)) {
        result.message = "failed seeding default fixture rows from JSON: " + err;
        close_db();
        return result;
    }
    if (seed_options.jsonl_folder.has_value() && !seed_options.jsonl_folder->empty()) {
        if (!preparer.SeedRowsFromJsonlDirectory(seed_options.jsonl_folder.value(), &err)) {
            result.message = "failed applying JSONL fixture rows: " + err;
            close_db();
            return result;
        }
    }

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT filename FROM state_artifact WHERE artifact_id=101;", -1, &st, nullptr) != SQLITE_OK
        || sqlite3_step(st) != SQLITE_ROW) {
        if (st != nullptr) sqlite3_finalize(st);
        result.message = "failed reading placeholder state_artifact row";
        close_db();
        return result;
    }
    const auto* filename_text = sqlite3_column_text(st, 0);
    const std::filesystem::path placeholder_sav = filename_text != nullptr
        ? std::filesystem::path(reinterpret_cast<const char*>(filename_text))
        : std::filesystem::path();
    sqlite3_finalize(st);
    if (placeholder_sav.extension() != ".sav") {
        result.message = "placeholder savestate fixture must use .sav extension";
        close_db();
        return result;
    }

    OutboxRelay relay({
        .db = db,
        .outbox_table = "exec_outbox_message",
        .context_name = "Execution",
    });
    int handled = 0;
    std::vector<OutboxRelayDispatchBinding> bindings{ {
        .key = { .event_type = "Execution.WorkflowStepReady.v1", .event_version = 1 },
        .handler = [&](const EventEnvelope&, std::string*) { ++handled; return true; },
    } };

    OutboxRelayResult first{};
    if (!relay.RelayBatchFromCursor(0, 8, bindings, &first, &err)) {
        result.message = "first replay pass failed: " + err;
        close_db();
        return result;
    }
    OutboxRelayResult second{};
    if (!relay.RelayBatchFromCursor(first.last_outbox_id, 8, bindings, &second, &err)) {
        result.message = "second replay pass failed: " + err;
        close_db();
        return result;
    }
    if (first.published_count != 1 || second.published_count != 0 || handled != 1) {
        result.message = "replay cursor robustness failed (first=" + std::to_string(first.published_count)
            + ", second=" + std::to_string(second.published_count)
            + ", handled=" + std::to_string(handled) + ")";
        close_db();
        return result;
    }

    result.passed = true;
    result.message = "replay cursor is robust across restarts and phase-3 placeholder authoring/state rows are present";
    close_db();
    return result;
}

ValidationResult ValidatePhase3PerServiceDedupeIsolation() {
    using namespace simcore::runner::parallel::simcoredb;

    ValidationResult result{ .name = "phase3.per_service_dedupe_isolation" };
    WorkflowCoordinatorBridge service_a;
    WorkflowCoordinatorBridge service_b;
    int service_a_seen = 0;
    int service_b_seen = 0;
    service_a.SetTerminalCallback([&](const TerminalJobSetSignal&) { ++service_a_seen; });
    service_b.SetTerminalCallback([&](const TerminalJobSetSignal&) { ++service_b_seen; });

    const TerminalJobSetSignal signal{
        .workflow_instance_id = 77,
        .workflow_step_id = 88,
        .job_set_id = 99,
        .terminal_state = "COMPLETED",
    };
    const bool a_first = service_a.NotifyTerminal(signal);
    const bool a_dup = service_a.NotifyTerminal(signal);
    const bool b_first = service_b.NotifyTerminal(signal);
    const bool b_dup = service_b.NotifyTerminal(signal);
    if (!a_first || a_dup || !b_first || b_dup) {
        result.message = "dedupe expectations failed across bridge instances";
        return result;
    }
    if (service_a_seen != 1 || service_b_seen != 1) {
        result.message = "terminal callbacks must fire once per service-local dedupe table";
        return result;
    }

    result.passed = true;
    result.message = "dedupe isolation verified: each service instance dedupes independently";
    return result;
}

ValidationResult ValidatePhase3ProgressTerminalStreamSeparation(const std::filesystem::path& migration_root) {
    using namespace simcore::db;
    using namespace simcore::db::execution::workflow;
    using namespace simcore::db::migrations;

    ValidationResult result{ .name = "phase3.progress_terminal_stream_separation" };
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        result.message = "failed to open sqlite memory db";
        if (db != nullptr) sqlite3_close(db);
        return result;
    }
    auto close_db = [&]() { if (db != nullptr) sqlite3_close(db); db = nullptr; };

    std::string err;
    const MigrationSourceOptions options{ .source_kind = MigrationSourceKind::Filesystem, .filesystem_root = migration_root };
    if (!ApplyContextMigrations(db, MigrationContext::Execution, options, &err)) {
        result.message = "failed applying execution migrations: " + err;
        close_db();
        return result;
    }
    if (!ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(3801, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'validation', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES(3802, 3801, 'Neutral', 'seedprobe.neutral', 'READY', 0, 2, unixepoch()*1000, unixepoch()*1000);
)SQL", &err)) {
        result.message = "seed setup failed: " + err;
        close_db();
        return result;
    }

    SqliteExecutionDb execution_db(db);
    auto* commands = execution_db.WorkflowCommandService();
    if (commands == nullptr) {
        result.message = "workflow command service unavailable";
        close_db();
        return result;
    }

    for (int i = 0; i < 100; ++i) {
        if (!commands->AppendStepInputEvent({
                .workflow_instance_id = 3801,
                .workflow_step_id = 3802,
                .event_kind = "Execution.WorkflowStepInputFragmentReady.v1",
                .source_key = std::optional<std::string>("progress"),
                .request_id = std::optional<std::string>("frag-" + std::to_string(i)),
                .message = std::optional<std::string>("progress"),
                .requested_by = "SimCoreDBValidation",
            }, &err)) {
            result.message = "failed appending progress fragments: " + err;
            close_db();
            return result;
        }
    }
    if (!commands->MarkStepTerminal({ .workflow_step_id = 3802, .terminal_state = "COMPLETED", .requested_by = "SimCoreDBValidation" }, &err)) {
        result.message = "failed writing terminal event: " + err;
        close_db();
        return result;
    }

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM exec_workflow_input_event WHERE workflow_step_id=3802;", -1, &st, nullptr) != SQLITE_OK
        || sqlite3_step(st) != SQLITE_ROW) {
        if (st != nullptr) sqlite3_finalize(st);
        result.message = "failed counting progress stream records";
        close_db();
        return result;
    }
    const int input_count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_step_id=3802;", -1, &st, nullptr) != SQLITE_OK
        || sqlite3_step(st) != SQLITE_ROW) {
        if (st != nullptr) sqlite3_finalize(st);
        result.message = "failed counting terminal stream records";
        close_db();
        return result;
    }
    const int terminal_count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    if (input_count < 100 || terminal_count != 1) {
        result.message = "expected high-volume input stream and one terminal stream record";
        close_db();
        return result;
    }

    result.passed = true;
    result.message = "progress input events and terminal events remain separated under high-frequency traffic";
    close_db();
    return result;
}

ValidationResult ValidatePhase3LagDeadLetterReadiness(const std::filesystem::path& migration_root) {
    using namespace simcore::db;
    using namespace simcore::db::events;
    using namespace simcore::db::migrations;
    using namespace simcore::db::retention;

    ValidationResult result{ .name = "phase3.lag_dead_letter_readiness" };
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        result.message = "failed to open sqlite memory db";
        if (db != nullptr) sqlite3_close(db);
        return result;
    }
    auto close_db = [&]() { if (db != nullptr) sqlite3_close(db); db = nullptr; };

    std::string err;
    const MigrationSourceOptions options{ .source_kind = MigrationSourceKind::Filesystem, .filesystem_root = migration_root };
    if (!ApplyContextMigrations(db, MigrationContext::Execution, options, &err)) {
        result.message = "failed applying execution migrations: " + err;
        close_db();
        return result;
    }
    if (!ExecSql(db, R"SQL(
INSERT INTO exec_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id
) VALUES
    (3951,'evt-p3-lag','Execution.WorkflowStepReady.v1',1,'Execution','workflow_step','1','corr','cause',unixepoch()*1000,'workflow_event',1);
)SQL", &err)) {
        result.message = "failed seeding outbox relay row: " + err;
        close_db();
        return result;
    }

    OutboxRelay relay({
        .db = db,
        .outbox_table = "exec_outbox_message",
        .context_name = "Execution",
        .max_attempts = 1,
    });
    std::vector<OutboxRelayDispatchBinding> bindings{ {
        .key = { .event_type = "Execution.WorkflowStepReady.v1", .event_version = 1 },
        .handler = [&](const EventEnvelope&, std::string* handler_error) {
            if (handler_error) *handler_error = "intentional-failure";
            return false;
        },
    } };
    OutboxRelayResult relay_result{};
    if (!relay.RelayBatchFromCursor(0, 10, bindings, &relay_result, &err)) {
        result.message = "relay batch failed: " + err;
        close_db();
        return result;
    }
    if (relay_result.dead_lettered_count != 1) {
        result.message = "expected one dead-lettered row";
        close_db();
        return result;
    }

    simcore::db::execution::workflow::SqliteExecutionDb execution_db(db);
    const auto preview = execution_db.PreviewOutboxRetention(
        std::vector<OutboxSubscriptionSnapshot>{
            OutboxSubscriptionSnapshot{
                .projector_name = "phase3-validation-subscriber",
                .last_outbox_id = 0,
                .updated_at_utc = simcore::db::types::UtcTimePoint::clock::now(),
                .status = "ACTIVE",
            },
        },
        simcore::db::types::UtcTimePoint::clock::now(),
        OutboxRetentionPolicy{});
    if (preview.lag_per_subscription.empty() || preview.lag_per_subscription.front().lag_outbox_rows < 1) {
        result.message = "lag readiness expected at least one lagging outbox row";
        close_db();
        return result;
    }

    result.passed = true;
    result.message = "dead-letter and lag readiness checks passed";
    close_db();
    return result;
}
