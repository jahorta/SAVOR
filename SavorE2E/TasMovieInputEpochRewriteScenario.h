#pragma once

#include <string>

#include "Cli.h"
#include "Common/DbService.h"
#include "ScenarioEntry.h"

namespace savor::e2e {

bool RunTasMovieInputEpochRewriteRealWorkerScenario(
    const CliOptions& options,
    const ResolvedE2eScenarioEntry& entry,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out);

} // namespace savor::e2e
