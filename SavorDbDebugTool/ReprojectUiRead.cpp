#include "ReprojectUiRead.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#include <sqlite3.h>

#include "Common/Migrations/MigrationRunner.h"
#include "UIRead/Projectors/UiReadProjectionService.h"

namespace savor::debugtool {
namespace {

struct Statement {
    sqlite3_stmt* st = nullptr;
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }
};

struct DbHandle {
    sqlite3* db = nullptr;
    ~DbHandle() {
        if (db != nullptr) {
            sqlite3_close(db);
        }
    }
};

bool Exec(sqlite3* db, const char* sql, std::string* error_out) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc == SQLITE_OK) {
        return true;
    }
    if (error_out != nullptr) {
        *error_out = err != nullptr ? err : sqlite3_errmsg(db);
    }
    sqlite3_free(err);
    return false;
}

bool Prepare(sqlite3* db, const char* sql, Statement* stmt, std::string* error_out) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt->st, nullptr) == SQLITE_OK) {
        return true;
    }
    if (error_out != nullptr) {
        *error_out = sqlite3_errmsg(db);
    }
    return false;
}

bool StepDone(sqlite3* db, sqlite3_stmt* st, std::string* error_out) {
    if (sqlite3_step(st) == SQLITE_DONE) {
        return true;
    }
    if (error_out != nullptr) {
        *error_out = sqlite3_errmsg(db);
    }
    return false;
}

std::int64_t UtcNowMillis() {
    const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    return now.time_since_epoch().count();
}

std::filesystem::path Canonicalish(const std::filesystem::path& path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? std::filesystem::absolute(path) : canonical;
}

bool OpenDb(const std::filesystem::path& path, bool readonly, DbHandle* handle, std::string* error_out) {
    if (handle == nullptr) {
        return false;
    }
    const int flags = (readonly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path.string().c_str(), &handle->db, flags, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = handle->db != nullptr ? sqlite3_errmsg(handle->db) : "sqlite3_open_v2 failed";
        }
        return false;
    }
    return Exec(handle->db, "PRAGMA busy_timeout=5000;", error_out)
        && Exec(handle->db, "PRAGMA foreign_keys=ON;", error_out);
}

std::int64_t ScalarInt64(sqlite3* db, const std::string& sql, std::string* error_out) {
    Statement st;
    if (!Prepare(db, sql.c_str(), &st, error_out)) {
        return 0;
    }
    if (sqlite3_step(st.st) == SQLITE_ROW && sqlite3_column_type(st.st, 0) != SQLITE_NULL) {
        return sqlite3_column_int64(st.st, 0);
    }
    return 0;
}

bool IsCompleteMigrationRoot(const std::filesystem::path& root) {
    const std::vector<std::string_view> contexts{
        "Execution",
        "State",
        "AnalysisSpine",
        "AnalysisSeedProbe",
        "AnalysisBattle",
        "Authoring",
        "UIRead",
        "Archive",
    };
    if (!std::filesystem::is_directory(root)) {
        return false;
    }
    for (const auto context : contexts) {
        if (!std::filesystem::is_directory(root / context)) {
            return false;
        }
    }
    return true;
}

bool ResolveMigrationRoot(const std::filesystem::path& explicit_root, std::filesystem::path* root_out, std::string* error_out) {
    if (root_out == nullptr) {
        return false;
    }
    if (!explicit_root.empty()) {
        const auto root = Canonicalish(explicit_root);
        if (!IsCompleteMigrationRoot(root)) {
            if (error_out != nullptr) {
                *error_out = "migration root is incomplete: " + root.string();
            }
            return false;
        }
        *root_out = root;
        return true;
    }

    const std::vector<std::filesystem::path> candidates{
        std::filesystem::path("SavorDb") / "migration",
        std::filesystem::path("..") / "SavorDb" / "migration",
        std::filesystem::path("..") / ".." / "SavorDb" / "migration",
        std::filesystem::path("..") / ".." / ".." / "SavorDb" / "migration",
        std::filesystem::path("migration"),
    };
    for (const auto& candidate : candidates) {
        const auto root = Canonicalish(candidate);
        if (IsCompleteMigrationRoot(root)) {
            *root_out = root;
            return true;
        }
    }
    if (error_out != nullptr) {
        *error_out = "could not locate SavorDb migration root; pass --migration <path-to-SavorDb\\migration>";
    }
    return false;
}

std::vector<std::string> SplitCommaList(const std::string& value) {
    std::vector<std::string> parts;
    std::stringstream ss(value);
    std::string part;
    while (std::getline(ss, part, ',')) {
        part.erase(part.begin(), std::find_if(part.begin(), part.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        part.erase(std::find_if(part.rbegin(), part.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), part.end());
        if (!part.empty()) {
            parts.push_back(std::move(part));
        }
    }
    return parts;
}

bool NormalizeStreams(const std::vector<std::string>& raw, std::vector<std::string>* stream_ids_out, std::string* error_out) {
    if (stream_ids_out == nullptr) {
        return false;
    }
    auto all = AllUiReadProjectionStreamIds();
    if (raw.empty()) {
        *stream_ids_out = std::move(all);
        return true;
    }
    if (std::find(raw.begin(), raw.end(), "all") != raw.end()) {
        *stream_ids_out = std::move(all);
        return true;
    }
    std::vector<std::string> normalized;
    for (const auto& stream : raw) {
        if (std::find(all.begin(), all.end(), stream) == all.end()) {
            if (error_out != nullptr) {
                *error_out = "unknown stream: " + stream;
            }
            return false;
        }
        if (std::find(normalized.begin(), normalized.end(), stream) == normalized.end()) {
            normalized.push_back(stream);
        }
    }
    *stream_ids_out = std::move(normalized);
    return true;
}

bool HasStream(const std::vector<std::string>& ids, std::string_view id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

savor::db::DbConfigPaths DbPathsForRoot(const std::filesystem::path& root) {
    savor::db::DbConfigPaths paths{};
    paths.execution_db_path = root / "execution.db";
    paths.state_db_path = root / "state.db";
    paths.analysis_db_path = root / "analysis.db";
    paths.authoring_db_path = root / "authoring.db";
    paths.ui_read_db_path = root / "ui_read.db";
    paths.archive_db_path = root / "archive.db";
    paths.object_store_root = root / "object_store";
    paths.archive_store_root = root / "archive_store";
    return paths;
}

std::filesystem::path SourcePathForStream(const savor::db::DbConfigPaths& paths, std::string_view stream_id) {
    if (stream_id == "execution") return paths.execution_db_path;
    if (stream_id == "state") return paths.state_db_path;
    if (stream_id == "archive") return paths.archive_db_path;
    return paths.analysis_db_path;
}

std::optional<savor::db::migrations::MigrationContext> SourceMigrationContextForStream(std::string_view stream_id) {
    namespace migrations = savor::db::migrations;
    if (stream_id == "execution") return migrations::MigrationContext::Execution;
    if (stream_id == "state") return migrations::MigrationContext::State;
    if (stream_id == "analysis-seedprobe") return migrations::MigrationContext::AnalysisSeedProbe;
    if (stream_id == "analysis-battle") return migrations::MigrationContext::AnalysisBattle;
    if (stream_id == "archive") return migrations::MigrationContext::Archive;
    return std::nullopt;
}

StreamReprojectPlan MakeStreamPlan(std::string stream_id, const savor::db::DbConfigPaths& paths) {
    StreamReprojectPlan plan{};
    plan.stream_id = std::move(stream_id);
    plan.source_db_path = SourcePathForStream(paths, plan.stream_id);

    if (plan.stream_id == "execution") {
        plan.source_context = "Execution";
        plan.source_outbox_table = "exec_outbox_message";
        plan.ui_tables_to_clear = {
            "ui_workflow_alert",
            "ui_workflow_edge",
            "ui_workflow_step",
            "ui_workflow_unit_activation_edge",
            "ui_workflow_unit_activation",
            "ui_workflow_instance",
            "ui_job_artifact",
            "ui_job_detail",
            "ui_job_summary",
        };
        plan.dirty_seed_plans = {
            { "workflow", "SELECT workflow_instance_id FROM exec_workflow_instance ORDER BY workflow_instance_id", 0 },
            { "job", "SELECT job_id FROM exec_job ORDER BY job_id", 0 },
        };
    } else if (plan.stream_id == "state") {
        plan.source_context = "State";
        plan.source_outbox_table = "state_outbox_message";
        plan.ui_tables_to_clear = { "ui_artifact_browser" };
        plan.dirty_seed_plans = {
            { "artifact", "SELECT artifact_id FROM state_artifact ORDER BY artifact_id", 0 },
        };
    } else if (plan.stream_id == "analysis-seedprobe") {
        plan.source_context = "AnalysisSeedProbe";
        plan.source_outbox_table = "sp_outbox_message";
        plan.ui_tables_to_clear = {
            "ui_seed_probe_unique_value",
            "ui_seed_probe_delta_point",
            "ui_seed_probe_summary",
        };
        plan.dirty_seed_plans = {
            { "seed_probe_run", "SELECT probe_run_id FROM sp_probe_run ORDER BY probe_run_id", 0 },
        };
    } else if (plan.stream_id == "analysis-battle") {
        plan.source_context = "AnalysisBattle";
        plan.source_outbox_table = "ab_outbox_message";
        plan.ui_tables_to_clear = {
            "ui_battle_manual_followup",
            "ui_battle_advancement_decision",
            "ui_battle_turn_job_replication",
            "ui_battle_turn_job",
            "ui_battle_wave",
            "ui_battle_group",
        };
        plan.dirty_seed_plans = {
            { "battle_group", "SELECT battle_set_id FROM ab_battle_set ORDER BY battle_set_id", 0 },
        };
    } else if (plan.stream_id == "archive") {
        plan.source_context = "Archive";
        plan.source_outbox_table = "ar_outbox_message";
        plan.ui_tables_to_clear = {
            "ui_archive_rehydrate_request",
            "ui_archive_catalog",
        };
        plan.dirty_seed_plans = {
            { "archive_package", "SELECT archive_package_id FROM ar_archive_package ORDER BY archive_package_id", 0 },
        };
    }
    return plan;
}

bool PopulateSourceCounts(StreamReprojectPlan* stream, std::string* error_out) {
    if (stream == nullptr) {
        return false;
    }
    DbHandle source;
    if (!OpenDb(stream->source_db_path, true, &source, error_out)) {
        if (error_out != nullptr) {
            *error_out = "failed opening source DB for stream " + stream->stream_id + ": " + *error_out;
        }
        return false;
    }
    stream->source_high_water_outbox_id = ScalarInt64(
        source.db,
        "SELECT COALESCE(MAX(outbox_id),0) FROM " + stream->source_outbox_table + ";",
        error_out);

    stream->expected_dirty_rows = 0;
    for (auto& seed : stream->dirty_seed_plans) {
        seed.count = ScalarInt64(source.db, "SELECT COUNT(1) FROM (" + seed.source_sql + ");", error_out);
        stream->expected_dirty_rows += seed.count;
    }
    return true;
}

bool BackupDatabase(const std::filesystem::path& source_path, std::filesystem::path* backup_path_out, std::string* error_out) {
    if (backup_path_out == nullptr) {
        return false;
    }
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto backup_path = source_path;
    backup_path += ".reproject-backup-" + std::to_string(stamp);

    DbHandle source;
    if (!OpenDb(source_path, true, &source, error_out)) {
        if (error_out != nullptr) {
            *error_out = "failed opening source ui_read backup DB: " + *error_out;
        }
        return false;
    }
    DbHandle dest;
    if (!OpenDb(backup_path, false, &dest, error_out)) {
        if (error_out != nullptr) {
            *error_out = "failed opening destination backup DB: " + *error_out;
        }
        return false;
    }
    sqlite3_backup* backup = sqlite3_backup_init(dest.db, "main", source.db, "main");
    if (backup == nullptr) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(dest.db);
        }
        return false;
    }
    const int step_rc = sqlite3_backup_step(backup, -1);
    const int finish_rc = sqlite3_backup_finish(backup);
    if (step_rc != SQLITE_DONE || finish_rc != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(dest.db);
        }
        return false;
    }
    *backup_path_out = backup_path;
    return true;
}

bool DeleteTableRows(sqlite3* db, const std::string& table, std::string* error_out) {
    const auto sql = "DELETE FROM " + table + ";";
    return Exec(db, sql.c_str(), error_out);
}

bool DeleteWhereStream(sqlite3* db, const char* table, const std::string& stream_id, std::string* error_out) {
    Statement st;
    const std::string sql = std::string("DELETE FROM ") + table + " WHERE stream_id=?1;";
    if (!Prepare(db, sql.c_str(), &st, error_out)) {
        return false;
    }
    sqlite3_bind_text(st.st, 1, stream_id.c_str(), -1, SQLITE_TRANSIENT);
    return StepDone(db, st.st, error_out);
}

bool DeleteAuditRows(sqlite3* db, const StreamReprojectPlan& stream, std::string* error_out) {
    Statement st;
    constexpr const char* kSql =
        "DELETE FROM ui_projection_subscription_audit WHERE source_context=?1 AND source_outbox_table=?2;";
    if (!Prepare(db, kSql, &st, error_out)) {
        return false;
    }
    sqlite3_bind_text(st.st, 1, stream.source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 2, stream.source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    return StepDone(db, st.st, error_out);
}

bool UpsertSubscriptionAtHighWater(sqlite3* db, const StreamReprojectPlan& stream, std::string* error_out) {
    Statement st;
    constexpr const char* kSql =
        "INSERT INTO ui_projection_subscription("
        "projector_name,source_context,source_outbox_table,last_outbox_id,last_event_id,updated_at_utc,status,last_error,"
        "stream_id,source_high_water_outbox_id,lag_count,lag_age_ms,last_batch_size,last_run_duration_ms,consecutive_failures,dead_letter_count) "
        "VALUES('UiReadProjector',?1,?2,?3,NULL,?4,'ACTIVE',NULL,?5,?3,0,0,0,0,0,0) "
        "ON CONFLICT(projector_name,source_context,source_outbox_table) DO UPDATE SET "
        "last_outbox_id=excluded.last_outbox_id,last_event_id=NULL,updated_at_utc=excluded.updated_at_utc,status='ACTIVE',last_error=NULL,"
        "stream_id=excluded.stream_id,source_high_water_outbox_id=excluded.source_high_water_outbox_id,lag_count=0,lag_age_ms=0,"
        "last_batch_size=0,last_run_duration_ms=0,consecutive_failures=0,dead_letter_count=0;";
    if (!Prepare(db, kSql, &st, error_out)) {
        return false;
    }
    sqlite3_bind_text(st.st, 1, stream.source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 2, stream.source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 3, stream.source_high_water_outbox_id);
    sqlite3_bind_int64(st.st, 4, UtcNowMillis());
    sqlite3_bind_text(st.st, 5, stream.stream_id.c_str(), -1, SQLITE_TRANSIENT);
    return StepDone(db, st.st, error_out);
}

bool InsertDirtyRow(sqlite3* db, const StreamReprojectPlan& stream, const std::string& entity_kind, std::int64_t entity_id, std::string* error_out) {
    Statement st;
    constexpr const char* kSql =
        "INSERT INTO ui_projection_dirty_entity("
        "stream_id,source_context,source_outbox_table,entity_kind,entity_id,first_outbox_id,last_outbox_id,event_count,updated_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6,?6,1,?7) "
        "ON CONFLICT(stream_id,entity_kind,entity_id) DO UPDATE SET "
        "first_outbox_id=MIN(first_outbox_id,excluded.first_outbox_id),last_outbox_id=MAX(last_outbox_id,excluded.last_outbox_id),"
        "event_count=event_count+1,updated_at_utc=excluded.updated_at_utc;";
    if (!Prepare(db, kSql, &st, error_out)) {
        return false;
    }
    sqlite3_bind_text(st.st, 1, stream.stream_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 2, stream.source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 3, stream.source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 4, entity_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 5, entity_id);
    sqlite3_bind_int64(st.st, 6, stream.source_high_water_outbox_id);
    sqlite3_bind_int64(st.st, 7, UtcNowMillis());
    return StepDone(db, st.st, error_out);
}

bool SeedDirtyEntities(sqlite3* ui_db, sqlite3* source_db, const StreamReprojectPlan& stream, std::string* error_out) {
    for (const auto& seed : stream.dirty_seed_plans) {
        Statement source;
        if (!Prepare(source_db, seed.source_sql.c_str(), &source, error_out)) {
            return false;
        }
        while (sqlite3_step(source.st) == SQLITE_ROW) {
            if (!InsertDirtyRow(ui_db, stream, seed.entity_kind, sqlite3_column_int64(source.st, 0), error_out)) {
                return false;
            }
        }
    }
    return true;
}

bool PrepareUiReadForStream(sqlite3* ui_db, const StreamReprojectPlan& stream, std::string* error_out) {
    for (const auto& table : stream.ui_tables_to_clear) {
        if (!DeleteTableRows(ui_db, table, error_out)) {
            return false;
        }
    }
    return DeleteWhereStream(ui_db, "ui_projection_dirty_entity", stream.stream_id, error_out)
        && DeleteWhereStream(ui_db, "ui_projection_dead_letter", stream.stream_id, error_out)
        && DeleteAuditRows(ui_db, stream, error_out)
        && UpsertSubscriptionAtHighWater(ui_db, stream, error_out);
}

bool SeedAllDirtyEntities(sqlite3* ui_db, const ReprojectUiReadPlan& plan, std::string* error_out) {
    for (const auto& stream : plan.streams) {
        DbHandle source;
        if (!OpenDb(stream.source_db_path, true, &source, error_out)) {
            return false;
        }
        if (!SeedDirtyEntities(ui_db, source.db, stream, error_out)) {
            return false;
        }
    }
    return true;
}

bool ApplySelectedSourceMigrations(
    const ReprojectUiReadPlan& plan,
    const savor::db::migrations::MigrationSourceOptions& migration_options,
    std::string* error_out) {
    std::set<std::pair<std::filesystem::path, savor::db::migrations::MigrationContext>> applied;
    for (const auto& stream : plan.streams) {
        const auto context = SourceMigrationContextForStream(stream.stream_id);
        if (!context.has_value()) {
            continue;
        }
        const auto key = std::make_pair(stream.source_db_path, *context);
        if (applied.count(key) != 0) {
            continue;
        }
        DbHandle source;
        if (!OpenDb(stream.source_db_path, false, &source, error_out)) {
            if (error_out != nullptr) {
                *error_out = "failed opening source DB for migrations for stream " + stream.stream_id + ": " + *error_out;
            }
            return false;
        }
        if (!savor::db::migrations::ApplyContextMigrations(source.db, *context, migration_options, error_out)) {
            if (error_out != nullptr) {
                *error_out = stream.stream_id + ": " + *error_out;
            }
            return false;
        }
        applied.insert(key);
    }
    return true;
}

bool IsSelectedStreamCaughtUp(const ReprojectUiReadPlan& plan, const savor::db::uiread::projectors::UiReadProjectionTelemetrySnapshot& snapshot) {
    for (const auto& stream : plan.streams) {
        const auto it = std::find_if(snapshot.streams.begin(), snapshot.streams.end(), [&](const auto& row) {
            return row.stream_id == stream.stream_id;
        });
        if (it == snapshot.streams.end() || it->lag_count != 0 || it->dirty_count != 0 || !it->last_error.empty()) {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<std::string> AllUiReadProjectionStreamIds() {
    return {
        "execution",
        "state",
        "analysis-seedprobe",
        "analysis-battle",
        "archive",
    };
}

bool ParseOptions(int argc, char** argv, ReprojectUiReadOptions* options, std::string* error_out) {
    if (options == nullptr) {
        return false;
    }
    ReprojectUiReadOptions parsed{};
    std::vector<std::string> raw_streams;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](const char* name) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                if (error_out != nullptr) {
                    *error_out = std::string("missing value for ") + name;
                }
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        if (arg == "--help" || arg == "-h") {
            if (error_out != nullptr) {
                *error_out = "help requested";
            }
            return false;
        }
        if (arg == "--apply") {
            parsed.apply = true;
        } else if (arg == "--db-root") {
            auto value = require_value("--db-root");
            if (!value.has_value()) return false;
            parsed.db_root = *value;
        } else if (arg == "--migration") {
            auto value = require_value("--migration");
            if (!value.has_value()) return false;
            parsed.migration_root = *value;
        } else if (arg == "--streams") {
            auto value = require_value("--streams");
            if (!value.has_value()) return false;
            const auto parts = SplitCommaList(*value);
            raw_streams.insert(raw_streams.end(), parts.begin(), parts.end());
        } else if (arg == "--max-iterations") {
            auto value = require_value("--max-iterations");
            if (!value.has_value()) return false;
            parsed.max_iterations = std::max(1, std::atoi(value->c_str()));
        } else {
            if (error_out != nullptr) {
                *error_out = "unknown argument: " + arg;
            }
            return false;
        }
    }
    if (parsed.db_root.empty()) {
        if (error_out != nullptr) {
            *error_out = "--db-root is required";
        }
        return false;
    }
    if (!NormalizeStreams(raw_streams, &parsed.stream_ids, error_out)) {
        return false;
    }
    *options = std::move(parsed);
    return true;
}

bool BuildPlan(const ReprojectUiReadOptions& options, ReprojectUiReadPlan* plan_out, std::string* error_out) {
    if (plan_out == nullptr) {
        return false;
    }
    if (options.db_root.empty()) {
        if (error_out != nullptr) {
            *error_out = "db_root is required";
        }
        return false;
    }
    const auto db_root = Canonicalish(options.db_root);
    if (!std::filesystem::is_directory(db_root)) {
        if (error_out != nullptr) {
            *error_out = "db root is not a directory: " + db_root.string();
        }
        return false;
    }

    std::filesystem::path migration_root;
    if (!ResolveMigrationRoot(options.migration_root, &migration_root, error_out)) {
        return false;
    }

    ReprojectUiReadPlan plan{};
    plan.db_paths = DbPathsForRoot(db_root);
    plan.migration_root = std::move(migration_root);
    plan.apply = options.apply;
    plan.max_iterations = std::max(1, options.max_iterations);

    std::vector<std::string> stream_ids;
    if (!NormalizeStreams(options.stream_ids, &stream_ids, error_out)) {
        return false;
    }

    if (!std::filesystem::exists(plan.db_paths.ui_read_db_path)) {
        if (error_out != nullptr) {
            *error_out = "missing ui_read DB: " + plan.db_paths.ui_read_db_path.string();
        }
        return false;
    }

    std::set<std::filesystem::path> required_sources;
    for (const auto& stream_id : stream_ids) {
        auto stream = MakeStreamPlan(stream_id, plan.db_paths);
        required_sources.insert(stream.source_db_path);
        if (!std::filesystem::exists(stream.source_db_path)) {
            if (error_out != nullptr) {
                *error_out = "missing source DB for stream " + stream_id + ": " + stream.source_db_path.string();
            }
            return false;
        }
        if (!PopulateSourceCounts(&stream, error_out)) {
            return false;
        }
        plan.streams.push_back(std::move(stream));
    }

    *plan_out = std::move(plan);
    return true;
}

bool ExecutePlan(const ReprojectUiReadPlan& plan, ReprojectUiReadResult* result_out, std::string* error_out) {
    ReprojectUiReadResult result{};
    result.applied = plan.apply;
    result.streams = plan.streams;
    if (!plan.apply) {
        if (result_out != nullptr) {
            *result_out = std::move(result);
        }
        return true;
    }

    if (!BackupDatabase(plan.db_paths.ui_read_db_path, &result.backup_path, error_out)) {
        return false;
    }

    DbHandle ui;
    if (!OpenDb(plan.db_paths.ui_read_db_path, false, &ui, error_out)) {
        return false;
    }

    const savor::db::migrations::MigrationSourceOptions migration_options{
        .source_kind = savor::db::migrations::MigrationSourceKind::Filesystem,
        .filesystem_root = plan.migration_root,
    };
    if (!ApplySelectedSourceMigrations(plan, migration_options, error_out)) {
        return false;
    }
    if (!savor::db::migrations::ApplyContextMigrations(ui.db, savor::db::migrations::MigrationContext::UIRead, migration_options, error_out)) {
        return false;
    }

    if (!Exec(ui.db, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }
    for (const auto& stream : plan.streams) {
        if (!PrepareUiReadForStream(ui.db, stream, error_out)) {
            Exec(ui.db, "ROLLBACK;", nullptr);
            return false;
        }
    }
    if (!SeedAllDirtyEntities(ui.db, plan, error_out)) {
        Exec(ui.db, "ROLLBACK;", nullptr);
        return false;
    }
    if (!Exec(ui.db, "COMMIT;", error_out)) {
        Exec(ui.db, "ROLLBACK;", nullptr);
        return false;
    }

    ui.db = (sqlite3_close(ui.db) == SQLITE_OK) ? nullptr : ui.db;

    savor::db::uiread::projectors::UiReadProjectionService projector(
        savor::db::uiread::projectors::UiReadProjectionConfig{
            .ui_read_db_path = plan.db_paths.ui_read_db_path,
            .execution_db_path = plan.db_paths.execution_db_path,
            .state_db_path = plan.db_paths.state_db_path,
            .analysis_db_path = plan.db_paths.analysis_db_path,
            .archive_db_path = plan.db_paths.archive_db_path,
            .max_batch_size = 5000,
            .max_dirty_materialization_batch_size = 1000,
            .max_attempts = 5,
            .poll_interval = std::chrono::milliseconds{ 250 },
            .enabled_stream_ids = [&]() {
                std::vector<std::string> ids;
                for (const auto& stream : plan.streams) ids.push_back(stream.stream_id);
                return ids;
            }(),
        });

    for (int iteration = 1; iteration <= plan.max_iterations; ++iteration) {
        std::string run_error;
        if (!projector.RunOnce(&run_error)) {
            if (error_out != nullptr) {
                *error_out = run_error;
            }
            return false;
        }
        result.iterations = iteration;
        if (IsSelectedStreamCaughtUp(plan, projector.SnapshotTelemetry())) {
            if (result_out != nullptr) {
                *result_out = std::move(result);
            }
            return true;
        }
    }

    if (error_out != nullptr) {
        *error_out = "UIRead projection did not catch up within max iterations";
    }
    return false;
}

void PrintPlan(const ReprojectUiReadPlan& plan, std::ostream& out) {
    out << (plan.apply ? "Apply" : "Dry run") << " UIRead reprojection plan\n";
    out << "  ui_read: " << plan.db_paths.ui_read_db_path.string() << "\n";
    out << "  migration: " << plan.migration_root.string() << "\n";
    for (const auto& stream : plan.streams) {
        out << "  stream " << stream.stream_id
            << ": high_water=" << stream.source_high_water_outbox_id
            << " dirty_rows=" << stream.expected_dirty_rows
            << " source=" << stream.source_db_path.string() << "\n";
        out << "    clear:";
        for (const auto& table : stream.ui_tables_to_clear) {
            out << " " << table;
        }
        out << "\n";
    }
}

void PrintResult(const ReprojectUiReadResult& result, std::ostream& out) {
    if (!result.applied) {
        out << "Dry run complete; no database changes were made.\n";
        return;
    }
    out << "UIRead reprojection applied.\n";
    out << "  backup: " << result.backup_path.string() << "\n";
    out << "  iterations: " << result.iterations << "\n";
}

void PrintUsage(std::ostream& out) {
    out << "SavorDbDebugTool reproject-ui-read --db-root <path> [--apply]\n"
        << "  [--streams all|execution,state,analysis-seedprobe,analysis-battle,archive]\n"
        << "  [--migration <path-to-SavorDb\\migration>] [--max-iterations N]\n";
}

} // namespace savor::debugtool
