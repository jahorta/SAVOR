#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "MigrationContext.h"

namespace simcore::db::migrations {

struct MigrationEntry {
    MigrationContext context{};
    std::string name;
    std::filesystem::path path;
    std::string sql;
};

enum class MigrationSourceKind {
    Embedded,
    Filesystem,
};

struct MigrationSourceOptions {
    MigrationSourceKind source_kind = MigrationSourceKind::Embedded;
    std::filesystem::path filesystem_root;
};

std::vector<MigrationContext> ListAllMigrationContexts();

std::vector<MigrationEntry> LoadContextMigrations(
    MigrationContext context,
    const MigrationSourceOptions& options = {});

bool EnsureMigrationTrackingTables(sqlite3* db, std::string* error_out = nullptr);

bool HasMigrationBeenApplied(
    sqlite3* db,
    MigrationContext context,
    const std::string& migration_name,
    bool* applied_out,
    std::string* error_out = nullptr);

std::optional<int> GetCurrentContextSchemaVersion(
    sqlite3* db,
    MigrationContext context,
    std::string* error_out = nullptr);

bool ApplyContextMigrations(
    sqlite3* db,
    MigrationContext context,
    const MigrationSourceOptions& options = {},
    std::string* error_out = nullptr);

} // namespace simcore::db::migrations
