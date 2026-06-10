#pragma once

#include "../ProgramKindRegistry.h"
#include "../../../Authoring/IAuthoringDb.h"
#include "SeedProbeGridAdapters.h"
#include "SeedProbeNeutralAdapters.h"
#include "SeedProbeUniqueAdapters.h"

namespace savor::db::execution::programdb::seedprobe {

struct SeedProbePhaseRegistrationConfig {
    savor::db::IAuthoringDb* authoring_db = nullptr;
    SeedProbeGridBlueprintConfig blueprint{};
    SeedProbeGridSpec grid{};
    UniqueIni unique{};
    SeedProbeUniqueTransitionHandler::CompletionGateFn unique_completion_gate{};
};

void RegisterSeedProbePhaseDescriptors(
    ProgramKindRegistry* registry,
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    SeedProbePhaseRegistrationConfig config = {});

} // namespace savor::db::execution::programdb::seedprobe
