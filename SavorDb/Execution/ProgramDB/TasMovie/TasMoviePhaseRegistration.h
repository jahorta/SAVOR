#pragma once

#include "TasMovieAdapters.h"
#include "../ProgramKindRegistry.h"

namespace savor::db::execution::programdb::tasmovie {

void RegisterTasMoviePhaseDescriptor(
    ProgramKindRegistry* registry,
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    TasMoviePhaseRegistrationConfig config);

} // namespace savor::db::execution::programdb::tasmovie
