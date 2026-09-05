#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace savor::db {

struct WorkspaceStagingCleanupSummary {
    std::uint64_t files = 0;
    std::uint64_t directories = 0;
    std::uint64_t bytes = 0;
};

bool ResetWorkspaceStagingDirectories(
    const std::filesystem::path& workspace_root,
    std::span<const std::filesystem::path> relative_directories,
    WorkspaceStagingCleanupSummary* summary_out = nullptr,
    std::string* error_out = nullptr);

} // namespace savor::db
