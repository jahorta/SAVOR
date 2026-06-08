#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "Authoring/IAuthoringDb.h"
#include "Analysis/IAnalysisDb.h"
#include "Common/DbConfigPaths.h"
#include "Execution/IExecutionDb.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/Workflow/WorkflowCoordinatorService.h"
#include "State/IStateDb.h"

#include "Cli.h"

namespace simcore::e2e {

simcore::db::DbConfigPaths BuildDbPaths(const CliOptions& options);

class ScopedWorkflowCoordinatorService {
public:
    using EventLineCallback = simcore::db::execution::workflow::WorkflowCoordinatorService::EventLineCallback;

    ScopedWorkflowCoordinatorService() = default;
    ~ScopedWorkflowCoordinatorService();

    ScopedWorkflowCoordinatorService(const ScopedWorkflowCoordinatorService&) = delete;
    ScopedWorkflowCoordinatorService& operator=(const ScopedWorkflowCoordinatorService&) = delete;

    bool Start(
        simcore::db::IExecutionDb* execution_db,
        simcore::db::IAuthoringDb* authoring_db,
        const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
        const CliOptions& options,
        std::string* error_out,
        EventLineCallback event_line_callback = {});
    void Stop();
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] simcore::db::execution::workflow::WorkflowCoordinatorTelemetry SnapshotTelemetry() const;

private:
    std::unique_ptr<simcore::db::execution::workflow::WorkflowCoordinatorService> service_;
};

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

bool SeedWorkflowGraphExecution(
    simcore::db::IAuthoringDb* authoring_db,
    simcore::db::IExecutionDb* execution_db,
    std::int64_t savestate_id,
    std::int64_t seed_probe_spec_id,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out);

bool SeedTasMovieWorkflow(
    simcore::db::IAuthoringDb* authoring_db,
    simcore::db::IExecutionDb* execution_db,
    std::int64_t dtm_artifact_id,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out);

bool SeedTasMovieSeedProbeWorkflow(
    simcore::db::IAuthoringDb* authoring_db,
    simcore::db::IExecutionDb* execution_db,
    std::int64_t dtm_artifact_id,
    std::int64_t seed_probe_spec_id,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out);

constexpr int kSeedProbeSamplesPerAxis = 5;

} // namespace simcore::e2e
