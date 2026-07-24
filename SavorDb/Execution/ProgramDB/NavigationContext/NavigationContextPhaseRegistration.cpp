#include "NavigationContextPhaseRegistration.h"

#include <utility>

namespace savor::db::execution::programdb::navigationcontext {

void RegisterNavigationContextProbePhaseDescriptor(
    ProgramKindRegistry* registry,
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    NavigationContextPhaseRegistrationConfig config) {
    if (registry == nullptr) {
        return;
    }

    auto descriptor = BuildNavigationContextProbeDescriptor(
        execution_db,
        state_db,
        std::move(config));
    (void)registry->Register(descriptor);
    (void)registry->RegisterForStepKind("navigation.context_probe", descriptor);
}

} // namespace savor::db::execution::programdb::navigationcontext
