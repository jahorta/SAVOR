#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "SeedProbeWorkflowDefinition.h"
#include "WorkflowOrchestration.h"

namespace simcore::db::execution::workflow {

struct WorkflowEngineTransition {
    std::int64_t workflow_step_id = 0;
    WorkflowStepState from = WorkflowStepState::Waiting;
    WorkflowStepState to = WorkflowStepState::Waiting;
    const char* reason = "";
};

struct WorkflowEngineResult {
    std::vector<WorkflowEngineTransition> transitions;
    bool workflow_completed = false;
};

WorkflowEngineResult ResolveReadiness(
    const WorkflowGraphSnapshot& snapshot,
    const WorkflowDefinition& definition,
    const std::unordered_set<std::string>& enabled_guards);

WorkflowEngineResult ReconcileRunningSteps(
    const WorkflowGraphSnapshot& snapshot,
    const std::unordered_map<std::int64_t, std::string>& job_set_terminal_state_by_id);

} // namespace simcore::db::execution::workflow
