#pragma once

#include <string>

#include "Common/DbService.h"
#include "Cli.h"

namespace savor::e2e {

bool RunTasMovieRealWorkerSmoke(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out);

bool RunTasMovieSeedProbeRealWorkerSmoke(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out);

} // namespace savor::e2e

