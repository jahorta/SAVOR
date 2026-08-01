#pragma once

#include <cstddef>
#include <filesystem>

#include "../ProgramKindDescriptor.h"

namespace savor::db {
struct IAnalysisDb;
struct IAuthoringDb;
struct IExecutionDb;
struct IStateDb;
}

namespace savor::db::execution::programdb::seedprobe {

struct SeedProbeProgramConfig {
    std::filesystem::path working_dir_root;
    std::size_t maximum_items_per_workset = 16;
};

ProgramKindDescriptor BuildSeedProbeProgramDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    savor::db::IAuthoringDb* authoring_db,
    SeedProbeProgramConfig config = {});

} // namespace savor::db::execution::programdb::seedprobe
