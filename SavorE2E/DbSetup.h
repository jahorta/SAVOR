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

namespace savor::e2e {

savor::db::DbConfigPaths BuildDbPaths(const CliOptions& options);

bool ResetTasMovieScenarioWorkspace(
    const CliOptions& options,
    std::filesystem::path* workspace_root_out,
    std::string* error_out);

bool CheckWorkflowQuiescence(
    savor::db::IExecutionDb* execution_db,
    std::string* diagnostics_out);

bool SeedStateSavestate(
    savor::db::IStateDb* state_db,
    const std::filesystem::path& savestate_file,
    std::int64_t* savestate_id_out,
    std::string* error_out);

bool SeedStateDtmArtifact(
    savor::db::IStateDb* state_db,
    const std::filesystem::path& dtm_file,
    std::int64_t* artifact_id_out,
    std::string* error_out);

bool SeedAuthoringSpec(
    savor::db::IAuthoringDb* authoring_db,
    const CliOptions& options,
    std::int64_t* seed_probe_spec_id_out,
    std::string* error_out);

bool SeedWorkflowGraphExecution(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    std::int64_t savestate_id,
    std::int64_t seed_probe_spec_id,
    const CliOptions& options,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out);

bool SeedTasMovieWorkflow(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    std::int64_t dtm_artifact_id,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out);

bool SeedTasMovieRootValidationWorkflow(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    std::int64_t establishment_attempt_id,
    std::int64_t rtc_value,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out);

constexpr int kSeedProbeSamplesPerAxis = 5;

} // namespace savor::e2e
