#pragma once

#include <filesystem>

#include "../ProgramKindDescriptor.h"

namespace savor::db {
struct IAnalysisDb;
struct IAuthoringDb;
struct IExecutionDb;
}

namespace savor::db::execution::programdb::battlecontext {

struct BattleContextProbePhaseRegistrationConfig {
    savor::db::IAuthoringDb* authoring_db = nullptr;
    std::filesystem::path working_dir_root;
};

ProgramKindDescriptor BuildBattleContextProbeDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleContextProbePhaseRegistrationConfig config);

} // namespace savor::db::execution::programdb::battlecontext
