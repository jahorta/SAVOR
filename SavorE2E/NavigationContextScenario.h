#pragma once

#include <string>

#include "Cli.h"

namespace savor::db::core {
class DBService;
}

namespace savor::e2e {

// Coordinator-owned entry point for a standalone Navigation Context capture.
// The source may be any complete savestate already recorded in StateDB.
bool RunNavigationContextScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out);

} // namespace savor::e2e
