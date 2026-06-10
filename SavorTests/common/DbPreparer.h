#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <sqlite3.h>

class DbPreparer {
public:
    DbPreparer(sqlite3* db, std::filesystem::path migration_root);

    bool InitializeRequiredMigrations(std::string* error_out = nullptr) const;
    bool SeedSavestateArtifactAndOverride(const std::filesystem::path& savestate_path, std::string* error_out = nullptr);
    bool SeedRowsFromJsonObject(const std::string& json_object, std::string* error_out = nullptr) const;
    bool SeedRowsFromJsonlDirectory(const std::filesystem::path& jsonl_dir, std::string* error_out = nullptr) const;

private:
    bool SeedRowsForTableFromJsonArrayText(
        const std::string& table_name,
        const std::string& json_array_text,
        std::string* error_out = nullptr) const;
    bool InsertJsonRow(const std::string& table_name, const std::string& row_json, std::string* error_out = nullptr) const;

    sqlite3* db_ = nullptr;
    std::filesystem::path migration_root_;
    std::optional<std::filesystem::path> savestate_override_path_;
};
