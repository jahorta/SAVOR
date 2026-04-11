#pragma once

#include "SctModel.h"

#include <string>

namespace soasim::sct {

[[nodiscard]] SctParseResult runSctParseProbe(const std::string& sctPath = "<PATH_TO_SCT_FILE>");
[[nodiscard]] std::string formatParseSummary(const SctParseResult& parseResult);

} // namespace soasim::sct
