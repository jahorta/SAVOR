#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace simcore::e2e {

struct CliOptions {
    std::string scenario = "seedprobe_real_worker_smoke";
    std::int64_t timeout_ms = 30000;
    std::int64_t poll_ms = 100;
    std::filesystem::path savestate_file;
    std::filesystem::path iso_path;
    std::filesystem::path dolphin_base_dir;
    std::optional<std::filesystem::path> migration_root;
    std::optional<std::filesystem::path> workspace_root;
    std::optional<std::filesystem::path> worker_dir_root;
};

void PrintUsage();
bool ParseArgs(int argc, char** argv, CliOptions* options_out, std::string* error_out);
std::filesystem::path ResolveWorkerExePath(const char* argv0);
std::filesystem::path ResolveMigrationRoot(const std::optional<std::filesystem::path>& explicit_root);

} // namespace simcore::e2e
