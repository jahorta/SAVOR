#pragma once

#include "BattleJobRunOptions.h"

#include <filesystem>
#include <iosfwd>
#include <string>

namespace savor::predict {

struct BattleJobSandboxResult {
    std::filesystem::path run_root;
    std::filesystem::path db_root;
};

int prepare_battle_job_sandbox(
    const BattleJobRunOptions& options,
    BattleJobSandboxResult* result_out,
    std::ostream& out,
    std::ostream& err);

} // namespace savor::predict
