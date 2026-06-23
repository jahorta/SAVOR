#pragma once

#include "BattleJobBatchRunOptions.h"

#include <iosfwd>

namespace savor::predict {

int run_battle_jobs(const BattleJobBatchRunOptions& options, std::ostream& out, std::ostream& err);

} // namespace savor::predict
