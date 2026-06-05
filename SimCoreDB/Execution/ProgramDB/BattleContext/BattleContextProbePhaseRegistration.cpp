#include "BattleContextProbePhaseRegistration.h"

namespace simcore::db::execution::programdb::battlecontext {

void RegisterBattleContextProbePhaseDescriptor(
    ProgramKindRegistry* registry,
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IAnalysisDb* analysis_db,
    BattleContextProbePhaseRegistrationConfig config) {
    if (registry == nullptr) {
        return;
    }
    auto descriptor = BuildBattleContextProbeDescriptor(
        execution_db,
        analysis_db,
        std::move(config));
    (void)registry->Register(descriptor);
    (void)registry->RegisterForStepKind("battle.context_probe", descriptor);
}

} // namespace simcore::db::execution::programdb::battlecontext
