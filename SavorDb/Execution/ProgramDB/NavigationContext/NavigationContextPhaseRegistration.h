#pragma once

#include "../ProgramKindRegistry.h"
#include "NavigationContextAdapters.h"

namespace savor::db::execution::programdb::navigationcontext {

void RegisterNavigationContextProbePhaseDescriptor(
    ProgramKindRegistry* registry,
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    NavigationContextPhaseRegistrationConfig config = {});

} // namespace savor::db::execution::programdb::navigationcontext
