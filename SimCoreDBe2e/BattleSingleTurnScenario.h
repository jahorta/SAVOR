#pragma once

#include <string>

#include "Cli.h"

namespace simcore::db::core {
class DBService;
}

namespace simcore::e2e {

bool RunBattleSingleTurnRealWorkerScenario(
    const CliOptions& options,
    const char* argv0,
    simcore::db::core::DBService* db_service,
    std::string* error_out);

bool RunTasMovieSeedProbeBattleWorkflowGraphRealWorkerScenario(
    const CliOptions& options,
    const char* argv0,
    simcore::db::core::DBService* db_service,
    std::string* error_out);

bool RunTasMovieSeedProbeBattleOverrideWorkflowGraphRealWorkerScenario(
    const CliOptions& options,
    const char* argv0,
    simcore::db::core::DBService* db_service,
    std::string* error_out);

bool RunTasMovieBattleWorkflowGraphRealWorkerScenario(
    const CliOptions& options,
    const char* argv0,
    simcore::db::core::DBService* db_service,
    std::string* error_out);

} // namespace simcore::e2e
