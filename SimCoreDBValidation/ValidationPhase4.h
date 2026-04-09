#pragma once

#include <filesystem>

#include "ValidationResult.h"

ValidationResult ValidatePhase4InvariantViolationRemediationSequence(const std::filesystem::path& migration_root);
ValidationResult ValidatePhase4PowerLossDuringClaimedJobMaterialization(const std::filesystem::path& migration_root);
ValidationResult ValidatePhase4DuplicateTerminalReplay(const std::filesystem::path& migration_root);
ValidationResult ValidatePhase4PartialWriterFailureRecovery(const std::filesystem::path& migration_root);
ValidationResult ValidatePhase4MissingDecisionResultRestartRerun(const std::filesystem::path& migration_root);
ValidationResult ValidatePhase4ObservabilityRetentionReadiness(const std::filesystem::path& migration_root);
