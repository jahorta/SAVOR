#include "Execution/Workflow/SeedProbeWorkflowDefinition.h"

#include <queue>
#include <unordered_map>

namespace simcore::db::execution::workflow {

WorkflowDefinition BuildSeedProbeChainDefinition() {
    WorkflowDefinition definition;
    definition.workflow_kind = "SEED_PROBE_CHAIN";
    definition.steps = {
        WorkflowStepDefinition{ .step_key = "Neutral", .step_kind = "seedprobe.neutral", .dependencies = {}, .max_attempts = 2 },
        WorkflowStepDefinition{ .step_key = "Grid", .step_kind = "seedprobe.grid", .dependencies = { "Neutral" }, .max_attempts = 2 },
        WorkflowStepDefinition{ .step_key = "Unique", .step_kind = "seedprobe.unique", .dependencies = { "Grid" }, .max_attempts = 2 },
        WorkflowStepDefinition{ .step_key = "Done", .step_kind = "seedprobe.done", .dependencies = { "Unique" }, .max_attempts = 1 },
    };
    return definition;
}

bool ValidateWorkflowDefinition(const WorkflowDefinition& definition, std::string* error_out) {
    if (definition.workflow_kind.empty()) {
        if (error_out) *error_out = "workflow_kind is required";
        return false;
    }
    if (definition.steps.empty()) {
        if (error_out) *error_out = "at least one step is required";
        return false;
    }

    std::unordered_map<std::string, int> index_by_key;
    for (size_t i = 0; i < definition.steps.size(); ++i) {
        const auto& step = definition.steps[i];
        if (step.step_key.empty()) {
            if (error_out) *error_out = "step_key is required";
            return false;
        }
        if (!index_by_key.emplace(step.step_key, static_cast<int>(i)).second) {
            if (error_out) *error_out = "duplicate step_key: " + step.step_key;
            return false;
        }
    }

    std::vector<int> indegree(definition.steps.size(), 0);
    std::vector<std::vector<int>> adj(definition.steps.size());

    for (size_t i = 0; i < definition.steps.size(); ++i) {
        const auto& step = definition.steps[i];
        for (const auto& dep : step.dependencies) {
            const auto dep_it = index_by_key.find(dep);
            if (dep_it == index_by_key.end()) {
                if (error_out) *error_out = "dependency not found for step " + step.step_key + ": " + dep;
                return false;
            }
            adj[dep_it->second].push_back(static_cast<int>(i));
            ++indegree[i];
        }
    }

    std::queue<int> q;
    for (size_t i = 0; i < indegree.size(); ++i) {
        if (indegree[i] == 0) q.push(static_cast<int>(i));
    }

    int visited = 0;
    while (!q.empty()) {
        const int node = q.front();
        q.pop();
        ++visited;
        for (const int next : adj[node]) {
            if (--indegree[next] == 0) {
                q.push(next);
            }
        }
    }

    if (visited != static_cast<int>(definition.steps.size())) {
        if (error_out) *error_out = "workflow graph contains cycle";
        return false;
    }

    return true;
}

} // namespace simcore::db::execution::workflow
