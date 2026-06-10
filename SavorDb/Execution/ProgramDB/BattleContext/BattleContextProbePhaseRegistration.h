#pragma once

#include "BattleContextProbeAdapters.h"
#include "../ProgramKindRegistry.h"

namespace savor::db::execution::programdb::battlecontext {

void RegisterBattleContextProbePhaseDescriptor(
    ProgramKindRegistry* registry,
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleContextProbePhaseRegistrationConfig config = {});

} // namespace savor::db::execution::programdb::battlecontext
