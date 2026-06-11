#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "ExportCurrentDbSchemas.h"

namespace {

bool IsCompleteMigrationRoot(const std::filesystem::path& root, std::string* error_out = nullptr) {
    if (!std::filesystem::exists(root) || !std::filesystem::is_directory(root)) {
        if (error_out != nullptr) {
            *error_out = "path is not a directory: " + root.string();
        }
        return false;
    }

    const std::vector<std::string> required_contexts{
        "Execution",
        "State",
        "AnalysisSpine",
        "AnalysisSeedProbe",
        "AnalysisBattle",
        "Authoring",
        "UIRead",
        "Archive",
    };

    for (const auto& context : required_contexts) {
        const auto context_dir = root / context;
        if (!std::filesystem::exists(context_dir) || !std::filesystem::is_directory(context_dir)) {
            if (error_out != nullptr) {
                *error_out = "migration root is missing context folder: " + context_dir.string();
            }
            return false;
        }
    }

    return true;
}

std::filesystem::path ToAbsoluteExistingPath(const std::filesystem::path& path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical;
    }
    return std::filesystem::absolute(path);
}

bool ResolveMigrationRoot(
    const std::optional<std::filesystem::path>& explicit_root,
    std::filesystem::path* migration_root_out,
    std::string* error_out) {
    if (migration_root_out == nullptr) {
        return false;
    }

    if (explicit_root.has_value() && !explicit_root->empty()) {
        const auto provided = ToAbsoluteExistingPath(*explicit_root);
        if (!IsCompleteMigrationRoot(provided, error_out)) {
            return false;
        }

        *migration_root_out = provided;
        return true;
    }

    const std::vector<std::filesystem::path> candidates{
        std::filesystem::path("SavorDb") / "migration",
        std::filesystem::path("..") / "SavorDb" / "migration",
        std::filesystem::path("..") / ".." / "SavorDb" / "migration",
        std::filesystem::path("..") / ".." / ".." / "SavorDb" / "migration",
        std::filesystem::path("migration"),
    };

    for (const auto& candidate : candidates) {
        const auto absolute_candidate = ToAbsoluteExistingPath(candidate);
        if (IsCompleteMigrationRoot(absolute_candidate)) {
            *migration_root_out = absolute_candidate;
            return true;
        }
    }

    if (error_out != nullptr) {
        *error_out = "could not locate a complete SavorDb migration root; pass --migration <path-to-SavorDb\\migration>";
    }
    return false;
}

void PrintUsage() {
    std::cout << "SavorDbSchemaExport - SavorDb schema snapshot exporter\n\n";
    std::cout << "Usage:\n";
    std::cout << "  SavorDbSchemaExport --migration <path-to-SavorDb\\migration>\n";
    std::cout << "  SavorDbSchemaExport [--migration-root <path-to-SavorDb\\migration>]\n\n";
    std::cout << "--migration-root is kept as a compatibility alias for --migration.\n";
}

} // namespace

int main(int argc, char** argv) {

    std::optional<std::filesystem::path> migration_root_override;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return 0;
        }

        if (arg == "--migration" || arg == "--migration-root") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << arg << "\n";
                return 2;
            }
            migration_root_override = std::filesystem::path(argv[++i]);
            continue;
        }

        std::cerr << "unknown argument: " << arg << "\n";
        PrintUsage();
        return 2;
    }

    std::filesystem::path migration_root;
    std::string migration_root_error;
    if (!ResolveMigrationRoot(migration_root_override, &migration_root, &migration_root_error)) {
        std::cerr << "failed resolving migration root: " << migration_root_error << "\n";
        return 2;
    }

    std::string schema_export_error;
    if (!ExportCurrentDbSchemas(migration_root, &schema_export_error)) {
        std::cerr << "failed exporting current db schemas: " << schema_export_error << "\n";
        return 1;
    }

    std::cout << "\nDB Schemas Updated Successfully from: " << migration_root << "\n";

    return 0;
}
