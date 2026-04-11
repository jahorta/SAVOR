#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "Authoring/IAuthoringDb.h"
#include "Analysis/IAnalysisDb.h"
#include "Common/DbConfigPaths.h"
#include "Execution/Workflow/SqliteExecutionDb.h"
#include "State/IStateDb.h"

#include "Cli.h"

namespace simcore::e2e {

simcore::db::DbConfigPaths BuildDbPaths(const CliOptions& options);

bool SeedStateSavestate(
    simcore::db::IStateDb* state_db,
    const std::filesystem::path& savestate_file,
    std::int64_t* savestate_id_out,
    std::string* error_out);

bool SeedAuthoringSpec(
    simcore::db::IAuthoringDb* authoring_db,
    std::int64_t* seed_probe_spec_id_out,
    std::string* error_out);

bool SeedExecutionWorkflow(
    simcore::db::IAnalysisDb* analysis_db,
    simcore::db::execution::workflow::SqliteExecutionDb* execution_db,
    std::int64_t savestate_id,
    std::int64_t seed_probe_spec_id,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out);

constexpr int kSeedProbeSamplesPerAxis = 5;

} // namespace simcore::e2e
