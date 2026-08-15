#pragma once

#include "../ProgramKindDescriptor.h"

#include <filesystem>

namespace savor::db {
struct IAnalysisDb;
struct IExecutionDb;
struct IStateDb;
}

namespace savor::db::execution::programdb::battlereplay {

struct BattleReplayProgramConfig {
    std::filesystem::path working_dir_root;
};

ProgramKindDescriptor BuildBattleReplayProgramDescriptor(
    IExecutionDb* execution_db,
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    BattleReplayProgramConfig config = {});

} // namespace savor::db::execution::programdb::battlereplay
