#include "BattleContextProbePhaseRegistration.h"

namespace savor::db::execution::programdb::battlecontext {

void RegisterBattleContextProbePhaseDescriptor(
    ProgramKindRegistry* registry,
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleContextProbePhaseRegistrationConfig config) {
    if (registry == nullptr) {
        return;
    }
    auto descriptor = BuildBattleContextProbeDescriptor(
        execution_db,
        analysis_db,
        std::move(config));
    (void)registry->Register(descriptor);
    (void)registry->RegisterForStepKind("battle_chain", descriptor);
    (void)registry->RegisterForStepKind("battle.context_probe", descriptor);
}

} // namespace savor::db::execution::programdb::battlecontext
