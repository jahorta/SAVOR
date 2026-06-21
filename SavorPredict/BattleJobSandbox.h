#pragma once

#include "BattleJobRunOptions.h"

#include "DbRootCopy.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace savor::predict {

struct BattleJobSandboxResult {
    std::filesystem::path run_root;
    std::filesystem::path db_root;
    savor::dbutils::SandboxMode sandbox_mode = savor::dbutils::SandboxMode::MinimalBattleSingleTurn;
    std::vector<savor::dbutils::TableCopyCount> table_counts;
    std::vector<savor::dbutils::CopiedArtifactFile> copied_artifacts;
    std::vector<std::string> validation_errors;
};

int prepare_battle_job_sandbox(
    const BattleJobRunOptions& options,
    BattleJobSandboxResult* result_out,
    std::ostream& out,
    std::ostream& err);

} // namespace savor::predict
