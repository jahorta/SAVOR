#include "BattleSingleTurnPhaseRegistration.h"

namespace savor::db::execution::programdb::battle {

void RegisterBattleSingleTurnPhaseDescriptor(
    ProgramKindRegistry* registry,
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleSingleTurnPhaseRegistrationConfig config) {
    if (registry == nullptr) {
        return;
    }
    auto descriptor = BuildBattleSingleTurnDescriptor(
        execution_db,
        state_db,
        analysis_db,
        std::move(config));
    (void)registry->Register(descriptor);
    (void)registry->RegisterForStepKind("battle.single_turn", descriptor);
}

} // namespace savor::db::execution::programdb::battle
