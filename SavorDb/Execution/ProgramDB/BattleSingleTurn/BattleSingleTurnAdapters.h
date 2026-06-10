#pragma once

#include <filesystem>
#include <string>

#include "../ProgramKindDescriptor.h"

namespace savor::db {
struct IAnalysisDb;
struct IAuthoringDb;
struct IExecutionDb;
struct IStateDb;
}

namespace savor::db::execution::programdb::battle {

struct BattleSingleTurnPhaseRegistrationConfig {
    savor::db::IAuthoringDb* authoring_db = nullptr;
    std::filesystem::path working_dir_root;
};

ProgramKindDescriptor BuildBattleSingleTurnDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleSingleTurnPhaseRegistrationConfig config);

} // namespace savor::db::execution::programdb::battle
