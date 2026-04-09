#include "DbPreparer.h"

#include <fstream>

#include "Common/Migrations/MigrationRunner.h"
#include "Execution/Workflow/SqliteExecutionDb.h"
#include "DbJSONL.h"
#include "ExecSql.h"


    DbPreparer::DbPreparer(sqlite3* db, std::filesystem::path migration_root)
        : db_(db), migration_root_(std::move(migration_root)) {
    }

    bool DbPreparer::InitializeRequiredMigrations(std::string* error_out) const {
        using namespace simcore::db::migrations;
        const MigrationSourceOptions options{ .source_kind = MigrationSourceKind::Filesystem, .filesystem_root = migration_root_ };
        return ApplyContextMigrations(db_, MigrationContext::Execution, options, error_out)
            && ApplyContextMigrations(db_, MigrationContext::Authoring, options, error_out)
            && ApplyContextMigrations(db_, MigrationContext::State, options, error_out);
    }

    bool DbPreparer::SeedRowsFromJsonObject(const std::string& json_object, std::string* error_out) const {
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

    bool DbPreparer::SeedRowsFromJsonlDirectory(const std::filesystem::path& folder, std::string* error_out) const {
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

    bool DbPreparer::SeedSavestateArtifactAndOverride(const std::filesystem::path& savestate_file, std::string* error_out) {
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

    bool DbPreparer::SeedRowsForTableArray(const std::string& table_name, const std::string& rows_json, std::string* error_out) const {
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

    bool DbPreparer::InsertJsonObjectRow(const std::string& table_name, const std::string& row_json, std::string* error_out) const {
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
            }
            else {
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

    const std::string* DbPreparer::OverrideValueForColumn(const std::string& table_name, const std::string& column_name) const {
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

    bool DbPreparer::IsSavestateIdColumn(const std::string& column_name) const {
        constexpr const char* suffix = "savestate_id";
        if (column_name == suffix) {
            return true;
        }
        if (column_name.size() <= std::char_traits<char>::length(suffix)) {
            return false;
        }
        return column_name.compare(column_name.size() - std::char_traits<char>::length(suffix), std::char_traits<char>::length(suffix), suffix) == 0;
    }


