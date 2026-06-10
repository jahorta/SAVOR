#pragma once

#include <string>

#include "Cli.h"
#include "Common/DbService.h"

namespace savor::e2e {

bool RunSeedProbeRealWorkerSmoke(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out);

bool RunSeedProbeWorkflowGraphRealWorkerSmoke(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out);

} // namespace savor::e2e
