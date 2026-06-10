#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "WorkflowComposition.h"
#include "WorkflowOrchestration.h"

namespace savor::db::execution::workflow {

inline std::optional<WorkflowCreateUnitActivationSpec> BuildUnitActivationSpecFromDefinition(
    const WorkflowUnitRegistry& registry,
    std::string activation_key,
    std::string graph_node_key,
    std::string unit_kind,
    std::string display_name,
    std::optional<std::string> authored_ref_kind,
    std::optional<std::int64_t> authored_ref_id,
    std::vector<std::string> dependencies,
    std::string* error_out = nullptr) {
    const auto* unit = registry.Find(unit_kind);
    if (unit == nullptr) {
        if (error_out != nullptr) {
            *error_out = "unknown workflow unit: " + unit_kind;
        }
        return std::nullopt;
    }
    if (unit->step_templates.empty()) {
        if (error_out != nullptr) {
            *error_out = "workflow unit has no step templates: " + unit_kind;
        }
        return std::nullopt;
    }

    WorkflowCreateUnitActivationSpec activation{};
    activation.activation_key = std::move(activation_key);
    activation.graph_node_key = graph_node_key.empty() ? activation.activation_key : std::move(graph_node_key);
    activation.unit_kind = std::move(unit_kind);
    activation.display_name = display_name.empty() ? unit->display_name : std::move(display_name);
    activation.activation_params_json = unit->default_activation_params_json.empty()
        ? "{}"
        : unit->default_activation_params_json;
    activation.authored_ref_kind = std::move(authored_ref_kind);
    activation.authored_ref_id = authored_ref_id;
    activation.dependencies = std::move(dependencies);
    activation.steps.reserve(unit->step_templates.size());
    for (const auto& tmpl : unit->step_templates) {
        activation.steps.push_back(WorkflowCreateUnitStepSpec{
            .step_key_suffix = tmpl.step_key_suffix,
            .step_kind = tmpl.step_kind,
            .priority = tmpl.priority,
            .max_attempts = tmpl.max_attempts,
        });
    }
    return activation;
}

} // namespace savor::db::execution::workflow
