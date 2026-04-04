#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "MigrationContext.h"

namespace simcore::db::migrations {

struct MigrationEntry {
    std::string name;
    std::filesystem::path path;
    std::string sql;
};

using MigrationExecutor = std::function<bool(MigrationContext, const MigrationEntry&, std::string* error_out)>;

std::vector<MigrationContext> ListAllMigrationContexts();

std::vector<MigrationEntry> LoadContextMigrations(const std::filesystem::path& migration_root, MigrationContext context);

bool RunContextMigrations(
    const std::filesystem::path& migration_root,
    MigrationContext context,
    const MigrationExecutor& exec,
    std::string* error_out = nullptr);

} // namespace simcore::db::migrations
