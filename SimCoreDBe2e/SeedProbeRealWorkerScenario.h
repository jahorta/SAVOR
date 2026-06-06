#pragma once

#include <string>

#include "Cli.h"
#include "Common/DbService.h"

namespace simcore::e2e {

bool RunSeedProbeRealWorkerSmoke(
    const CliOptions& options,
    const char* argv0,
    simcore::db::core::DBService* db_service,
    std::string* error_out);

bool RunSeedProbeWorkflowGraphRealWorkerSmoke(
    const CliOptions& options,
    const char* argv0,
    simcore::db::core::DBService* db_service,
    std::string* error_out);

} // namespace simcore::e2e
