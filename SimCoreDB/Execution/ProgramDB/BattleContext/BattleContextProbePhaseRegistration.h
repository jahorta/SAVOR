#pragma once

#include "BattleContextProbeAdapters.h"
#include "../ProgramKindRegistry.h"

namespace simcore::db::execution::programdb::battlecontext {

void RegisterBattleContextProbePhaseDescriptor(
    ProgramKindRegistry* registry,
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IAnalysisDb* analysis_db,
    BattleContextProbePhaseRegistrationConfig config = {});

} // namespace simcore::db::execution::programdb::battlecontext
