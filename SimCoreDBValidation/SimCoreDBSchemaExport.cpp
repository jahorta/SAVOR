#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "ValidationPhase3.h"
#include "ValidationPhase4.h"
#include "ValidationResult.h"
#include "Common/Migrations/MigrationRunner.h"
#include "Common/Events/EventPayloadDispatch.h"
#include "Common/Events/EventPayloadValidation.h"
#include "Common/Events/OutboxRelay.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeNeutralAdapters.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeUniqueAdapters.h"
#include "Execution/Workflow/AdapterChainOrchestrator.h"
#include "Execution/Workflow/SqliteExecutionDb.h"
#include "Execution/Workflow/SeedProbeWorkflowDefinition.h"
#include "ExportCurrentDbSchemas.h"

namespace {

using simcore::db::events::EventEnvelope;
using simcore::db::events::OutboxRelay;
using simcore::db::events::OutboxRelayDispatchBinding;
using simcore::db::events::OutboxRelayResult;

std::filesystem::path ResolveMigrationRoot(std::optional<std::filesystem::path> explicit_root) {
    if (explicit_root.has_value() && !explicit_root->empty()) {
        auto provided = explicit_root.value();
        if (std::filesystem::exists(provided / "Execution")) {
            return provided;
        }

        // Accept Windows-style separators even when running on POSIX hosts.
        std::string normalized = provided.generic_string();
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        provided = std::filesystem::path(normalized);
        if (std::filesystem::exists(provided / "Execution")) {
            return provided;
        }
    }

    const std::vector<std::filesystem::path> candidates{
        std::filesystem::path("SimCoreDB") / "migration",
        std::filesystem::path("..") / "SimCoreDB" / "migration",
        std::filesystem::path("..") / ".." / "SimCoreDB" / "migration",
        std::filesystem::path("..") / ".." / ".." / "SimCoreDB" / "migration",
        std::filesystem::path("migration"),
    };

    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate / "Execution")) {
            return candidate;
        }
    }

    return candidates.front();
}

void PrintUsage(const std::map<std::string, std::string>& validations) {
    std::cout << "SimCoreDBValidation - SimCoreDB workflow migration validation tool\n\n";
    std::cout << "Usage:\n";
    std::cout << "  SimCoreDBValidation --list\n";
    std::cout << "  SimCoreDBValidation --run <validation-name|all> [--migration-root <path>] [--savestate-file <path>] [--phase3-default-rows-json <path>] [--phase3-jsonl-dir <path>]\n\n";
    std::cout << "Available validations:\n";
    for (const auto& [name, desc] : validations) {
        std::cout << "  - " << name << ": " << desc << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {

    bool list_only = false;
    std::string run_target = "all";
    std::optional<std::filesystem::path> migration_root_override;
    std::optional<std::filesystem::path> savestate_file;
    std::optional<std::filesystem::path> phase3_default_rows_json_path;
    std::optional<std::filesystem::path> phase3_jsonl_dir;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--migration-root") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for --migration-root\n";
                return 2;
            }
            migration_root_override = std::filesystem::path(argv[++i]);
        }
    }

    const auto migration_root = ResolveMigrationRoot(migration_root_override);
    std::string schema_export_error;
    if (!ExportCurrentDbSchemas(migration_root, &schema_export_error)) {
        std::cerr << "failed exporting current db schemas: " << schema_export_error << "\n";
        return 1;
    }

    return 0;
}
