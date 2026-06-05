#include "BattleSingleTurnPhaseRegistration.h"

namespace simcore::db::execution::programdb::battle {

void RegisterBattleSingleTurnPhaseDescriptor(
    ProgramKindRegistry* registry,
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IStateDb* state_db,
    simcore::db::IAnalysisDb* analysis_db,
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

} // namespace simcore::db::execution::programdb::battle
