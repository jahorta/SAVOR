#pragma once

#include <filesystem>

#include "../ProgramKindDescriptor.h"

namespace savor::db {
struct IAnalysisDb;
struct IExecutionDb;
struct IStateDb;
}

namespace savor::db::execution::programdb::tasmovievalidation {

struct TasMovieValidationProgramConfig {
    std::filesystem::path working_dir_root;
};

ProgramKindDescriptor BuildTasMovieValidationProgramDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    TasMovieValidationProgramConfig config = {});

} // namespace savor::db::execution::programdb::tasmovievalidation
