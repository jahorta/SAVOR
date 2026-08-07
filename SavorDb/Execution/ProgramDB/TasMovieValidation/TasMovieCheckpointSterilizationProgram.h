#pragma once

#include <filesystem>

#include "../ProgramKindDescriptor.h"

namespace savor::db {
struct IAnalysisDb;
struct IExecutionDb;
struct IStateDb;
}

namespace savor::db::execution::programdb::tasmoviecheckpointsterilization {

struct TasMovieCheckpointSterilizationProgramConfig {
    std::filesystem::path working_dir_root;
};

ProgramKindDescriptor BuildTasMovieCheckpointSterilizationProgramDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    TasMovieCheckpointSterilizationProgramConfig config = {});

} // namespace savor::db::execution::programdb::tasmoviecheckpointsterilization
