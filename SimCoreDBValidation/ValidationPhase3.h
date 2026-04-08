#pragma once

#include <filesystem>

#include "ValidationResult.h"

ValidationResult ValidatePhase3ReplayRobustness(const std::filesystem::path& migration_root);
ValidationResult ValidatePhase3PerServiceDedupeIsolation();
ValidationResult ValidatePhase3ProgressTerminalStreamSeparation(const std::filesystem::path& migration_root);
ValidationResult ValidatePhase3LagDeadLetterReadiness(const std::filesystem::path& migration_root);
