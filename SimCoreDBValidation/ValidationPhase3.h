#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "ValidationResult.h"

struct Phase3DbSeedOptions {
    std::optional<std::string> default_rows_json;
    std::optional<std::filesystem::path> jsonl_folder;
    std::optional<std::filesystem::path> savestate_file;
};

ValidationResult ValidatePhase3ReplayRobustness(const std::filesystem::path& migration_root, const Phase3DbSeedOptions& seed_options);
ValidationResult ValidatePhase3PerServiceDedupeIsolation();
ValidationResult ValidatePhase3ProgressTerminalStreamSeparation(const std::filesystem::path& migration_root);
ValidationResult ValidatePhase3LagDeadLetterReadiness(const std::filesystem::path& migration_root);
