#pragma once

#include <string>

#include "Cli.h"

namespace simcore::e2e {

bool RunSeedProbeRealWorkerSmoke(const CliOptions& options, const char* argv0, std::string* error_out);

} // namespace simcore::e2e
