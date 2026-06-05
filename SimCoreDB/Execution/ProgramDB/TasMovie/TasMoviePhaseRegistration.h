#pragma once

#include "TasMovieAdapters.h"
#include "../ProgramKindRegistry.h"

namespace simcore::db::execution::programdb::tasmovie {

void RegisterTasMoviePhaseDescriptor(
    ProgramKindRegistry* registry,
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IStateDb* state_db,
    simcore::db::IAnalysisDb* analysis_db,
    TasMoviePhaseRegistrationConfig config);

} // namespace simcore::db::execution::programdb::tasmovie

