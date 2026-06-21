#pragma once

#include "BattleJobRunManifest.h"
#include "BattleJobRunOptions.h"

#include <iosfwd>

namespace savor::predict {

int run_battle_job(const BattleJobRunOptions& options, std::ostream& out, std::ostream& err);

} // namespace savor::predict
