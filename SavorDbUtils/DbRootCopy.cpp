#include "DbRootCopy.h"

#include "Common/DbService.h"
#include "Common/Migrations/MigrationRunner.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <set>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace savor::dbutils {
namespace fs = std::filesystem;

namespace {

constexpr int kBattleSingleTurnProgramKind = 5;

class SqliteHandle {
public:
    SqliteHandle() = default;
    SqliteHandle(const SqliteHandle&) = delete;
    SqliteHandle& operator=(const SqliteHandle&) = delete;
    ~SqliteHandle() {
        if (db_ != nullptr) {
            sqlite3_close(db_);
        }
    }

    sqlite3** out() { return &db_; }
    sqlite3* get() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

struct Statement {
    sqlite3_stmt* st = nullptr;
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }
};

struct SourceExecJob {
    std::int64_t job_id = 0;
    std::int64_t job_set_id = 0;
    std::optional<std::int64_t> savestate_id;
    int program_kind = 0;
};

struct SourceTurnJob {
    std::int64_t turn_job_id = 0;
    std::optional<std::int64_t> exec_job_id;
    std::int64_t wave_id = 0;
    std::int64_t battle_set_id = 0;
    int turn_index = 0;
    std::int64_t plan_id = 0;
    std::optional<std::int64_t> source_savestate_id;
    std::optional<std::int64_t> seed_candidate_id;
    std::optional<std::int64_t> output_savestate_id;
};

struct SourceBattleSet {
    std::int64_t battle_set_id = 0;
    std::int64_t entry_savestate_id = 0;
    std::int64_t battle_run_spec_id = 0;
    std::int64_t explorer_settings_id = 0;
};

struct SourceWaveRefs {
    std::optional<std::int64_t> context_probe_id;
    std::optional<std::int64_t> parent_wave_id;
    std::optional<std::int64_t> parent_turn_job_id;
    std::optional<std::int64_t> seed_candidate_id;
    std::optional<std::int64_t> battle_advancement_pool_id;
};

struct SourceSeedCandidate {
    std::int64_t seed_candidate_id = 0;
    std::optional<std::int64_t> source_probe_result_id;
    std::optional<std::int64_t> source_input_frame_id;
};

struct SourceConfirmedProbeResult {
    std::int64_t probe_result_id = 0;
    std::int64_t probe_run_id = 0;
    std::int64_t input_frame_id = 0;
};

struct SourceExplorerSettings {
    std::int64_t explorer_settings_id = 0;
    std::optional<std::int64_t> default_plan_id;
    std::optional<std::int64_t> default_predicate_set_id;
};

struct ArtifactRecord {
    std::int64_t savestate_id = 0;
    std::int64_t artifact_id = 0;
    std::string sha256;
    std::string file_ext;
    fs::path source_path;
};

std::string sqlite_error(sqlite3* db) {
    return db == nullptr ? "sqlite error" : sqlite3_errmsg(db);
}

bool is_delete_safe(const fs::path& dest, const std::optional<fs::path>& source) {
    std::error_code ec;
    const auto absolute_dest = fs::weakly_canonical(fs::absolute(dest), ec);
    if (ec || absolute_dest.empty() || absolute_dest == absolute_dest.root_path()) {
        return false;
    }

    if (source.has_value()) {
        const auto absolute_source = fs::weakly_canonical(fs::absolute(*source), ec);
        if (!ec && absolute_dest == absolute_source) {
            return false;
        }
    }

    return absolute_dest.has_filename() && absolute_dest.parent_path() != absolute_dest;
}

bool remove_existing_if_allowed(
    const fs::path& dest,
    bool overwrite,
    const std::optional<fs::path>& source,
    std::ostream* out,
    std::ostream& err) {
    std::error_code ec;
    if (!fs::exists(dest, ec)) {
        return true;
    }
    if (!overwrite) {
        err << "Destination already exists: " << dest.string() << "\n";
        err << "Pass overwrite=true to replace it.\n";
        return false;
    }
    if (!is_delete_safe(dest, source)) {
        err << "Refusing to overwrite unsafe destination path: " << dest.string() << "\n";
        return false;
    }
    if (out != nullptr) {
        *out << "Removing existing destination " << dest.string() << "\n";
    }
    fs::remove_all(dest, ec);
    if (ec) {
        err << "Failed to remove destination: " << ec.message() << "\n";
        return false;
    }
    return true;
}

int copy_sqlite_db(const fs::path& source, const fs::path& dest, std::ostream& err) {
    SqliteHandle source_db;
    int rc = sqlite3_open_v2(source.string().c_str(), source_db.out(), SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        err << "Failed to open source sqlite database " << source.string() << ": " << sqlite_error(source_db.get()) << "\n";
        return 1;
    }

    SqliteHandle dest_db;
    rc = sqlite3_open_v2(dest.string().c_str(), dest_db.out(), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        err << "Failed to create destination sqlite database " << dest.string() << ": " << sqlite_error(dest_db.get()) << "\n";
        return 1;
    }

    sqlite3_backup* backup = sqlite3_backup_init(dest_db.get(), "main", source_db.get(), "main");
    if (backup == nullptr) {
        err << "Failed to initialize sqlite backup for " << source.string() << ": " << sqlite_error(dest_db.get()) << "\n";
        return 1;
    }

    do {
        rc = sqlite3_backup_step(backup, 256);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);

    const int finish_rc = sqlite3_backup_finish(backup);
    if (rc != SQLITE_DONE || finish_rc != SQLITE_OK) {
        err << "Sqlite backup failed for " << source.string() << ": step=" << rc << " finish=" << finish_rc << "\n";
        return 1;
    }

    return 0;
}

int copy_directory_if_present(const fs::path& source, const fs::path& dest, std::ostream& out, std::ostream& err) {
    std::error_code ec;
    if (!fs::exists(source, ec)) {
        return 0;
    }
    if (!fs::is_directory(source, ec)) {
        err << "Expected directory at " << source.string() << "\n";
        return 1;
    }

    out << "Copying " << source.filename().string() << "\n";
    fs::copy(source, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        err << "Failed to copy " << source.string() << " to " << dest.string() << ": " << ec.message() << "\n";
        return 1;
    }
    return 0;
}

bool open_sqlite(const fs::path& path, int flags, SqliteHandle* out, std::ostream& err) {
    if (out == nullptr) {
        err << "Internal error: null sqlite output.\n";
        return false;
    }
    if (sqlite3_open_v2(path.string().c_str(), out->out(), flags | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        err << "Failed to open sqlite database " << path.string() << ": " << sqlite_error(out->get()) << "\n";
        return false;
    }
    sqlite3_busy_timeout(out->get(), 5000);
    return true;
}

bool exec(sqlite3* db, const std::string& sql, std::ostream& err) {
    char* sqlite_error_text = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &sqlite_error_text) == SQLITE_OK) {
        return true;
    }
    err << (sqlite_error_text != nullptr ? sqlite_error_text : sqlite3_errmsg(db)) << "\n";
    sqlite3_free(sqlite_error_text);
    return false;
}

bool prepare(sqlite3* db, const char* sql, Statement* out, std::ostream& err) {
    if (sqlite3_prepare_v2(db, sql, -1, &out->st, nullptr) != SQLITE_OK) {
        err << sqlite3_errmsg(db) << "\n";
        return false;
    }
    return true;
}

std::optional<std::int64_t> column_i64_optional(sqlite3_stmt* st, int column) {
    if (sqlite3_column_type(st, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st, column);
}

std::string column_text(sqlite3_stmt* st, int column) {
    const auto* text = sqlite3_column_text(st, column);
    return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
}

std::optional<std::string> scalar_text(sqlite3* db, const char* sql, std::int64_t id, std::ostream& err) {
    Statement st;
    if (!prepare(db, sql, &st, err)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return column_text(st.st, 0);
}

std::optional<std::int64_t> scalar_i64(sqlite3* db, const char* sql, std::int64_t id, std::ostream& err) {
    Statement st;
    if (!prepare(db, sql, &st, err)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, id);
    if (sqlite3_step(st.st) != SQLITE_ROW || sqlite3_column_type(st.st, 0) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st.st, 0);
}

bool table_exists(sqlite3* db, const std::string& schema, const std::string& table, std::ostream& err) {
    const auto sql = "SELECT 1 FROM " + schema + ".sqlite_schema WHERE type='table' AND name=?1 LIMIT 1;";
    Statement st;
    if (!prepare(db, sql.c_str(), &st, err)) {
        return false;
    }
    sqlite3_bind_text(st.st, 1, table.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(st.st) == SQLITE_ROW;
}

std::vector<std::string> table_columns(sqlite3* db, const std::string& schema, const std::string& table, std::ostream& err) {
    std::vector<std::string> columns;
    const auto sql = "PRAGMA " + schema + ".table_info(" + table + ");";
    Statement st;
    if (!prepare(db, sql.c_str(), &st, err)) {
        return columns;
    }
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        columns.push_back(column_text(st.st, 1));
    }
    return columns;
}

bool column_exists(sqlite3* db, const std::string& schema, const std::string& table, const std::string& column, std::ostream& err) {
    const auto columns = table_columns(db, schema, table, err);
    return std::find(columns.begin(), columns.end(), column) != columns.end();
}

std::string quote_ident(const std::string& value) {
    std::string out = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            out += "\"\"";
        } else {
            out += ch;
        }
    }
    out += '"';
    return out;
}

std::string join_columns(const std::vector<std::string>& columns) {
    std::ostringstream out;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << quote_ident(columns[i]);
    }
    return out.str();
}

bool attach_source(sqlite3* dest, const fs::path& source_path, std::ostream& err) {
    Statement st;
    if (!prepare(dest, "ATTACH DATABASE ?1 AS src;", &st, err)) {
        return false;
    }
    sqlite3_bind_text(st.st, 1, source_path.string().c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        err << "Failed attaching source database " << source_path.string() << ": " << sqlite3_errmsg(dest) << "\n";
        return false;
    }
    return true;
}

std::vector<std::string> common_columns(sqlite3* db, const std::string& table, std::ostream& err) {
    const auto dest_columns = table_columns(db, "main", table, err);
    const auto source_columns = table_columns(db, "src", table, err);
    std::unordered_set<std::string> source_set(source_columns.begin(), source_columns.end());
    std::vector<std::string> out;
    for (const auto& column : dest_columns) {
        if (source_set.contains(column)) {
            out.push_back(column);
        }
    }
    return out;
}

bool copy_rows(
    sqlite3* db,
    const std::string& db_name,
    const std::string& table,
    const std::string& where_clause,
    BattleSingleTurnJobSubsetResult* result,
    std::ostream& err) {
    if (!table_exists(db, "main", table, err) || !table_exists(db, "src", table, err)) {
        return true;
    }
    const auto columns = common_columns(db, table, err);
    if (columns.empty()) {
        err << "No common columns for table " << table << "\n";
        return false;
    }
    const auto cols = join_columns(columns);
    const auto sql = "INSERT OR IGNORE INTO main." + quote_ident(table) + " (" + cols + ") "
        "SELECT " + cols + " FROM src." + quote_ident(table) + " " + where_clause + ";";
    if (!exec(db, sql, err)) {
        err << "Failed copying " << db_name << "." << table << "\n";
        return false;
    }
    const int changed = sqlite3_changes(db);
    if (result != nullptr && changed != 0) {
        result->table_counts.push_back({ db_name, table, changed });
    }
    return true;
}

bool copy_rows_by_ids(
    sqlite3* db,
    const std::string& db_name,
    const std::string& table,
    const std::string& id_column,
    const std::set<std::int64_t>& ids,
    BattleSingleTurnJobSubsetResult* result,
    std::ostream& err) {
    if (ids.empty()) {
        return true;
    }
    std::ostringstream list;
    bool first = true;
    for (const auto id : ids) {
        if (!first) {
            list << ",";
        }
        first = false;
        list << id;
    }
    return copy_rows(db, db_name, table, "WHERE " + quote_ident(id_column) + " IN (" + list.str() + ")", result, err);
}

bool copy_one_by_id(
    sqlite3* db,
    const std::string& db_name,
    const std::string& table,
    const std::string& id_column,
    std::int64_t id,
    BattleSingleTurnJobSubsetResult* result,
    std::ostream& err) {
    if (id <= 0) {
        return true;
    }
    return copy_rows_by_ids(db, db_name, table, id_column, { id }, result, err);
}

bool copy_exec_job_without_coordinator_lineage(
    sqlite3* db,
    std::int64_t job_id,
    BattleSingleTurnJobSubsetResult* result,
    std::ostream& err) {
    if (job_id <= 0) {
        return true;
    }
    if (!table_exists(db, "main", "exec_job", err)
        || !table_exists(db, "src", "exec_job", err)) {
        return true;
    }
    const auto columns = common_columns(db, "exec_job", err);
    if (columns.empty()) {
        err << "No common columns for table exec_job\n";
        return false;
    }
    static const std::unordered_set<std::string> normalized_columns{
        "parent_job_id",
        "workset_id",
        "workset_item_ordinal",
        "dispatch_attempt_id",
        "dispatch_item_ordinal",
        "reserved_attempt_id",
        "worker_result_blob_id",
        "cancellation_caused_by_job_id",
    };
    std::ostringstream selected;
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (index != 0) {
            selected << ",";
        }
        if (normalized_columns.contains(columns[index])) {
            selected << "NULL";
        } else {
            selected << quote_ident(columns[index]);
        }
    }
    const auto sql =
        "INSERT OR IGNORE INTO main.exec_job ("
        + join_columns(columns)
        + ") SELECT "
        + selected.str()
        + " FROM src.exec_job WHERE job_id="
        + std::to_string(job_id)
        + ";";
    if (!exec(db, sql, err)) {
        err << "Failed copying normalized execution.exec_job\n";
        return false;
    }
    const int changed = sqlite3_changes(db);
    if (result != nullptr && changed != 0) {
        result->table_counts.push_back(
            {"execution", "exec_job", changed});
    }
    return true;
}

bool copy_battle_advancement_pool(
    sqlite3* db,
    std::int64_t pool_id,
    BattleSingleTurnJobSubsetResult* result,
    std::ostream& err) {
    if (pool_id <= 0) {
        return true;
    }
    if (table_exists(db, "main", "ab_battle_advancement_pool", err)
        && table_exists(db, "src", "ab_battle_advancement_pool", err)) {
        return copy_one_by_id(db,
            "analysis",
            "ab_battle_advancement_pool",
            "battle_advancement_pool_id",
            pool_id,
            result,
            err);
    }
    return copy_one_by_id(db, "analysis", "ab_selection_pool", "selection_pool_id", pool_id, result, err);
}

bool foreign_key_check(sqlite3* db, const std::string& db_name, std::ostream& err) {
    Statement st;
    if (!prepare(db, "PRAGMA foreign_key_check;", &st, err)) {
        return false;
    }
    if (sqlite3_step(st.st) == SQLITE_ROW) {
        err << "Foreign key check failed for " << db_name << ": table="
            << column_text(st.st, 0) << " rowid=" << sqlite3_column_int64(st.st, 1) << "\n";
        return false;
    }
    return true;
}

bool copy_with_attached_source(
    const fs::path& source_db_path,
    const fs::path& target_db_path,
    const std::string& db_name,
    const std::function<bool(sqlite3*)>& copy_fn,
    std::ostream& err) {
    SqliteHandle db;
    if (!open_sqlite(target_db_path, SQLITE_OPEN_READWRITE, &db, err)) {
        return false;
    }
    if (!attach_source(db.get(), source_db_path, err)) {
        return false;
    }
    if (!exec(db.get(), "PRAGMA foreign_keys=OFF;", err) || !exec(db.get(), "BEGIN IMMEDIATE;", err)) {
        return false;
    }
    if (!copy_fn(db.get())) {
        (void)exec(db.get(), "ROLLBACK;", err);
        return false;
    }
    if (!exec(db.get(), "COMMIT;", err)) {
        (void)exec(db.get(), "ROLLBACK;", err);
        return false;
    }
    if (!foreign_key_check(db.get(), db_name, err)) {
        return false;
    }
    return true;
}

std::optional<SourceExecJob> read_source_exec_job(sqlite3* execution_db, std::int64_t exec_job_id, std::ostream& err) {
    Statement st;
    constexpr const char* kSql =
        "SELECT job_id,job_set_id,program_kind,savestate_id FROM exec_job WHERE job_id=?1;";
    if (!prepare(execution_db, kSql, &st, err)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, exec_job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        err << "Execution job not found: " << exec_job_id << "\n";
        return std::nullopt;
    }
    SourceExecJob row{};
    row.job_id = sqlite3_column_int64(st.st, 0);
    row.job_set_id = sqlite3_column_int64(st.st, 1);
    row.program_kind = sqlite3_column_int(st.st, 2);
    row.savestate_id = column_i64_optional(st.st, 3);
    return row;
}

std::optional<SourceTurnJob> read_source_turn_job(sqlite3* analysis_db, const BattleJobSelector& selector, std::ostream& err) {
    const bool by_turn = selector.turn_job_id.has_value();
    constexpr const char* kSqlByTurn =
        "SELECT j.turn_job_id,j.exec_job_id,j.wave_id,w.battle_set_id,w.turn_index,j.plan_id,"
        "j.source_savestate_id,j.seed_candidate_id,j.output_savestate_id "
        "FROM ab_turn_job j JOIN ab_turn_wave w ON w.wave_id=j.wave_id WHERE j.turn_job_id=?1 LIMIT 1;";
    constexpr const char* kSqlByExec =
        "SELECT j.turn_job_id,j.exec_job_id,j.wave_id,w.battle_set_id,w.turn_index,j.plan_id,"
        "j.source_savestate_id,j.seed_candidate_id,j.output_savestate_id "
        "FROM ab_turn_job j JOIN ab_turn_wave w ON w.wave_id=j.wave_id WHERE j.exec_job_id=?1 LIMIT 1;";
    Statement st;
    if (!prepare(analysis_db, by_turn ? kSqlByTurn : kSqlByExec, &st, err)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, by_turn ? *selector.turn_job_id : *selector.exec_job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        err << "Battle turn job not found in source analysis DB.\n";
        return std::nullopt;
    }
    SourceTurnJob row{};
    row.turn_job_id = sqlite3_column_int64(st.st, 0);
    row.exec_job_id = column_i64_optional(st.st, 1);
    row.wave_id = sqlite3_column_int64(st.st, 2);
    row.battle_set_id = sqlite3_column_int64(st.st, 3);
    row.turn_index = sqlite3_column_int(st.st, 4);
    row.plan_id = sqlite3_column_int64(st.st, 5);
    row.source_savestate_id = column_i64_optional(st.st, 6);
    row.seed_candidate_id = column_i64_optional(st.st, 7);
    row.output_savestate_id = column_i64_optional(st.st, 8);
    return row;
}

std::optional<SourceBattleSet> read_battle_set(sqlite3* analysis_db, std::int64_t battle_set_id, std::ostream& err) {
    Statement st;
    constexpr const char* kSql =
        "SELECT battle_set_id,entry_savestate_id,battle_run_spec_id,explorer_settings_id "
        "FROM ab_battle_set WHERE battle_set_id=?1;";
    if (!prepare(analysis_db, kSql, &st, err)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, battle_set_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        err << "Battle set not found: " << battle_set_id << "\n";
        return std::nullopt;
    }
    return SourceBattleSet{
        .battle_set_id = sqlite3_column_int64(st.st, 0),
        .entry_savestate_id = sqlite3_column_int64(st.st, 1),
        .battle_run_spec_id = sqlite3_column_int64(st.st, 2),
        .explorer_settings_id = sqlite3_column_int64(st.st, 3),
    };
}

std::optional<SourceWaveRefs> read_wave_refs(sqlite3* analysis_db, std::int64_t wave_id, std::ostream& err) {
    Statement st;
    const auto pool_column = column_exists(analysis_db, "main", "ab_turn_wave", "battle_advancement_pool_id", err)
        ? "battle_advancement_pool_id"
        : "selection_pool_id";
    const auto sql = std::string("SELECT context_probe_id,parent_wave_id,parent_turn_job_id,seed_candidate_id,")
        + pool_column + " FROM ab_turn_wave WHERE wave_id=?1;";
    if (!prepare(analysis_db, sql.c_str(), &st, err)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, wave_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        err << "Turn wave not found: " << wave_id << "\n";
        return std::nullopt;
    }
    SourceWaveRefs refs{};
    refs.context_probe_id = column_i64_optional(st.st, 0);
    refs.parent_wave_id = column_i64_optional(st.st, 1);
    refs.parent_turn_job_id = column_i64_optional(st.st, 2);
    refs.seed_candidate_id = column_i64_optional(st.st, 3);
    refs.battle_advancement_pool_id = column_i64_optional(st.st, 4);
    return refs;
}

std::optional<SourceSeedCandidate> read_seed_candidate(sqlite3* analysis_db, std::int64_t seed_candidate_id, std::ostream& err) {
    Statement st;
    constexpr const char* kSql =
        "SELECT seed_candidate_id,source_probe_result_id,source_input_frame_id "
        "FROM ab_seed_candidate WHERE seed_candidate_id=?1;";
    if (!prepare(analysis_db, kSql, &st, err)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, seed_candidate_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        err << "Seed candidate not found: " << seed_candidate_id << "\n";
        return std::nullopt;
    }
    SourceSeedCandidate row{};
    row.seed_candidate_id = sqlite3_column_int64(st.st, 0);
    row.source_probe_result_id = column_i64_optional(st.st, 1);
    row.source_input_frame_id = column_i64_optional(st.st, 2);
    return row;
}

std::optional<SourceExplorerSettings> read_explorer_settings(sqlite3* authoring_db, std::int64_t settings_id, std::ostream& err) {
    Statement st;
    constexpr const char* kSql =
        "SELECT explorer_settings_id,default_plan_id,default_predicate_set_id "
        "FROM au_explorer_settings WHERE explorer_settings_id=?1;";
    if (!prepare(authoring_db, kSql, &st, err)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, settings_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        err << "Explorer settings not found: " << settings_id << "\n";
        return std::nullopt;
    }
    SourceExplorerSettings row{};
    row.explorer_settings_id = sqlite3_column_int64(st.st, 0);
    row.default_plan_id = column_i64_optional(st.st, 1);
    row.default_predicate_set_id = column_i64_optional(st.st, 2);
    return row;
}

std::set<std::int64_t> query_i64_set(sqlite3* db, const char* sql, std::int64_t id, std::ostream& err) {
    std::set<std::int64_t> values;
    Statement st;
    if (!prepare(db, sql, &st, err)) {
        return values;
    }
    sqlite3_bind_int64(st.st, 1, id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        if (sqlite3_column_type(st.st, 0) != SQLITE_NULL) {
            values.insert(sqlite3_column_int64(st.st, 0));
        }
    }
    return values;
}

std::optional<SourceConfirmedProbeResult> read_confirmed_probe_result(
    sqlite3* analysis_db,
    std::int64_t probe_result_id,
    std::ostream& err) {
    Statement st;
    constexpr const char* kSql =
        "SELECT probe_result_id,probe_run_id,input_frame_id "
        "FROM sp_probe_result "
        "WHERE probe_result_id=?1 AND evidence_state='CONFIRMED' "
        "AND confirmation_of_probe_result_id IS NULL;";
    if (!prepare(analysis_db, kSql, &st, err)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, probe_result_id);
    const auto rc = sqlite3_step(st.st);
    if (rc == SQLITE_ROW) {
        return SourceConfirmedProbeResult{
            .probe_result_id = sqlite3_column_int64(st.st, 0),
            .probe_run_id = sqlite3_column_int64(st.st, 1),
            .input_frame_id = sqlite3_column_int64(st.st, 2),
        };
    }
    if (rc != SQLITE_DONE) {
        err << "Failed resolving confirmed SeedProbe result: "
            << sqlite3_errmsg(analysis_db) << "\n";
    } else {
        err << "Battle seed candidate references a missing or unconfirmed "
               "SeedProbe result: "
            << probe_result_id << "\n";
    }
    return std::nullopt;
}

void collect_input_frame_axis_ids(sqlite3* analysis_db, const std::set<std::int64_t>& input_frame_ids, std::set<std::int64_t>* axis_ids, std::ostream& err) {
    if (axis_ids == nullptr) {
        return;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT main_axis_xy_id,cstick_axis_xy_id,trigger_axis_xy_id FROM sp_input_frame WHERE input_frame_id=?1;";
    for (const auto input_frame_id : input_frame_ids) {
        if (!prepare(analysis_db, kSql, &st, err)) {
            return;
        }
        sqlite3_bind_int64(st.st, 1, input_frame_id);
        if (sqlite3_step(st.st) == SQLITE_ROW) {
            axis_ids->insert(sqlite3_column_int64(st.st, 0));
            axis_ids->insert(sqlite3_column_int64(st.st, 1));
            axis_ids->insert(sqlite3_column_int64(st.st, 2));
        }
        sqlite3_finalize(st.st);
        st.st = nullptr;
    }
}

std::optional<ArtifactRecord> read_artifact_for_savestate(sqlite3* state_db, std::int64_t savestate_id, std::ostream& err) {
    Statement st;
    constexpr const char* kSql =
        "SELECT s.savestate_id,a.artifact_id,a.sha256,a.file_ext,a.filename "
        "FROM state_savestate s JOIN state_artifact a ON a.artifact_id=s.artifact_id "
        "WHERE s.savestate_id=?1;";
    if (!prepare(state_db, kSql, &st, err)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, savestate_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        err << "Savestate not found: " << savestate_id << "\n";
        return std::nullopt;
    }
    ArtifactRecord row{};
    row.savestate_id = sqlite3_column_int64(st.st, 0);
    row.artifact_id = sqlite3_column_int64(st.st, 1);
    row.sha256 = column_text(st.st, 2);
    row.file_ext = column_text(st.st, 3);
    row.source_path = column_text(st.st, 4);
    return row;
}

bool rewrite_artifact_filename(sqlite3* target_state_db, std::int64_t artifact_id, const fs::path& path, std::ostream& err) {
    Statement st;
    if (!prepare(target_state_db, "UPDATE state_artifact SET filename=?2 WHERE artifact_id=?1;", &st, err)) {
        return false;
    }
    sqlite3_bind_int64(st.st, 1, artifact_id);
    sqlite3_bind_text(st.st, 2, path.string().c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        err << "Failed rewriting artifact filename: " << sqlite3_errmsg(target_state_db) << "\n";
        return false;
    }
    return true;
}

bool validate_selector(const BattleJobSelector& selector, std::ostream& err) {
    const bool has_turn = selector.turn_job_id.has_value();
    const bool has_exec = selector.exec_job_id.has_value();
    if (has_turn == has_exec) {
        err << "Specify exactly one battle job selector.\n";
        return false;
    }
    if (has_turn && *selector.turn_job_id <= 0) {
        err << "turn_job_id must be positive.\n";
        return false;
    }
    if (has_exec && *selector.exec_job_id <= 0) {
        err << "exec_job_id must be positive.\n";
        return false;
    }
    return true;
}

} // namespace

const char* ToString(SandboxMode mode) {
    switch (mode) {
    case SandboxMode::MinimalBattleSingleTurn: return "minimal";
    case SandboxMode::FullCopy: return "full-copy";
    }
    return "unknown";
}

std::optional<SandboxMode> ParseSandboxMode(std::string_view text) {
    if (text == "minimal") {
        return SandboxMode::MinimalBattleSingleTurn;
    }
    if (text == "full-copy") {
        return SandboxMode::FullCopy;
    }
    return std::nullopt;
}

savor::db::DbConfigPaths MakeDbConfigPaths(const fs::path& root) {
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

int CopyDbRootFull(const CopyDbRootOptions& options, std::ostream& out, std::ostream& err) {
    std::error_code ec;
    if (!fs::is_directory(options.source_root, ec)) {
        err << "Source DB root does not exist: " << options.source_root.string() << "\n";
        return 1;
    }
    if (!remove_existing_if_allowed(options.dest_root, options.overwrite, options.source_root, &out, err)) {
        return 1;
    }
    fs::create_directories(options.dest_root, ec);
    if (ec) {
        err << "Failed to create destination: " << ec.message() << "\n";
        return 1;
    }

    constexpr std::array<const char*, 6> db_names = {
        "analysis.db",
        "execution.db",
        "state.db",
        "ui_read.db",
        "authoring.db",
        "archive.db" };

    for (const auto* db_name : db_names) {
        const auto source_db = options.source_root / db_name;
        if (!fs::exists(source_db, ec)) {
            continue;
        }
        out << "Copying " << db_name << " via sqlite backup\n";
        const auto dest_db = options.dest_root / db_name;
        if (const int rc = copy_sqlite_db(source_db, dest_db, err); rc != 0) {
            return rc;
        }
    }
    if (const int rc = copy_directory_if_present(options.source_root / "object_store", options.dest_root / "object_store", out, err); rc != 0) {
        return rc;
    }
    if (const int rc = copy_directory_if_present(options.source_root / "archive_store", options.dest_root / "archive_store", out, err); rc != 0) {
        return rc;
    }
    out << "Prepared DB root at " << options.dest_root.string() << "\n";
    return 0;
}

int CreateEmptyMigratedDbRoot(const CreateEmptyMigratedDbRootOptions& options, std::ostream& err) {
    if (options.db_root.empty()) {
        err << "Target DB root is empty.\n";
        return 1;
    }
    if (!remove_existing_if_allowed(options.db_root, options.overwrite, std::nullopt, nullptr, err)) {
        return 1;
    }
    std::error_code ec;
    fs::create_directories(options.db_root, ec);
    if (ec) {
        err << "Failed creating DB root: " << ec.message() << "\n";
        return 1;
    }
    savor::db::core::DBService db_service(
        MakeDbConfigPaths(options.db_root),
        savor::db::migrations::MigrationSourceOptions{ .source_kind = savor::db::migrations::MigrationSourceKind::Embedded });
    std::string error;
    if (!db_service.Start(&error)) {
        err << "Failed creating migrated DB root: " << error << "\n";
        return 1;
    }
    db_service.Stop();
    return 0;
}

int hydrate_battle_single_turn_job_subset_into_existing(
    const HydrateBattleSingleTurnJobSubsetOptions& options,
    BattleSingleTurnJobSubsetResult* result_out,
    std::ostream& out,
    std::ostream& err) {
    if (result_out == nullptr) {
        err << "Internal error: null subset result.\n";
        return 1;
    }
    *result_out = BattleSingleTurnJobSubsetResult{};
    result_out->source_root = options.source_root;
    result_out->target_root = options.target_root;

    if (!validate_selector(options.selector, err)) {
        return 2;
    }
    std::error_code ec;
    if (!fs::is_directory(options.source_root, ec)) {
        err << "Source DB root does not exist: " << options.source_root.string() << "\n";
        return 1;
    }

    const auto artifact_root = options.artifact_root.empty()
        ? options.target_root.parent_path() / "source-artifacts"
        : options.artifact_root;

    SqliteHandle source_execution;
    SqliteHandle source_analysis;
    SqliteHandle source_authoring;
    SqliteHandle source_state;
    if (!open_sqlite(options.source_root / "execution.db", SQLITE_OPEN_READONLY, &source_execution, err)
        || !open_sqlite(options.source_root / "analysis.db", SQLITE_OPEN_READONLY, &source_analysis, err)
        || !open_sqlite(options.source_root / "authoring.db", SQLITE_OPEN_READONLY, &source_authoring, err)
        || !open_sqlite(options.source_root / "state.db", SQLITE_OPEN_READONLY, &source_state, err)) {
        return 1;
    }

    auto turn = read_source_turn_job(source_analysis.get(), options.selector, err);
    if (!turn.has_value()) {
        result_out->validation_errors.push_back("source turn job not found");
        return 1;
    }
    if (turn->turn_index != 1) {
        result_out->validation_errors.push_back("only first-turn jobs are supported");
        err << "Minimal battle subset only supports first-turn jobs; turn_index=" << turn->turn_index << "\n";
        return 1;
    }
    if (!turn->exec_job_id.has_value()) {
        result_out->validation_errors.push_back("source turn job has no exec_job_id");
        err << "Source battle turn job has no exec_job_id.\n";
        return 1;
    }
    auto exec_job = read_source_exec_job(source_execution.get(), *turn->exec_job_id, err);
    if (!exec_job.has_value()) {
        result_out->validation_errors.push_back("source execution job not found");
        return 1;
    }
    if (exec_job->program_kind != kBattleSingleTurnProgramKind) {
        result_out->validation_errors.push_back("source execution job is not BattleSingleTurn");
        err << "Source execution job is not BattleSingleTurn; program_kind=" << exec_job->program_kind << "\n";
        return 1;
    }
    auto battle_set = read_battle_set(source_analysis.get(), turn->battle_set_id, err);
    auto wave_refs = read_wave_refs(source_analysis.get(), turn->wave_id, err);
    if (!battle_set.has_value() || !wave_refs.has_value()) {
        return 1;
    }
    const auto seed_candidate_id = turn->seed_candidate_id.value_or(wave_refs->seed_candidate_id.value_or(0));
    auto seed_candidate = seed_candidate_id > 0
        ? read_seed_candidate(source_analysis.get(), seed_candidate_id, err)
        : std::optional<SourceSeedCandidate>{};
    auto settings = read_explorer_settings(source_authoring.get(), battle_set->explorer_settings_id, err);
    if (!settings.has_value()) {
        return 1;
    }

    result_out->source_turn_job_id = turn->turn_job_id;
    result_out->source_exec_job_id = *turn->exec_job_id;
    result_out->source_job_set_id = exec_job->job_set_id;
    result_out->source_battle_set_id = battle_set->battle_set_id;
    result_out->source_wave_id = turn->wave_id;

    std::set<std::int64_t> input_frame_ids;
    std::set<std::int64_t> probe_result_ids;
    std::set<std::int64_t> probe_run_ids;
    std::set<std::int64_t> probe_set_ids;
    std::set<std::int64_t> analysis_input_set_ids;
    std::set<std::int64_t> axis_ids;
    if (seed_candidate.has_value()) {
        if (seed_candidate->source_probe_result_id.has_value()) {
            const auto result = read_confirmed_probe_result(
                source_analysis.get(),
                *seed_candidate->source_probe_result_id,
                err);
            if (!result.has_value()) {
                result_out->validation_errors.push_back(
                    "seed candidate source is not a confirmed SeedProbe result");
                return 1;
            }
            probe_result_ids.insert(result->probe_result_id);
            probe_run_ids.insert(result->probe_run_id);
            input_frame_ids.insert(result->input_frame_id);
        }
        if (seed_candidate->source_input_frame_id.has_value()) {
            input_frame_ids.insert(*seed_candidate->source_input_frame_id);
        }
    }
    for (const auto probe_run_id : probe_run_ids) {
        if (const auto probe_set_id = scalar_i64(source_analysis.get(), "SELECT probe_set_id FROM sp_probe_run WHERE probe_run_id=?1;", probe_run_id, err);
            probe_set_id.has_value()) {
            probe_set_ids.insert(*probe_set_id);
        }
        if (const auto input_set_id = scalar_i64(source_analysis.get(), "SELECT accepted_input_set_id FROM sp_probe_run WHERE probe_run_id=?1;", probe_run_id, err);
            input_set_id.has_value()) {
            analysis_input_set_ids.insert(*input_set_id);
            const auto accepted_frames = query_i64_set(
                source_analysis.get(),
                "SELECT input_frame_id FROM an_input_set_frame "
                "WHERE input_set_id=?1;",
                *input_set_id,
                err);
            input_frame_ids.insert(
                accepted_frames.begin(),
                accepted_frames.end());
        }
        const auto accepted_evidence = query_i64_set(
            source_analysis.get(),
            "SELECT observation.probe_result_id "
            "FROM sp_probe_result observation "
            "WHERE observation.probe_run_id=?1 AND ("
            "  (observation.evidence_state='CONFIRMED' "
            "   AND observation.confirmation_of_probe_result_id IS NULL "
            "   AND EXISTS ("
            "     SELECT 1 FROM sp_probe_run accepted_run "
            "     JOIN an_input_set_frame accepted_frame "
            "       ON accepted_frame.input_set_id=accepted_run.accepted_input_set_id "
            "     WHERE accepted_run.probe_run_id=observation.probe_run_id "
            "       AND accepted_frame.input_frame_id=observation.input_frame_id"
            "   )) "
            "  OR EXISTS ("
            "    SELECT 1 FROM sp_probe_result representative "
            "    JOIN sp_probe_run accepted_run "
            "      ON accepted_run.probe_run_id=representative.probe_run_id "
            "    JOIN an_input_set_frame accepted_frame "
            "      ON accepted_frame.input_set_id=accepted_run.accepted_input_set_id "
            "      AND accepted_frame.input_frame_id=representative.input_frame_id "
            "    WHERE representative.probe_result_id="
            "      observation.confirmation_of_probe_result_id "
            "      AND representative.evidence_state='CONFIRMED' "
            "      AND representative.confirmation_of_probe_result_id IS NULL"
            "  )"
            ");",
            probe_run_id,
            err);
        probe_result_ids.insert(
            accepted_evidence.begin(),
            accepted_evidence.end());
    }
    for (const auto probe_result_id : probe_result_ids) {
        if (const auto input_frame_id = scalar_i64(
                source_analysis.get(),
                "SELECT input_frame_id FROM sp_probe_result "
                "WHERE probe_result_id=?1;",
                probe_result_id,
                err);
            input_frame_id.has_value()) {
            input_frame_ids.insert(*input_frame_id);
        }
    }
    collect_input_frame_axis_ids(source_analysis.get(), input_frame_ids, &axis_ids, err);

    std::set<std::int64_t> plan_ids = { turn->plan_id };
    if (settings->default_plan_id.has_value()) {
        plan_ids.insert(*settings->default_plan_id);
    }
    std::set<std::int64_t> predicate_set_ids;
    if (settings->default_predicate_set_id.has_value()) {
        predicate_set_ids.insert(*settings->default_predicate_set_id);
    }
    std::set<std::int64_t> action_preset_ids;
    for (const auto plan_id : plan_ids) {
        auto ids = query_i64_set(
            source_authoring.get(),
            "SELECT DISTINCT a.action_preset_id "
            "FROM au_battle_plan_action a JOIN au_battle_plan_turn t ON t.plan_turn_id=a.plan_turn_id "
            "WHERE t.plan_id=?1;",
            plan_id,
            err);
        action_preset_ids.insert(ids.begin(), ids.end());
    }
    std::set<std::int64_t> predicate_spec_ids;
    for (const auto predicate_set_id : predicate_set_ids) {
        auto ids = query_i64_set(
            source_authoring.get(),
            "SELECT predicate_spec_id FROM au_predicate_set_item WHERE predicate_set_id=?1;",
            predicate_set_id,
            err);
        predicate_spec_ids.insert(ids.begin(), ids.end());
    }
    std::set<std::int64_t> address_program_ids;
    for (const auto predicate_spec_id : predicate_spec_ids) {
        if (const auto lhs = scalar_i64(source_authoring.get(), "SELECT lhs_address_program_id FROM au_predicate_spec WHERE predicate_spec_id=?1;", predicate_spec_id, err);
            lhs.has_value()) {
            address_program_ids.insert(*lhs);
        }
        if (const auto rhs = scalar_i64(source_authoring.get(), "SELECT rhs_address_program_id FROM au_predicate_spec WHERE predicate_spec_id=?1;", predicate_spec_id, err);
            rhs.has_value()) {
            address_program_ids.insert(*rhs);
        }
    }

    std::set<std::int64_t> savestate_ids;
    if (exec_job->savestate_id.has_value()) {
        savestate_ids.insert(*exec_job->savestate_id);
    }
    if (battle_set->entry_savestate_id > 0) {
        savestate_ids.insert(battle_set->entry_savestate_id);
    }
    if (turn->source_savestate_id.has_value()) {
        savestate_ids.insert(*turn->source_savestate_id);
    }
    if (turn->output_savestate_id.has_value()) {
        savestate_ids.insert(*turn->output_savestate_id);
    }
    if (savestate_ids.empty()) {
        result_out->validation_errors.push_back("no referenced source savestate");
        err << "No source savestate was referenced by the selected battle job.\n";
        return 1;
    }

    std::set<std::int64_t> artifact_ids;
    std::vector<ArtifactRecord> artifacts;
    for (const auto savestate_id : savestate_ids) {
        auto artifact = read_artifact_for_savestate(source_state.get(), savestate_id, err);
        if (!artifact.has_value()) {
            result_out->validation_errors.push_back("missing state savestate/artifact");
            return 1;
        }
        if (!fs::exists(artifact->source_path, ec)) {
            result_out->validation_errors.push_back("missing artifact file");
            err << "Source artifact file does not exist: " << artifact->source_path.string() << "\n";
            return 1;
        }
        artifact_ids.insert(artifact->artifact_id);
        artifacts.push_back(std::move(*artifact));
    }

    const auto source_paths = MakeDbConfigPaths(options.source_root);
    const auto target_paths = MakeDbConfigPaths(options.target_root);

    if (!copy_with_attached_source(
            source_paths.execution_db_path,
            target_paths.execution_db_path,
            "execution",
            [&](sqlite3* db) {
                const auto job_set_id = std::to_string(exec_job->job_set_id);
                const auto workflow_instance_filter =
                    "WHERE workflow_instance_id IN (SELECT workflow_instance_id FROM src.exec_workflow_step WHERE job_set_id=" + job_set_id + ")";
                if (!copy_one_by_id(db, "execution", "exec_job_set", "job_set_id", exec_job->job_set_id, result_out, err)
                    || !copy_rows(db, "execution", "exec_workflow_instance", workflow_instance_filter, result_out, err)
                    || !copy_rows(db, "execution", "exec_workflow_instance_argument", workflow_instance_filter, result_out, err)
                    || !copy_rows(db, "execution", "exec_workflow_instance_input_binding", workflow_instance_filter, result_out, err)
                    || !copy_rows(db, "execution", "exec_workflow_unit_activation", workflow_instance_filter, result_out, err)
                    || !copy_rows(db, "execution", "exec_workflow_unit_activation_edge", workflow_instance_filter, result_out, err)
                    || !copy_rows(db, "execution", "exec_workflow_step", "WHERE job_set_id=" + job_set_id, result_out, err)
                    || !copy_exec_job_without_coordinator_lineage(
                        db,
                        exec_job->job_id,
                        result_out,
                        err)) {
                    return false;
                }
                return exec(db, "UPDATE exec_job_set SET parent_job_set_id=NULL WHERE job_set_id=" + std::to_string(exec_job->job_set_id) + ";", err)
                    && exec(
                        db,
                        "UPDATE exec_job SET parent_job_id=NULL WHERE job_id="
                            + std::to_string(exec_job->job_id)
                            + ";",
                        err);
            },
            err)) {
        return 1;
    }

    if (!copy_with_attached_source(
            source_paths.analysis_db_path,
            target_paths.analysis_db_path,
            "analysis",
            [&](sqlite3* db) {
                return copy_rows_by_ids(db, "analysis", "sp_probe_set", "probe_set_id", probe_set_ids, result_out, err)
                    && copy_rows_by_ids(db, "analysis", "an_input_set", "input_set_id", analysis_input_set_ids, result_out, err)
                    && copy_rows_by_ids(db, "analysis", "sp_probe_run", "probe_run_id", probe_run_ids, result_out, err)
                    && copy_rows_by_ids(db, "analysis", "sp_probe_result", "probe_result_id", probe_result_ids, result_out, err)
                    && copy_rows_by_ids(db, "analysis", "sp_axis_xy", "axis_xy_id", axis_ids, result_out, err)
                    && copy_rows_by_ids(db, "analysis", "sp_input_frame", "input_frame_id", input_frame_ids, result_out, err)
                    && copy_rows_by_ids(db, "analysis", "an_input_set_frame", "input_set_id", analysis_input_set_ids, result_out, err)
                    && copy_one_by_id(db, "analysis", "ab_battle_set", "battle_set_id", battle_set->battle_set_id, result_out, err)
                    && copy_one_by_id(db, "analysis", "ab_seed_candidate", "seed_candidate_id", seed_candidate_id, result_out, err)
                    && copy_battle_advancement_pool(db, wave_refs->battle_advancement_pool_id.value_or(0), result_out, err)
                    && copy_one_by_id(db, "analysis", "ab_battle_context_probe", "context_probe_id", wave_refs->context_probe_id.value_or(0), result_out, err)
                    && copy_one_by_id(db, "analysis", "ab_turn_wave", "wave_id", turn->wave_id, result_out, err)
                    && copy_one_by_id(db, "analysis", "ab_turn_job", "turn_job_id", turn->turn_job_id, result_out, err);
            },
            err)) {
        return 1;
    }

    if (!copy_with_attached_source(
            source_paths.authoring_db_path,
            target_paths.authoring_db_path,
            "authoring",
            [&](sqlite3* db) {
                return copy_rows_by_ids(db, "authoring", "au_address_program", "address_program_id", address_program_ids, result_out, err)
                    && copy_rows_by_ids(db, "authoring", "au_predicate_spec", "predicate_spec_id", predicate_spec_ids, result_out, err)
                    && copy_rows_by_ids(db, "authoring", "au_predicate_spec_required_breakpoint", "predicate_spec_id", predicate_spec_ids, result_out, err)
                    && copy_rows_by_ids(db, "authoring", "au_predicate_set", "predicate_set_id", predicate_set_ids, result_out, err)
                    && copy_rows_by_ids(db, "authoring", "au_predicate_set_item", "predicate_set_id", predicate_set_ids, result_out, err)
                    && copy_rows_by_ids(db, "authoring", "au_battle_plan", "plan_id", plan_ids, result_out, err)
                    && copy_rows(db, "authoring", "au_battle_plan_turn", "WHERE plan_id IN (" + [&]() {
                        std::ostringstream ids;
                        bool first = true;
                        for (const auto id : plan_ids) {
                            if (!first) ids << ",";
                            first = false;
                            ids << id;
                        }
                        return ids.str();
                    }() + ")", result_out, err)
                    && copy_rows(db, "authoring", "au_battle_plan_action", "WHERE plan_turn_id IN (SELECT plan_turn_id FROM src.au_battle_plan_turn WHERE plan_id IN (" + [&]() {
                        std::ostringstream ids;
                        bool first = true;
                        for (const auto id : plan_ids) {
                            if (!first) ids << ",";
                            first = false;
                            ids << id;
                        }
                        return ids.str();
                    }() + "))", result_out, err)
                    && copy_rows_by_ids(db, "authoring", "au_battle_plan_action_preset", "action_preset_id", action_preset_ids, result_out, err)
                    && copy_one_by_id(db, "authoring", "au_battle_run_spec", "battle_run_spec_id", battle_set->battle_run_spec_id, result_out, err)
                    && copy_one_by_id(db, "authoring", "au_explorer_settings", "explorer_settings_id", battle_set->explorer_settings_id, result_out, err);
            },
            err)) {
        return 1;
    }

    if (!copy_with_attached_source(
            source_paths.state_db_path,
            target_paths.state_db_path,
            "state",
            [&](sqlite3* db) {
                return copy_rows_by_ids(db, "state", "state_artifact", "artifact_id", artifact_ids, result_out, err)
                    && copy_rows_by_ids(db, "state", "state_savestate", "savestate_id", savestate_ids, result_out, err);
            },
            err)) {
        return 1;
    }

    SqliteHandle target_state;
    if (!open_sqlite(target_paths.state_db_path, SQLITE_OPEN_READWRITE, &target_state, err)) {
        return 1;
    }
    fs::create_directories(artifact_root, ec);
    if (ec) {
        err << "Failed creating artifact root: " << ec.message() << "\n";
        return 1;
    }
    std::set<std::int64_t> copied_artifacts;
    for (const auto& artifact : artifacts) {
        if (copied_artifacts.contains(artifact.artifact_id)) {
            continue;
        }
        const auto ext = artifact.file_ext.empty() ? artifact.source_path.extension().string() : artifact.file_ext;
        const auto copied_path = artifact_root / ("artifact-" + std::to_string(artifact.artifact_id) + ext);
        fs::copy_file(artifact.source_path, copied_path, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            err << "Failed copying artifact file " << artifact.source_path.string()
                << " to " << copied_path.string() << ": " << ec.message() << "\n";
            return 1;
        }
        if (!rewrite_artifact_filename(target_state.get(), artifact.artifact_id, copied_path, err)) {
            return 1;
        }
        copied_artifacts.insert(artifact.artifact_id);
        result_out->copied_artifacts.push_back({
            .artifact_id = artifact.artifact_id,
            .savestate_id = artifact.savestate_id,
            .source_path = artifact.source_path,
            .copied_path = copied_path,
        });
    }

    out << "Hydrated minimal battle job exec " << result_out->source_exec_job_id
        << " into " << options.target_root.string() << "\n";
    return 0;
}

int HydrateBattleSingleTurnJobSubset(
    const HydrateBattleSingleTurnJobSubsetOptions& options,
    BattleSingleTurnJobSubsetResult* result_out,
    std::ostream& out,
    std::ostream& err) {
    if (result_out == nullptr) {
        err << "Internal error: null subset result.\n";
        return 1;
    }
    if (const int rc = CreateEmptyMigratedDbRoot(
            { .db_root = options.target_root, .overwrite = options.overwrite_target },
            err);
        rc != 0) {
        return rc;
    }
    const int rc = hydrate_battle_single_turn_job_subset_into_existing(options, result_out, out, err);
    if (rc == 0) {
        out << "Prepared minimal battle job DB root at " << options.target_root.string() << "\n";
    }
    return rc;
}

int HydrateBattleSingleTurnJobSubsets(
    const HydrateBattleSingleTurnJobSubsetsOptions& options,
    BattleSingleTurnJobSubsetsResult* result_out,
    std::ostream& out,
    std::ostream& err) {
    if (result_out == nullptr) {
        err << "Internal error: null subsets result.\n";
        return 1;
    }
    *result_out = BattleSingleTurnJobSubsetsResult{};
    result_out->source_root = options.source_root;
    result_out->target_root = options.target_root;

    if (options.selectors.empty()) {
        result_out->validation_errors.push_back("no battle job selectors");
        err << "At least one battle job selector is required.\n";
        return 2;
    }

    std::set<std::string> seen_selectors;
    for (const auto& selector : options.selectors) {
        if (!validate_selector(selector, err)) {
            result_out->validation_errors.push_back("invalid battle job selector");
            return 2;
        }
        std::ostringstream key;
        if (selector.exec_job_id.has_value()) {
            key << "exec:" << *selector.exec_job_id;
        } else {
            key << "turn:" << *selector.turn_job_id;
        }
        if (!seen_selectors.insert(key.str()).second) {
            result_out->validation_errors.push_back("duplicate battle job selector");
            err << "Duplicate battle job selector: " << key.str() << "\n";
            return 2;
        }
    }

    if (const int rc = CreateEmptyMigratedDbRoot(
            { .db_root = options.target_root, .overwrite = options.overwrite_target },
            err);
        rc != 0) {
        return rc;
    }

    std::set<std::int64_t> copied_artifact_ids;
    for (const auto& selector : options.selectors) {
        BattleSingleTurnJobSubsetResult job_result;
        HydrateBattleSingleTurnJobSubsetOptions job_options{
            .source_root = options.source_root,
            .target_root = options.target_root,
            .artifact_root = options.artifact_root,
            .selector = selector,
            .overwrite_target = false,
        };
        const int rc = hydrate_battle_single_turn_job_subset_into_existing(
            job_options,
            &job_result,
            out,
            err);
        result_out->validation_errors.insert(
            result_out->validation_errors.end(),
            job_result.validation_errors.begin(),
            job_result.validation_errors.end());
        if (rc != 0) {
            return rc;
        }
        result_out->table_counts.insert(
            result_out->table_counts.end(),
            job_result.table_counts.begin(),
            job_result.table_counts.end());
        for (const auto& artifact : job_result.copied_artifacts) {
            if (copied_artifact_ids.insert(artifact.artifact_id).second) {
                result_out->copied_artifacts.push_back(artifact);
            }
        }
        result_out->jobs.push_back(std::move(job_result));
    }

    out << "Prepared minimal battle job DB root at " << options.target_root.string()
        << " for " << result_out->jobs.size() << " jobs\n";
    return 0;
}

} // namespace savor::dbutils
