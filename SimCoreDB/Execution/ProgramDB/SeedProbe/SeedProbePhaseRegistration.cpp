#include "SeedProbePhaseRegistration.h"

namespace simcore::db::execution::programdb::seedprobe {

void RegisterSeedProbePhaseDescriptors(
    ProgramKindRegistry* registry,
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IAnalysisDb* analysis_db,
    SeedProbePhaseRegistrationConfig config) {
    if (registry == nullptr) {
        return;
    }

    auto neutral = BuildSeedProbeNeutralDescriptor(execution_db, analysis_db, config.authoring_db);
    auto grid = BuildSeedProbeGridDescriptor(
        execution_db,
        analysis_db,
        config.blueprint,
        config.grid,
        config.authoring_db);
    auto unique = BuildSeedProbeUniqueDescriptor(
        execution_db,
        analysis_db,
        config.blueprint,
        config.unique,
        std::move(config.unique_completion_gate),
        config.authoring_db);

    (void)registry->Register(neutral);
    (void)registry->RegisterForStepKind("seedprobe.neutral", neutral);
    (void)registry->RegisterForStepKind("seedprobe.grid", grid);
    (void)registry->RegisterForStepKind("seedprobe.unique", unique);
}

} // namespace simcore::db::execution::programdb::seedprobe
