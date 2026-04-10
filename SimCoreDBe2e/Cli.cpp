#include "Cli.h"

#include <iostream>

namespace simcore::e2e {

std::filesystem::path ResolveWorkerExePath(const char* argv0) {
    const auto exe_path = std::filesystem::absolute(std::filesystem::path(argv0));
    return exe_path.parent_path() / "SimCoreWorker.exe";
}

std::filesystem::path ResolveMigrationRoot(const std::optional<std::filesystem::path>& explicit_root) {
    if (explicit_root.has_value() && !explicit_root->empty() && std::filesystem::exists(*explicit_root / "Execution")) {
        return *explicit_root;
    }

    const std::filesystem::path candidates[] = {
        std::filesystem::path("SimCoreDB") / "migration",
        std::filesystem::path("..") / "SimCoreDB" / "migration",
        std::filesystem::path("..") / ".." / "SimCoreDB" / "migration",
        std::filesystem::path("migration"),
    };

    for (const auto& path : candidates) {
        if (std::filesystem::exists(path / "Execution")) {
            return path;
        }
    }

    return candidates[0];
}

void PrintUsage() {
    std::cout << "SimCoreDBe2e - real-worker end-to-end workflow harness\n\n";
    std::cout << "Usage:\n";
    std::cout << "  SimCoreDBe2e"
              << " --savestate-file <path>"
              << " --iso <path>"
              << " --dolphin-base-dir <path>"
              << " [--scenario seedprobe_real_worker_smoke]"
              << " [--timeout-ms 30000]"
              << " [--poll-ms 100]"
              << " [--migration-root <path>]"
              << " [--workspace-root <path>]"
              << " [--worker-dir-root <path>]\n\n";
}

bool ParseArgs(int argc, char** argv, CliOptions* options_out, std::string* error_out) {
    CliOptions options{};

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto require_value = [&](const char* flag, std::string* out) {
            if (i + 1 >= argc) {
                if (error_out) *error_out = std::string("missing value for ") + flag;
                return false;
            }
            *out = argv[++i];
            return true;
        };

        if (arg == "--scenario") {
            if (!require_value("--scenario", &options.scenario)) return false;
        } else if (arg == "--timeout-ms") {
            std::string v;
            if (!require_value("--timeout-ms", &v)) return false;
            options.timeout_ms = std::stoll(v);
        } else if (arg == "--poll-ms") {
            std::string v;
            if (!require_value("--poll-ms", &v)) return false;
            options.poll_ms = std::stoll(v);
        } else if (arg == "--savestate-file") {
            std::string v;
            if (!require_value("--savestate-file", &v)) return false;
            options.savestate_file = std::filesystem::path(v);
        } else if (arg == "--migration-root") {
            std::string v;
            if (!require_value("--migration-root", &v)) return false;
            options.migration_root = std::filesystem::path(v);
        } else if (arg == "--iso") {
            std::string v;
            if (!require_value("--iso", &v)) return false;
            options.iso_path = std::filesystem::path(v);
        } else if (arg == "--dolphin-base-dir") {
            std::string v;
            if (!require_value("--dolphin-base-dir", &v)) return false;
            options.dolphin_base_dir = std::filesystem::path(v);
        } else if (arg == "--workspace-root") {
            std::string v;
            if (!require_value("--workspace-root", &v)) return false;
            options.workspace_root = std::filesystem::path(v);
        } else if (arg == "--worker-dir-root") {
            std::string v;
            if (!require_value("--worker-dir-root", &v)) return false;
            options.worker_dir_root = std::filesystem::path(v);
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            std::exit(0);
        } else {
            if (error_out) *error_out = "unknown argument: " + arg;
            return false;
        }
    }

    if (options.savestate_file.empty()) {
        if (error_out) *error_out = "--savestate-file is required";
        return false;
    }
    if (!std::filesystem::exists(options.savestate_file)) {
        if (error_out) *error_out = "savestate file does not exist: " + options.savestate_file.string();
        return false;
    }
    if (options.iso_path.empty() || !std::filesystem::exists(options.iso_path)) {
        if (error_out) *error_out = "--iso is required and must exist";
        return false;
    }
    if (options.dolphin_base_dir.empty() || !std::filesystem::exists(options.dolphin_base_dir)) {
        if (error_out) *error_out = "--dolphin-base-dir is required and must exist";
        return false;
    }
    if (options.timeout_ms <= 0 || options.poll_ms <= 0) {
        if (error_out) *error_out = "--timeout-ms and --poll-ms must be > 0";
        return false;
    }

    *options_out = std::move(options);
    return true;
}

} // namespace simcore::e2e
