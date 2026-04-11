#include "SeedProbeWorkflowDefinition.h"

#include <algorithm>
#include <set>
#include <queue>
#include <sstream>
#include <unordered_map>

namespace simcore::db::execution::workflow {

WorkflowDefinition BuildSeedProbeChainDefinition() {
    WorkflowDefinition definition;
    definition.workflow_kind = "SEED_PROBE_CHAIN";
    definition.initial_inputs = { "sp_probe_run.probe_run_id"};
    definition.steps = {
        WorkflowStepDefinition{
            .step_key = "Neutral",
            .step_kind = "seedprobe.neutral",
            .dependencies = {},
            .required_inputs = { "sp_probe_run.probe_run_id" },
            .provided_outputs = { "seedprobe.neutral.seed_context" },
            .max_attempts = 2 },
        WorkflowStepDefinition{
            .step_key = "Grid",
            .step_kind = "seedprobe.grid",
            .dependencies = { "Neutral" },
            .required_inputs = { "seedprobe.neutral.seed_context" },
            .provided_outputs = { "seedprobe.grid.seed_evidence" },
            .max_attempts = 2 },
        WorkflowStepDefinition{
            .step_key = "Unique",
            .step_kind = "seedprobe.unique",
            .dependencies = { "Grid" },
            .required_inputs = { "seedprobe.grid.seed_evidence" },
            .provided_outputs = { "general.input_frame_list" },
            .max_attempts = 2 },
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

    const auto validate_keys = [&](const WorkflowStepDefinition& step, const std::vector<std::string>& keys, const char* field) -> bool {
        std::set<std::string> seen;
        for (const auto& key : keys) {
            if (key.empty()) {
                if (error_out) *error_out = std::string(field) + " contains empty key in step " + step.step_key;
                return false;
            }
            if (!seen.emplace(key).second) {
                if (error_out) *error_out = std::string("duplicate ") + field + " key '" + key + "' in step " + step.step_key;
                return false;
            }
        }
        return true;
    };

    {
        WorkflowStepDefinition workflow_seed_step{};
        workflow_seed_step.step_key = definition.workflow_kind;
        if (!validate_keys(workflow_seed_step, definition.initial_inputs, "initial_inputs")) return false;
    }

    std::vector<int> indegree(definition.steps.size(), 0);
    std::vector<std::vector<int>> adj(definition.steps.size());
    for (size_t i = 0; i < definition.steps.size(); ++i) {
        const auto& step = definition.steps[i];
        if (!validate_keys(step, step.required_inputs, "required_inputs")) return false;
        if (!validate_keys(step, step.provided_outputs, "provided_outputs")) return false;
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

    std::vector<std::set<std::string>> upstream_outputs(definition.steps.size());
    std::queue<int> q;
    for (size_t i = 0; i < indegree.size(); ++i) {
        if (indegree[i] == 0) {
            q.push(static_cast<int>(i));
            upstream_outputs[i].insert(definition.initial_inputs.begin(), definition.initial_inputs.end());
        }
    }

    std::vector<std::string> contract_errors;
    int visited = 0;
    while (!q.empty()) {
        const int node = q.front();
        q.pop();
        ++visited;

        const auto& step = definition.steps[node];
        std::vector<std::string> missing_inputs;
        for (const auto& required_key : step.required_inputs) {
            if (upstream_outputs[node].find(required_key) == upstream_outputs[node].end()) {
                missing_inputs.push_back(required_key);
            }
        }
        if (!missing_inputs.empty()) {
            std::ostringstream oss;
            oss << "step '" << step.step_key << "' missing required_inputs [";
            for (size_t i = 0; i < missing_inputs.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << missing_inputs[i];
            }
            oss << "]";
            contract_errors.push_back(oss.str());
        }

        std::set<std::string> outputs_after_step = upstream_outputs[node];
        outputs_after_step.insert(step.provided_outputs.begin(), step.provided_outputs.end());
        for (const int next : adj[node]) {
            upstream_outputs[next].insert(outputs_after_step.begin(), outputs_after_step.end());
            if (--indegree[next] == 0) {
                q.push(next);
            }
        }
    }

    if (visited != static_cast<int>(definition.steps.size())) {
        if (error_out) *error_out = "workflow graph contains cycle";
        return false;
    }

    if (!contract_errors.empty()) {
        std::ostringstream oss;
        oss << "unsatisfied required_inputs: ";
        for (size_t i = 0; i < contract_errors.size(); ++i) {
            if (i > 0) oss << "; ";
            oss << contract_errors[i];
        }
        if (error_out) *error_out = oss.str();
        return false;
    }

    return true;
}

bool WorkflowDefinitionRegistry::RegisterDefinition(WorkflowDefinition definition, std::string* error_out) {
    std::string validation_error;
    if (!ValidateWorkflowDefinition(definition, &validation_error)) {
        if (error_out) {
            *error_out = "invalid workflow definition '" + definition.workflow_kind + "': " + validation_error;
        }
        return false;
    }

    const auto inserted = definitions_.emplace(definition.workflow_kind, std::move(definition));
    if (!inserted.second) {
        if (error_out) {
            *error_out = "workflow definition already registered: " + inserted.first->first;
        }
        return false;
    }
    return true;
}

const WorkflowDefinition* WorkflowDefinitionRegistry::Find(std::string_view workflow_kind) const {
    const auto it = definitions_.find(std::string(workflow_kind));
    if (it == definitions_.end()) {
        return nullptr;
    }
    return &it->second;
}

bool WorkflowDefinitionRegistry::RegisterSeedProbeDefaults(std::string* error_out) {
    return RegisterDefinition(BuildSeedProbeChainDefinition(), error_out);
}

} // namespace simcore::db::execution::workflow
