#pragma once

#include <string>

#include "Cli.h"
#include "ScenarioEntry.h"

namespace savor::db::core {
class DBService;
}

namespace savor::e2e {

bool RunBattleWorkflowGraphRealWorkerScenario(
    const CliOptions& options,
    const ResolvedE2eScenarioEntry& entry,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out);

} // namespace savor::e2e
