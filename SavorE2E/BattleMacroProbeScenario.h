#pragma once

#include <string>

#include "Cli.h"

namespace savor::db::core {
class DBService;
}

namespace savor::e2e {

bool RunBattleMacroProbeScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out);

} // namespace savor::e2e
