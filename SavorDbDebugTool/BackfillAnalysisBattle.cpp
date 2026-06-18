#include "BackfillAnalysisBattle.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#include <sqlite3.h>

#include "Common/Migrations/MigrationRunner.h"
#include "Core/Input/SoaBattle/BattleCommandCodec.h"
#include "Runner/IPC/Wire.h"
#include "Utils/Hash.h"
#include "Utils/IniDoc.h"

namespace savor::debugtool {
namespace {

constexpr const char* kJobSection = "BattleSingleTurn.Job";

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

struct SourceRow {
    std::int64_t turn_job_id = 0;
    std::optional<std::int64_t> exec_job_id;
    std::int64_t plan_id = 0;
    std::optional<std::int64_t> source_savestate_id;
    std::optional<std::int64_t> seed_candidate_id;
    std::optional<std::int64_t> authored_plan_id;
    std::optional<int> authored_turn_index;
    std::optional<std::string> resolved_turn_commands_blob;
    std::optional<std::string> resolved_turn_variant_key;
    std::int64_t wave_seed_candidate_id = 0;
    int wave_turn_index = 0;
};

struct ExecJobRow {
    int program_kind = 0;
    std::optional<std::int64_t> savestate_id;
    std::string input_ini;
};

struct ParsedJobIni {
    std::int64_t savestate_id = 0;
    std::int64_t seed_candidate_id = 0;
    std::int64_t plan_id = 0;
    int turn_index = 0;
    std::string resolved_turn_commands_blob;
    std::string resolved_turn_variant_key;
};

struct BackfillValues {
    std::optional<std::int64_t> source_savestate_id;
    std::optional<std::int64_t> seed_candidate_id;
    std::optional<std::int64_t> authored_plan_id;
    std::optional<int> authored_turn_index;
    std::optional<std::string> resolved_turn_commands_blob;
    std::optional<std::string> resolved_turn_variant_key;
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

std::optional<std::int64_t> ColumnInt64Optional(sqlite3_stmt* st, int col) {
    if (sqlite3_column_type(st, col) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st, col);
}

std::optional<int> ColumnIntOptional(sqlite3_stmt* st, int col) {
    if (sqlite3_column_type(st, col) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int(st, col);
}

std::optional<std::string> ColumnTextOptional(sqlite3_stmt* st, int col) {
    if (sqlite3_column_type(st, col) == SQLITE_NULL) {
        return std::nullopt;
    }
    const auto* text = sqlite3_column_text(st, col);
    return text == nullptr ? std::optional<std::string>{} : std::string(reinterpret_cast<const char*>(text));
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

bool BackupDatabase(const std::filesystem::path& source_path, std::filesystem::path* backup_path_out, std::string* error_out) {
    if (backup_path_out == nullptr) {
        return false;
    }
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto backup_path = source_path;
    backup_path += ".analysis-battle-backfill-" + std::to_string(stamp);

    DbHandle source;
    if (!OpenDb(source_path, true, &source, error_out)) {
        if (error_out != nullptr) {
            *error_out = "failed opening source analysis backup DB: " + *error_out;
        }
        return false;
    }
    DbHandle dest;
    if (!OpenDb(backup_path, false, &dest, error_out)) {
        if (error_out != nullptr) {
            *error_out = "failed opening destination analysis backup DB: " + *error_out;
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
    const int rc = sqlite3_backup_step(backup, -1);
    const int finish_rc = sqlite3_backup_finish(backup);
    if (rc != SQLITE_DONE || finish_rc != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(dest.db);
        }
        return false;
    }
    *backup_path_out = std::move(backup_path);
    return true;
}

std::int64_t CountAlreadyComplete(sqlite3* analysis, std::string* error_out) {
    Statement st;
    constexpr const char* kSql =
        "SELECT COUNT(1) FROM ab_turn_job "
        "WHERE source_savestate_id IS NOT NULL "
        "AND seed_candidate_id IS NOT NULL "
        "AND authored_plan_id IS NOT NULL "
        "AND authored_turn_index IS NOT NULL "
        "AND resolved_turn_commands_blob IS NOT NULL "
        "AND resolved_turn_variant_key IS NOT NULL;";
    if (!Prepare(analysis, kSql, &st, error_out)) {
        return 0;
    }
    if (sqlite3_step(st.st) == SQLITE_ROW) {
        return sqlite3_column_int64(st.st, 0);
    }
    return 0;
}

bool HasColumn(sqlite3* db, std::string_view table, std::string_view column) {
    Statement st;
    const auto sql = "PRAGMA table_info(" + std::string(table) + ");";
    if (!Prepare(db, sql.c_str(), &st, nullptr)) {
        return false;
    }
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        const auto* text = sqlite3_column_text(st.st, 1);
        if (text != nullptr && column == reinterpret_cast<const char*>(text)) {
            return true;
        }
    }
    return false;
}

bool EnsureBackfillSchema(sqlite3* analysis, std::string* error_out) {
    const std::vector<std::string_view> required_columns{
        "source_savestate_id",
        "seed_candidate_id",
        "authored_plan_id",
        "authored_turn_index",
        "resolved_turn_commands_blob",
        "resolved_turn_variant_key",
    };
    for (const auto column : required_columns) {
        if (!HasColumn(analysis, "ab_turn_job", column)) {
            if (error_out != nullptr) {
                *error_out = "analysis.db is missing ab_turn_job." + std::string(column)
                    + "; run backfill-analysis-battle with --apply so migrations can be applied before backfill";
            }
            return false;
        }
    }
    return true;
}

bool LoadCandidateRows(sqlite3* analysis, std::vector<SourceRow>* rows_out, std::string* error_out) {
    if (rows_out == nullptr) {
        return false;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT j.turn_job_id,j.exec_job_id,j.plan_id,j.source_savestate_id,j.seed_candidate_id,"
        "j.authored_plan_id,j.authored_turn_index,j.resolved_turn_commands_blob,j.resolved_turn_variant_key,"
        "w.seed_candidate_id,w.turn_index "
        "FROM ab_turn_job j "
        "JOIN ab_turn_wave w ON w.wave_id=j.wave_id "
        "WHERE j.source_savestate_id IS NULL "
        "OR j.seed_candidate_id IS NULL "
        "OR j.authored_plan_id IS NULL "
        "OR j.authored_turn_index IS NULL "
        "OR j.resolved_turn_commands_blob IS NULL "
        "OR j.resolved_turn_variant_key IS NULL "
        "ORDER BY j.turn_job_id;";
    if (!Prepare(analysis, kSql, &st, error_out)) {
        return false;
    }
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        SourceRow row{};
        row.turn_job_id = sqlite3_column_int64(st.st, 0);
        row.exec_job_id = ColumnInt64Optional(st.st, 1);
        row.plan_id = sqlite3_column_int64(st.st, 2);
        row.source_savestate_id = ColumnInt64Optional(st.st, 3);
        row.seed_candidate_id = ColumnInt64Optional(st.st, 4);
        row.authored_plan_id = ColumnInt64Optional(st.st, 5);
        row.authored_turn_index = ColumnIntOptional(st.st, 6);
        row.resolved_turn_commands_blob = ColumnTextOptional(st.st, 7);
        row.resolved_turn_variant_key = ColumnTextOptional(st.st, 8);
        row.wave_seed_candidate_id = sqlite3_column_int64(st.st, 9);
        row.wave_turn_index = sqlite3_column_int(st.st, 10);
        rows_out->push_back(std::move(row));
    }
    return sqlite3_errcode(analysis) == SQLITE_ROW || sqlite3_errcode(analysis) == SQLITE_DONE || sqlite3_errcode(analysis) == SQLITE_OK;
}

std::optional<ExecJobRow> LoadExecJob(sqlite3* execution, std::int64_t exec_job_id, std::string* error_out) {
    Statement st;
    constexpr const char* kSql =
        "SELECT program_kind,savestate_id,input_ini FROM exec_job WHERE job_id=?1;";
    if (!Prepare(execution, kSql, &st, error_out)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, exec_job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    ExecJobRow row{};
    row.program_kind = sqlite3_column_int(st.st, 0);
    row.savestate_id = ColumnInt64Optional(st.st, 1);
    if (const auto text = ColumnTextOptional(st.st, 2); text.has_value()) {
        row.input_ini = *text;
    }
    return row;
}

std::optional<ParsedJobIni> ParseBattleSingleTurnJobIni(const std::string& input_ini) {
    if (input_ini.find("[BattleSingleTurn.Job]") == std::string::npos) {
        return std::nullopt;
    }
    const auto ini = IniDoc::parse(input_ini);
    ParsedJobIni parsed{};
    parsed.savestate_id = ini.get_i64(kJobSection, "savestate_id", 0);
    parsed.seed_candidate_id = ini.get_i64(kJobSection, "seed_candidate_id", 0);
    parsed.plan_id = ini.get_i64(kJobSection, "plan_id", 0);
    parsed.turn_index = static_cast<int>(ini.get_i64(kJobSection, "turn_index", 0));
    parsed.resolved_turn_commands_blob = ini.get(kJobSection, "resolved_turn_commands_blob", "");
    if (parsed.resolved_turn_commands_blob.empty()) {
        parsed.resolved_turn_commands_blob = ini.get(kJobSection, "concrete_turn_plan_hex", "");
    }
    parsed.resolved_turn_variant_key = ini.get(kJobSection, "resolved_turn_variant_key", "");
    if (parsed.resolved_turn_variant_key.empty()) {
        parsed.resolved_turn_variant_key = ini.get(kJobSection, "target_variant_key", "");
    }
    return parsed;
}

BackfillValues BuildBackfillValues(const SourceRow& source, const ExecJobRow& exec, const ParsedJobIni& parsed, bool* invalid_command_blob) {
    if (invalid_command_blob != nullptr) {
        *invalid_command_blob = false;
    }
    BackfillValues values{};
    if (!source.source_savestate_id.has_value()) {
        if (parsed.savestate_id > 0) {
            values.source_savestate_id = parsed.savestate_id;
        } else if (exec.savestate_id.has_value() && *exec.savestate_id > 0) {
            values.source_savestate_id = *exec.savestate_id;
        }
    }
    if (!source.seed_candidate_id.has_value()) {
        if (parsed.seed_candidate_id > 0) {
            values.seed_candidate_id = parsed.seed_candidate_id;
        } else if (source.wave_seed_candidate_id > 0) {
            values.seed_candidate_id = source.wave_seed_candidate_id;
        }
    }
    if (!source.authored_plan_id.has_value()) {
        values.authored_plan_id = parsed.plan_id > 0 ? parsed.plan_id : source.plan_id;
    }
    if (!source.authored_turn_index.has_value()) {
        values.authored_turn_index = parsed.turn_index > 0 ? parsed.turn_index : source.wave_turn_index;
    }

    bool command_blob_valid = false;
    if (!source.resolved_turn_commands_blob.has_value() && !parsed.resolved_turn_commands_blob.empty()) {
        command_blob_valid = soa::battle::actions::decode_battle_turn_commands_hex(parsed.resolved_turn_commands_blob).has_value();
        if (command_blob_valid) {
            values.resolved_turn_commands_blob = parsed.resolved_turn_commands_blob;
        } else if (invalid_command_blob != nullptr) {
            *invalid_command_blob = true;
        }
    } else if (source.resolved_turn_commands_blob.has_value()) {
        command_blob_valid = true;
    }

    if (!source.resolved_turn_variant_key.has_value()) {
        if (!parsed.resolved_turn_variant_key.empty()) {
            values.resolved_turn_variant_key = parsed.resolved_turn_variant_key;
        } else if (command_blob_valid && !parsed.resolved_turn_commands_blob.empty()) {
            values.resolved_turn_variant_key = hash::sha256(parsed.resolved_turn_commands_blob.data(), parsed.resolved_turn_commands_blob.size());
        }
    }
    return values;
}

bool HasAnyValue(const BackfillValues& values) {
    return values.source_savestate_id.has_value()
        || values.seed_candidate_id.has_value()
        || values.authored_plan_id.has_value()
        || values.authored_turn_index.has_value()
        || values.resolved_turn_commands_blob.has_value()
        || values.resolved_turn_variant_key.has_value();
}

bool ApplyValues(sqlite3* analysis, std::int64_t turn_job_id, const BackfillValues& values, std::string* error_out) {
    Statement st;
    constexpr const char* kSql =
        "UPDATE ab_turn_job SET "
        "source_savestate_id=CASE WHEN source_savestate_id IS NULL THEN ?1 ELSE source_savestate_id END,"
        "seed_candidate_id=CASE WHEN seed_candidate_id IS NULL THEN ?2 ELSE seed_candidate_id END,"
        "authored_plan_id=CASE WHEN authored_plan_id IS NULL THEN ?3 ELSE authored_plan_id END,"
        "authored_turn_index=CASE WHEN authored_turn_index IS NULL THEN ?4 ELSE authored_turn_index END,"
        "resolved_turn_commands_blob=CASE WHEN resolved_turn_commands_blob IS NULL THEN ?5 ELSE resolved_turn_commands_blob END,"
        "resolved_turn_variant_key=CASE WHEN resolved_turn_variant_key IS NULL THEN ?6 ELSE resolved_turn_variant_key END "
        "WHERE turn_job_id=?7;";
    if (!Prepare(analysis, kSql, &st, error_out)) {
        return false;
    }
    if (values.source_savestate_id.has_value()) sqlite3_bind_int64(st.st, 1, *values.source_savestate_id); else sqlite3_bind_null(st.st, 1);
    if (values.seed_candidate_id.has_value()) sqlite3_bind_int64(st.st, 2, *values.seed_candidate_id); else sqlite3_bind_null(st.st, 2);
    if (values.authored_plan_id.has_value()) sqlite3_bind_int64(st.st, 3, *values.authored_plan_id); else sqlite3_bind_null(st.st, 3);
    if (values.authored_turn_index.has_value()) sqlite3_bind_int(st.st, 4, *values.authored_turn_index); else sqlite3_bind_null(st.st, 4);
    if (values.resolved_turn_commands_blob.has_value()) sqlite3_bind_text(st.st, 5, values.resolved_turn_commands_blob->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st.st, 5);
    if (values.resolved_turn_variant_key.has_value()) sqlite3_bind_text(st.st, 6, values.resolved_turn_variant_key->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(st.st, 6);
    sqlite3_bind_int64(st.st, 7, turn_job_id);
    return StepDone(analysis, st.st, error_out);
}

bool ApplyRequiredMigrations(const BackfillAnalysisBattlePlan& plan, std::string* error_out) {
    const savor::db::migrations::MigrationSourceOptions migration_options{
        .source_kind = savor::db::migrations::MigrationSourceKind::Filesystem,
        .filesystem_root = plan.migration_root,
    };
    {
        DbHandle analysis;
        if (!OpenDb(plan.db_paths.analysis_db_path, false, &analysis, error_out)) {
            return false;
        }
        if (!savor::db::migrations::ApplyContextMigrations(analysis.db, savor::db::migrations::MigrationContext::AnalysisBattle, migration_options, error_out)) {
            return false;
        }
    }
    {
        DbHandle execution;
        if (!OpenDb(plan.db_paths.execution_db_path, false, &execution, error_out)) {
            return false;
        }
        if (!savor::db::migrations::ApplyContextMigrations(execution.db, savor::db::migrations::MigrationContext::Execution, migration_options, error_out)) {
            return false;
        }
    }
    return true;
}

} // namespace

bool ParseBackfillAnalysisBattleOptions(int argc, char** argv, BackfillAnalysisBattleOptions* options, std::string* error_out) {
    if (options == nullptr) {
        return false;
    }
    BackfillAnalysisBattleOptions parsed{};
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
    *options = std::move(parsed);
    return true;
}

bool BuildBackfillAnalysisBattlePlan(const BackfillAnalysisBattleOptions& options, BackfillAnalysisBattlePlan* plan_out, std::string* error_out) {
    if (plan_out == nullptr) {
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

    BackfillAnalysisBattlePlan plan{};
    plan.db_paths = DbPathsForRoot(db_root);
    plan.migration_root = std::move(migration_root);
    plan.apply = options.apply;
    if (!std::filesystem::exists(plan.db_paths.analysis_db_path)) {
        if (error_out != nullptr) {
            *error_out = "missing analysis DB: " + plan.db_paths.analysis_db_path.string();
        }
        return false;
    }
    if (!std::filesystem::exists(plan.db_paths.execution_db_path)) {
        if (error_out != nullptr) {
            *error_out = "missing execution DB: " + plan.db_paths.execution_db_path.string();
        }
        return false;
    }
    *plan_out = std::move(plan);
    return true;
}

bool ExecuteBackfillAnalysisBattlePlan(const BackfillAnalysisBattlePlan& plan, BackfillAnalysisBattleResult* result_out, std::string* error_out) {
    BackfillAnalysisBattleResult result{};
    result.applied = plan.apply;

    if (plan.apply) {
        if (!BackupDatabase(plan.db_paths.analysis_db_path, &result.backup_path, error_out)) {
            return false;
        }
        if (!ApplyRequiredMigrations(plan, error_out)) {
            return false;
        }
    }

    DbHandle analysis;
    if (!OpenDb(plan.db_paths.analysis_db_path, !plan.apply, &analysis, error_out)) {
        return false;
    }
    DbHandle execution;
    if (!OpenDb(plan.db_paths.execution_db_path, true, &execution, error_out)) {
        return false;
    }
    if (!EnsureBackfillSchema(analysis.db, error_out)) {
        return false;
    }

    result.rows_already_complete = CountAlreadyComplete(analysis.db, error_out);

    std::vector<SourceRow> rows;
    if (!LoadCandidateRows(analysis.db, &rows, error_out)) {
        return false;
    }
    result.candidate_rows = static_cast<std::int64_t>(rows.size());

    if (plan.apply && !Exec(analysis.db, "BEGIN IMMEDIATE;", error_out)) {
        return false;
    }

    for (const auto& row : rows) {
        if (!row.exec_job_id.has_value() || *row.exec_job_id <= 0) {
            ++result.rows_missing_exec_job;
            continue;
        }
        const auto exec_job = LoadExecJob(execution.db, *row.exec_job_id, error_out);
        if (!exec_job.has_value()) {
            ++result.rows_missing_exec_job;
            continue;
        }
        if (exec_job->program_kind != savor::PK_BattleSingleTurnRunner) {
            ++result.rows_missing_or_invalid_ini;
            continue;
        }
        const auto parsed = ParseBattleSingleTurnJobIni(exec_job->input_ini);
        if (!parsed.has_value()) {
            ++result.rows_missing_or_invalid_ini;
            continue;
        }

        bool invalid_command_blob = false;
        const auto values = BuildBackfillValues(row, *exec_job, *parsed, &invalid_command_blob);
        if (invalid_command_blob) {
            ++result.rows_invalid_command_blob;
        }
        if (!HasAnyValue(values)) {
            continue;
        }
        ++result.rows_updated;
        if (plan.apply && !ApplyValues(analysis.db, row.turn_job_id, values, error_out)) {
            Exec(analysis.db, "ROLLBACK;", nullptr);
            return false;
        }
    }

    if (plan.apply && !Exec(analysis.db, "COMMIT;", error_out)) {
        Exec(analysis.db, "ROLLBACK;", nullptr);
        return false;
    }

    if (result_out != nullptr) {
        *result_out = std::move(result);
    }
    return true;
}

void PrintBackfillAnalysisBattlePlan(const BackfillAnalysisBattlePlan& plan, std::ostream& out) {
    out << (plan.apply ? "Apply" : "Dry run") << " AnalysisBattle job INI backfill plan\n";
    out << "  analysis: " << plan.db_paths.analysis_db_path.string() << "\n";
    out << "  execution: " << plan.db_paths.execution_db_path.string() << "\n";
    out << "  migration: " << plan.migration_root.string() << "\n";
}

void PrintBackfillAnalysisBattleResult(const BackfillAnalysisBattleResult& result, std::ostream& out) {
    out << (result.applied ? "AnalysisBattle backfill applied.\n" : "Dry run complete; no database changes were made.\n");
    if (result.applied) {
        out << "  backup: " << result.backup_path.string() << "\n";
    }
    out << "  candidate_rows: " << result.candidate_rows << "\n";
    out << "  rows_updated: " << result.rows_updated << "\n";
    out << "  rows_already_complete: " << result.rows_already_complete << "\n";
    out << "  rows_missing_exec_job: " << result.rows_missing_exec_job << "\n";
    out << "  rows_missing_or_invalid_ini: " << result.rows_missing_or_invalid_ini << "\n";
    out << "  rows_invalid_command_blob: " << result.rows_invalid_command_blob << "\n";
}

void PrintBackfillAnalysisBattleUsage(std::ostream& out) {
    out << "SavorDbDebugTool backfill-analysis-battle --db-root <path> [--apply]\n"
        << "  [--migration <path-to-SavorDb\\migration>]\n";
}

} // namespace savor::debugtool
