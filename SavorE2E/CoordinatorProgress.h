#pragma once

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>
#include <string>

#include "Execution/IExecutionDb.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Runner/Parallel/PRTypes.h"
#include "Runner/Parallel/SavorDb/DBWorkflowWorkerCoordinator.h"
#include "Runner/Parallel/WorkerTelemetry.h"

namespace savor::e2e {

using WorkerProgressById = std::unordered_map<std::size_t, savor::PRProgress>;

bool IsInteractiveStdout();

bool AreWorkflowStepsTerminal(const savor::db::execution::workflow::WorkflowGraphSnapshot& graph);
bool HasFailedWorkflowStep(const savor::db::execution::workflow::WorkflowGraphSnapshot& graph);

std::vector<std::string> BuildCoordinatorProgressLines(
    savor::db::IExecutionDb* execution_db,
    const savor::runner::parallel::savordb::WorkflowCoordinatorTelemetry& telemetry,
    const std::vector<WorkerSnapshot>& worker_snapshot,
    const std::optional<savor::db::execution::workflow::WorkflowGraphSnapshot>& graph,
    const WorkerProgressById* last_progress_by_worker = nullptr);

} // namespace savor::e2e
