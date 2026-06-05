#pragma once

#include <string>

#include "Common/DbService.h"
#include "Cli.h"

namespace simcore::e2e {

bool RunTasMovieRealWorkerSmoke(
    const CliOptions& options,
    const char* argv0,
    simcore::db::core::DBService* db_service,
    std::string* error_out);

bool RunTasMovieSeedProbeRealWorkerSmoke(
    const CliOptions& options,
    const char* argv0,
    simcore::db::core::DBService* db_service,
    std::string* error_out);

} // namespace simcore::e2e

