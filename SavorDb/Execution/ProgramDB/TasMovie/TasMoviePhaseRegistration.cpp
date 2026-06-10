#include "TasMoviePhaseRegistration.h"

namespace savor::db::execution::programdb::tasmovie {

void RegisterTasMoviePhaseDescriptor(
    ProgramKindRegistry* registry,
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
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
    (void)registry->RegisterForStepKind("tas_movie", descriptor);
    (void)registry->RegisterForStepKind("tasmovie.play", descriptor);
}

} // namespace savor::db::execution::programdb::tasmovie
