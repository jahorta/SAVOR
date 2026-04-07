#pragma once

#include "../ProgramKindRegistry.h"
#include "SeedProbeGridAdapters.h"
#include "SeedProbeNeutralAdapters.h"
#include "SeedProbeUniqueAdapters.h"

namespace simcore::db::execution::programdb::seedprobe {

struct SeedProbePhaseRegistrationConfig {
    SeedProbeGridBlueprintConfig blueprint{};
    SeedProbeGridSpec grid{};
    UniqueIni unique{};
    SeedProbeGridResultMapper::ContextLookupFn grid_context_lookup{};
    SeedProbeUniqueTransitionHandler::CompletionGateFn unique_completion_gate{};
};

void RegisterSeedProbePhaseDescriptors(
    ProgramKindRegistry* registry,
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IAnalysisDb* analysis_db,
    SeedProbePhaseRegistrationConfig config = {});

} // namespace simcore::db::execution::programdb::seedprobe
