#include "BattleEndResultsPhaseRegistration.h"

#include <utility>

namespace savor::db::execution::programdb::battleend {

void RegisterBattleEndWorkflowPhaseDescriptors(
    ProgramKindRegistry* registry,
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleEndWorkflowPhaseRegistrationConfig config) {
    if (registry == nullptr) {
        return;
    }
    auto completion = BuildBattleCompletionDescriptor(
        execution_db,
        state_db,
        analysis_db,
        config);
    auto field_return = BuildFieldReturnSeedProbeDescriptor(
        execution_db,
        state_db,
        analysis_db,
        config);
    auto field_return_grid = BuildFieldReturnSeedProbeGridDescriptor(
        execution_db,
        state_db,
        analysis_db,
        config);
    auto field_return_unique = BuildFieldReturnSeedProbeUniqueDescriptor(
        execution_db,
        state_db,
        analysis_db,
        config);
    auto field_return_materialize = BuildFieldReturnSeedMaterializeDescriptor(
        execution_db,
        state_db,
        analysis_db,
        config);
    auto results = BuildBattleResultsScreenDescriptor(
        execution_db,
        state_db,
        analysis_db,
        std::move(config));
    (void)registry->Register(completion);
    (void)registry->Register(field_return);
    (void)registry->Register(results);
    (void)registry->RegisterForStepKind("battle.completion", completion);
    (void)registry->RegisterForStepKind("battle.field_return_seed_probe", field_return);
    (void)registry->RegisterForStepKind("battle.field_return_seed_probe.grid", field_return_grid);
    (void)registry->RegisterForStepKind("battle.field_return_seed_probe.unique", field_return_unique);
    (void)registry->RegisterForStepKind("battle.field_return_seed_probe.materialize", field_return_materialize);
    (void)registry->RegisterForStepKind("battle.results_screen", results);
}

void RegisterBattleEndResultsPhaseDescriptor(
    ProgramKindRegistry* registry,
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleEndWorkflowPhaseRegistrationConfig config) {
    RegisterBattleEndWorkflowPhaseDescriptors(
        registry,
        execution_db,
        state_db,
        analysis_db,
        std::move(config));
}

} // namespace savor::db::execution::programdb::battleend
