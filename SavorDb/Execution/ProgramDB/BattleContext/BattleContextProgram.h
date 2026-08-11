#pragma once

#include <filesystem>

#include "../ProgramKindDescriptor.h"

namespace savor::db {
struct IAnalysisDb;
struct IExecutionDb;
struct IStateDb;
}

namespace savor::db::execution::programdb::battlecontext {

struct BattleContextProgramConfig {
    std::filesystem::path working_dir_root;
};

ProgramKindDescriptor BuildBattleContextProgramDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleContextProgramConfig config = {});

} // namespace savor::db::execution::programdb::battlecontext
