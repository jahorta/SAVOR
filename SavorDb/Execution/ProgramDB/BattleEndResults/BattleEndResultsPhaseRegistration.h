#pragma once

#include "../ProgramKindRegistry.h"
#include "BattleEndResultsAdapters.h"

namespace savor::db::execution::programdb::battleend {

void RegisterBattleEndWorkflowPhaseDescriptors(
    ProgramKindRegistry* registry,
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleEndWorkflowPhaseRegistrationConfig config = {});

void RegisterBattleEndResultsPhaseDescriptor(
    ProgramKindRegistry* registry,
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleEndWorkflowPhaseRegistrationConfig config = {});

} // namespace savor::db::execution::programdb::battleend
