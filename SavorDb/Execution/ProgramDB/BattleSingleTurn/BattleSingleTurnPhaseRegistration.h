#pragma once

#include "../ProgramKindRegistry.h"
#include "BattleSingleTurnAdapters.h"

namespace savor::db::execution::programdb::battle {

void RegisterBattleSingleTurnPhaseDescriptor(
    ProgramKindRegistry* registry,
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleSingleTurnPhaseRegistrationConfig config = {});

} // namespace savor::db::execution::programdb::battle
