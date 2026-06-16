#pragma once

#include <string>

#include "Cli.h"

namespace savor::db::core {
class DBService;
}

namespace savor::e2e {

bool RunSeedProbeBattleRealWorkerScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out);

bool RunBattleWorkflowGraphRealWorkerScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out);

bool RunTasMovieSeedProbeBattleWorkflowGraphRealWorkerScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out);

bool RunTasMovieSeedProbeBattleOverrideWorkflowGraphRealWorkerScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out);

bool RunTasMovieBattleWorkflowGraphRealWorkerScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out);

} // namespace savor::e2e
