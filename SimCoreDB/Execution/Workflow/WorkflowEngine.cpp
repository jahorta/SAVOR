#include "Execution/Workflow/WorkflowEngine.h"

#include <algorithm>

namespace simcore::db::execution::workflow {

namespace {

const WorkflowStepDefinition* FindStepDefinition(const WorkflowDefinition& definition, const std::string& step_key) {
    for (const auto& step : definition.steps) {
        if (step.step_key == step_key) {
            return &step;
        }
    }
    return nullptr;
}

bool IsTerminal(WorkflowStepState state) {
    return state == WorkflowStepState::Completed || state == WorkflowStepState::Failed || state == WorkflowStepState::Skipped;
}

} // namespace

WorkflowEngineResult ResolveReadiness(
    const WorkflowGraphSnapshot& snapshot,
    const WorkflowDefinition& definition,
    const std::unordered_set<std::string>& enabled_guards) {
    WorkflowEngineResult result;

    std::unordered_map<std::string, const WorkflowStepRecord*> step_record_by_key;
    for (const auto& step : snapshot.steps) {
        step_record_by_key.emplace(step.step_key, &step);
    }

    for (const auto& step_record : snapshot.steps) {
        if (step_record.state != WorkflowStepState::Waiting) {
            continue;
        }

        const auto* step_definition = FindStepDefinition(definition, step_record.step_key);
        if (!step_definition) {
            continue;
        }

        bool deps_completed = true;
        for (const auto& dep : step_definition->dependencies) {
            const auto dep_it = step_record_by_key.find(dep);
            if (dep_it == step_record_by_key.end() || dep_it->second->state != WorkflowStepState::Completed) {
                deps_completed = false;
                break;
            }
        }

        bool guard_pass = true;
        if (step_definition->guard_kind.has_value()) {
            guard_pass = enabled_guards.find(*step_definition->guard_kind) != enabled_guards.end();
        }

        if (deps_completed && guard_pass) {
            result.transitions.push_back(WorkflowEngineTransition{
                .workflow_step_id = step_record.workflow_step_id,
                .from = WorkflowStepState::Waiting,
                .to = WorkflowStepState::Ready,
                .reason = "dependencies_satisfied",
            });
        }
    }

    const bool all_terminal = std::all_of(snapshot.steps.begin(), snapshot.steps.end(), [](const WorkflowStepRecord& step) {
        return IsTerminal(step.state);
    });
    result.workflow_completed = all_terminal;
    return result;
}

WorkflowEngineResult ReconcileRunningSteps(
    const WorkflowGraphSnapshot& snapshot,
    const std::unordered_map<std::int64_t, std::string>& job_set_terminal_state_by_id) {
    WorkflowEngineResult result;

    for (const auto& step : snapshot.steps) {
        if (step.state != WorkflowStepState::Running && step.state != WorkflowStepState::Materialized) {
            continue;
        }
        if (!step.job_set_id.has_value()) {
            continue;
        }

        const auto job_it = job_set_terminal_state_by_id.find(*step.job_set_id);
        if (job_it == job_set_terminal_state_by_id.end()) {
            continue;
        }

        if (job_it->second == "COMPLETED") {
            result.transitions.push_back(WorkflowEngineTransition{
                .workflow_step_id = step.workflow_step_id,
                .from = step.state,
                .to = WorkflowStepState::Completed,
                .reason = "reconciled_job_set_completed",
            });
        } else if (job_it->second == "FAILED") {
            result.transitions.push_back(WorkflowEngineTransition{
                .workflow_step_id = step.workflow_step_id,
                .from = step.state,
                .to = WorkflowStepState::Failed,
                .reason = "reconciled_job_set_failed",
            });
        }
    }

    return result;
}

} // namespace simcore::db::execution::workflow
