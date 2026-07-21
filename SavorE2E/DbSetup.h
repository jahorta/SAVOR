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

namespace savor::e2e {

savor::db::DbConfigPaths BuildDbPaths(const CliOptions& options);

class ScopedWorkflowCoordinatorService {
public:
    using EventLineCallback = savor::db::execution::workflow::WorkflowCoordinatorService::EventLineCallback;

    ScopedWorkflowCoordinatorService() = default;
    ~ScopedWorkflowCoordinatorService();

    ScopedWorkflowCoordinatorService(const ScopedWorkflowCoordinatorService&) = delete;
    ScopedWorkflowCoordinatorService& operator=(const ScopedWorkflowCoordinatorService&) = delete;

    bool Start(
        savor::db::IExecutionDb* execution_db,
        savor::db::IAuthoringDb* authoring_db,
        const savor::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
        const CliOptions& options,
        std::string* error_out,
        EventLineCallback event_line_callback = {},
        bool strict_smoke_terminal_on_failure = false);
    void Stop();
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] savor::db::execution::workflow::WorkflowCoordinatorTelemetry SnapshotTelemetry() const;

private:
    std::unique_ptr<savor::db::execution::workflow::WorkflowCoordinatorService> service_;
};

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
    const CliOptions& options,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out);

bool SeedTasMovieSeedProbeWorkflow(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    std::int64_t dtm_artifact_id,
    std::int64_t seed_probe_spec_id,
    const CliOptions& options,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out);

constexpr int kSeedProbeSamplesPerAxis = 5;

} // namespace savor::e2e
