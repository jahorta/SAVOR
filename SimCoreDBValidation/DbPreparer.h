#pragma once

#include <filesystem>

#include "sqlite3.h"

class DbPreparer {
public:
	DbPreparer(sqlite3* db, std::filesystem::path migration_root);


	bool InitializeRequiredMigrations(std::string* error_out) const;
    bool SeedRowsFromJsonObject(const std::string& json_object, std::string* error_out) const;
    bool SeedRowsFromJsonlDirectory(const std::filesystem::path& folder, std::string* error_out) const;
    bool SeedSavestateArtifactAndOverride(const std::filesystem::path& savestate_file, std::string* error_out);


private:
    bool SeedRowsForTableArray(const std::string& table_name, const std::string& rows_json, std::string* error_out) const;
    bool InsertJsonObjectRow(const std::string& table_name, const std::string& row_json, std::string* error_out) const;
    const std::string* OverrideValueForColumn(const std::string& table_name, const std::string& column_name) const;
    bool IsSavestateIdColumn(const std::string& column_name) const;

    sqlite3* db_;
    std::filesystem::path migration_root_;
    std::optional<std::int64_t> savestate_override_id_;
    std::optional<std::int64_t> savestate_override_artifact_id_;
    mutable std::string savestate_override_cache_;
    mutable std::string artifact_override_cache_;
};