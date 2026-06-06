#pragma once

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>
#include <string>

#include "Execution/IExecutionDb.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Runner/Parallel/PRTypes.h"
#include "Runner/Parallel/SimCoreDB/DBWorkflowWorkerCoordinator.h"
#include "Runner/Parallel/WorkerTelemetry.h"

namespace simcore::e2e {

using WorkerProgressById = std::unordered_map<std::size_t, simcore::PRProgress>;

bool IsInteractiveStdout();

bool AreWorkflowStepsTerminal(const simcore::db::execution::workflow::WorkflowGraphSnapshot& graph);
bool HasFailedWorkflowStep(const simcore::db::execution::workflow::WorkflowGraphSnapshot& graph);

std::vector<std::string> BuildCoordinatorProgressLines(
    simcore::db::IExecutionDb* execution_db,
    const simcore::runner::parallel::simcoredb::WorkflowCoordinatorTelemetry& telemetry,
    const std::vector<WorkerSnapshot>& worker_snapshot,
    const std::optional<simcore::db::execution::workflow::WorkflowGraphSnapshot>& graph,
    const WorkerProgressById* last_progress_by_worker = nullptr);

} // namespace simcore::e2e
