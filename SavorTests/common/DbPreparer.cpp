#include "DbPreparer.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include "Common/Migrations/MigrationRunner.h"

namespace {

struct Statement {
    sqlite3_stmt* st = nullptr;
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }
};

} // namespace

DbPreparer::DbPreparer(sqlite3* db, std::filesystem::path migration_root)
    : db_(db), migration_root_(std::move(migration_root)) {}

bool DbPreparer::InitializeRequiredMigrations(std::string* error_out) const {
    using namespace savor::db::migrations;

    if (db_ == nullptr) {
        if (error_out != nullptr) {
            *error_out = "sqlite db handle is null";
        }
        return false;
    }

    const MigrationSourceOptions options{
        .source_kind = MigrationSourceKind::Filesystem,
        .filesystem_root = migration_root_,
    };

    return ApplyContextMigrations(db_, MigrationContext::Execution, options, error_out)
        && ApplyContextMigrations(db_, MigrationContext::State, options, error_out);
}

bool DbPreparer::SeedSavestateArtifactAndOverride(const std::filesystem::path& savestate_path, std::string* error_out) {
    if (!std::filesystem::exists(savestate_path)) {
        if (error_out != nullptr) {
            *error_out = "savestate path does not exist: " + savestate_path.string();
        }
        return false;
    }

    savestate_override_path_ = savestate_path;
    return true;
}

bool DbPreparer::SeedRowsFromJsonObject(const std::string& json_object, std::string* error_out) const {
    if (db_ == nullptr) {
        if (error_out != nullptr) {
            *error_out = "sqlite db handle is null";
        }
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(db_, "SELECT key, value FROM json_each(?1);", -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    sqlite3_bind_text(st.st, 1, json_object.c_str(), static_cast<int>(json_object.size()), SQLITE_TRANSIENT);

    while (sqlite3_step(st.st) == SQLITE_ROW) {
        const auto* table_text = sqlite3_column_text(st.st, 0);
        const auto* array_text = sqlite3_column_text(st.st, 1);
        if (table_text == nullptr || array_text == nullptr) {
            continue;
        }

        const std::string table_name = reinterpret_cast<const char*>(table_text);
        const std::string json_array_text = reinterpret_cast<const char*>(array_text);
        if (!SeedRowsForTableFromJsonArrayText(table_name, json_array_text, error_out)) {
            return false;
        }
    }

    return true;
}

bool DbPreparer::SeedRowsFromJsonlDirectory(const std::filesystem::path& jsonl_dir, std::string* error_out) const {
    if (db_ == nullptr) {
        if (error_out != nullptr) {
            *error_out = "sqlite db handle is null";
        }
        return false;
    }

    if (!std::filesystem::exists(jsonl_dir)) {
        if (error_out != nullptr) {
            *error_out = "jsonl directory does not exist: " + jsonl_dir.string();
        }
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(jsonl_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".jsonl") {
            continue;
        }

        const std::string table_name = entry.path().stem().string();
        std::ifstream in(entry.path());
        if (!in) {
            if (error_out != nullptr) {
                *error_out = "failed to open: " + entry.path().string();
            }
            return false;
        }

        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }
            if (!InsertJsonRow(table_name, line, error_out)) {
                return false;
            }
        }
    }

    return true;
}

bool DbPreparer::SeedRowsForTableFromJsonArrayText(
    const std::string& table_name,
    const std::string& json_array_text,
    std::string* error_out) const {
    Statement st;
    if (sqlite3_prepare_v2(db_, "SELECT value FROM json_each(?1);", -1, &st.st, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    sqlite3_bind_text(st.st, 1, json_array_text.c_str(), static_cast<int>(json_array_text.size()), SQLITE_TRANSIENT);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        const auto* row_text = sqlite3_column_text(st.st, 0);
        if (row_text == nullptr) {
            continue;
        }
        if (!InsertJsonRow(table_name, reinterpret_cast<const char*>(row_text), error_out)) {
            return false;
        }
    }
    return true;
}

bool DbPreparer::InsertJsonRow(const std::string& table_name, const std::string& row_json, std::string* error_out) const {
    Statement key_query;
    if (sqlite3_prepare_v2(db_, "SELECT key, value, type FROM json_each(?1);", -1, &key_query.st, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    sqlite3_bind_text(key_query.st, 1, row_json.c_str(), static_cast<int>(row_json.size()), SQLITE_TRANSIENT);

    struct Field {
        std::string key;
        std::string value;
        std::string type;
    };
    std::vector<Field> fields;

    while (sqlite3_step(key_query.st) == SQLITE_ROW) {
        const auto* key_text = sqlite3_column_text(key_query.st, 0);
        const auto* value_text = sqlite3_column_text(key_query.st, 1);
        const auto* type_text = sqlite3_column_text(key_query.st, 2);
        if (key_text == nullptr || type_text == nullptr) {
            continue;
        }

        Field f;
        f.key = reinterpret_cast<const char*>(key_text);
        f.type = reinterpret_cast<const char*>(type_text);
        if (value_text != nullptr) {
            f.value = reinterpret_cast<const char*>(value_text);
        }

        if (savestate_override_path_.has_value() && table_name == "state_artifact") {
            if (f.key == "filename") {
                f.type = "text";
                f.value = savestate_override_path_->filename().string();
            } else if (f.key == "file_ext") {
                f.type = "text";
                f.value = savestate_override_path_->extension().string();
            }
        }

        fields.push_back(std::move(f));
    }

    if (fields.empty()) {
        return true;
    }

    std::ostringstream sql;
    sql << "INSERT OR REPLACE INTO " << table_name << " (";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            sql << ',';
        }
        sql << fields[i].key;
    }
    sql << ") VALUES (";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            sql << ',';
        }
        sql << '?' << (i + 1);
    }
    sql << ");";

    Statement insert_st;
    if (sqlite3_prepare_v2(db_, sql.str().c_str(), -1, &insert_st.st, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    for (size_t i = 0; i < fields.size(); ++i) {
        const int bind_index = static_cast<int>(i + 1);
        if (fields[i].type == "null") {
            sqlite3_bind_null(insert_st.st, bind_index);
        } else if (fields[i].type == "integer") {
            sqlite3_bind_int64(insert_st.st, bind_index, std::strtoll(fields[i].value.c_str(), nullptr, 10));
        } else if (fields[i].type == "real") {
            sqlite3_bind_double(insert_st.st, bind_index, std::strtod(fields[i].value.c_str(), nullptr));
        } else {
            sqlite3_bind_text(insert_st.st, bind_index, fields[i].value.c_str(), static_cast<int>(fields[i].value.size()), SQLITE_TRANSIENT);
        }
    }

    if (sqlite3_step(insert_st.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    return true;
}
