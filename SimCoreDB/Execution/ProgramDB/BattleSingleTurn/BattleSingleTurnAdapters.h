#pragma once

#include <filesystem>
#include <string>

#include "../ProgramKindDescriptor.h"

namespace simcore::db {
struct IAnalysisDb;
struct IAuthoringDb;
struct IExecutionDb;
struct IStateDb;
}

namespace simcore::db::execution::programdb::battle {

struct BattleSingleTurnPhaseRegistrationConfig {
    simcore::db::IAuthoringDb* authoring_db = nullptr;
    std::filesystem::path working_dir_root;
};

ProgramKindDescriptor BuildBattleSingleTurnDescriptor(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IStateDb* state_db,
    simcore::db::IAnalysisDb* analysis_db,
    BattleSingleTurnPhaseRegistrationConfig config);

} // namespace simcore::db::execution::programdb::battle
