#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "Authoring/IAuthoringDb.h"
#include "Analysis/IAnalysisDb.h"
#include "Common/DbConfigPaths.h"
#include "Execution/IExecutionDb.h"
#include "State/IStateDb.h"

#include "Cli.h"

namespace simcore::e2e {

simcore::db::DbConfigPaths BuildDbPaths(const CliOptions& options);

bool SeedStateSavestate(
    simcore::db::IStateDb* state_db,
    const std::filesystem::path& savestate_file,
    std::int64_t* savestate_id_out,
    std::string* error_out);

bool SeedStateDtmArtifact(
    simcore::db::IStateDb* state_db,
    const std::filesystem::path& dtm_file,
    std::int64_t* artifact_id_out,
    std::string* error_out);

bool SeedAuthoringSpec(
    simcore::db::IAuthoringDb* authoring_db,
    std::int64_t* seed_probe_spec_id_out,
    std::string* error_out);

bool SeedExecutionWorkflow(
    simcore::db::IAnalysisDb* analysis_db,
    simcore::db::IExecutionDb* execution_db,
    std::int64_t savestate_id,
    std::int64_t seed_probe_spec_id,
    std::int64_t* workflow_instance_id_out,
    std::int64_t* probe_run_id_out,
    std::string* error_out);

bool SeedTasMovieWorkflow(
    simcore::db::IExecutionDb* execution_db,
    std::int64_t dtm_artifact_id,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out);

bool SeedTasMovieSeedProbeWorkflow(
    simcore::db::IAnalysisDb* analysis_db,
    simcore::db::IExecutionDb* execution_db,
    std::int64_t placeholder_savestate_id,
    std::int64_t seed_probe_spec_id,
    std::int64_t* workflow_instance_id_out,
    std::int64_t* probe_run_id_out,
    std::string* error_out);

constexpr int kSeedProbeSamplesPerAxis = 5;

} // namespace simcore::e2e
