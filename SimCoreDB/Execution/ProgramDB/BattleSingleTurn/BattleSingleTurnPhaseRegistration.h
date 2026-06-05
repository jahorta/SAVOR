#pragma once

#include "../ProgramKindRegistry.h"
#include "BattleSingleTurnAdapters.h"

namespace simcore::db::execution::programdb::battle {

void RegisterBattleSingleTurnPhaseDescriptor(
    ProgramKindRegistry* registry,
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IStateDb* state_db,
    simcore::db::IAnalysisDb* analysis_db,
    BattleSingleTurnPhaseRegistrationConfig config = {});

} // namespace simcore::db::execution::programdb::battle
