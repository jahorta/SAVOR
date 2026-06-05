#pragma once

#include <filesystem>

#include "../ProgramKindDescriptor.h"

namespace simcore::db {
struct IAnalysisDb;
struct IAuthoringDb;
struct IExecutionDb;
}

namespace simcore::db::execution::programdb::battlecontext {

struct BattleContextProbePhaseRegistrationConfig {
    simcore::db::IAuthoringDb* authoring_db = nullptr;
    std::filesystem::path working_dir_root;
};

ProgramKindDescriptor BuildBattleContextProbeDescriptor(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IAnalysisDb* analysis_db,
    BattleContextProbePhaseRegistrationConfig config);

} // namespace simcore::db::execution::programdb::battlecontext
