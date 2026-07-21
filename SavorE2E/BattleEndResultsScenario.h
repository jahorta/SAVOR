#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "Cli.h"

namespace savor::db::core {
class DBService;
}

namespace savor::e2e {

enum class BattleEndSeedSelector {
    Neutral,
    SeedValue,
    SeedDelta,
};

struct BattleEndWorkflowLaunchOptions {
    std::int64_t source_victory_savestate_id = 0;
    std::int64_t seed_probe_spec_id = 0;
    BattleEndSeedSelector seed_selector = BattleEndSeedSelector::Neutral;
    std::optional<std::int64_t> seed_selector_value;
};

// Explicit E2E entry point for the coordinator-owned battle-end workflow.
// The source savestate must already be the Victory output of a successful
// BattleSingleTurn execution recorded in the same database set.
bool RunBattleEndResultsScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out);

} // namespace savor::e2e
