#pragma once

#include "../ProgramKindDescriptor.h"

#include <filesystem>

namespace savor::db {
struct IAnalysisDb;
struct IExecutionDb;
struct IStateDb;
}

namespace savor::db::execution::programdb::battlecompletion {

struct BattleCompletionProgramConfig {
    std::filesystem::path working_dir_root;
};

ProgramKindDescriptor BuildBattleCompletionProgramDescriptor(
    IExecutionDb* execution_db,
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    BattleCompletionProgramConfig config = {});

} // namespace savor::db::execution::programdb::battlecompletion
