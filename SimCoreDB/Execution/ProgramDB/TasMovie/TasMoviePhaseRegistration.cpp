#include "TasMoviePhaseRegistration.h"

namespace simcore::db::execution::programdb::tasmovie {

void RegisterTasMoviePhaseDescriptor(
    ProgramKindRegistry* registry,
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IStateDb* state_db,
    simcore::db::IAnalysisDb* analysis_db,
    TasMoviePhaseRegistrationConfig config) {
    if (registry == nullptr) {
        return;
    }

    auto descriptor = BuildTasMovieDescriptor(
        execution_db,
        state_db,
        analysis_db,
        std::move(config));
    (void)registry->Register(descriptor);
    (void)registry->RegisterForStepKind("tasmovie.play", descriptor);
}

} // namespace simcore::db::execution::programdb::tasmovie

