#pragma once

#include <filesystem>
#include <string>

#include "../ProgramKindDescriptor.h"

namespace savor::db {
struct IAnalysisDb;
struct IExecutionDb;
struct IStateDb;
}

namespace savor::db::execution::programdb::tasmovieinputepoch {

struct TasMovieInputEpochProgramConfig {
    std::filesystem::path working_dir_root;
    std::string capture_module_sha256;
};

ProgramKindDescriptor BuildAnnotationProgramDescriptor(
    IExecutionDb* execution_db,
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    TasMovieInputEpochProgramConfig config = {});

ProgramKindDescriptor BuildBreakpointDiagnosticProgramDescriptor(
    IExecutionDb* execution_db,
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    TasMovieInputEpochProgramConfig config = {});

ProgramKindDescriptor BuildRewriteProgramDescriptor(
    IExecutionDb* execution_db,
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    TasMovieInputEpochProgramConfig config = {});

ProgramKindDescriptor BuildCutsceneProgramDescriptor(
    IExecutionDb* execution_db,
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    TasMovieInputEpochProgramConfig config = {});

} // namespace savor::db::execution::programdb::tasmovieinputepoch
