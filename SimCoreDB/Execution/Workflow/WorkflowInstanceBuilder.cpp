#include "WorkflowInstanceBuilder.h"

namespace simcore::db::execution::workflow {

WorkflowInstanceBuilder::WorkflowInstanceBuilder(
    const WorkflowDefinitionRegistry* registry,
    const WorkflowInstanceValidator* validator)
    : registry_(registry)
    , validator_(validator != nullptr ? validator : &owned_validator_) {
}

bool WorkflowInstanceBuilder::BuildCreateCommand(
    const WorkflowDefinitionInstantiationInput& input,
    WorkflowCreateInstanceCommand* command_out,
    std::string* error_out) const {
    if (registry_ == nullptr) {
        if (error_out) *error_out = "workflow definition registry unavailable";
        return false;
    }
    if (command_out == nullptr) {
        if (error_out) *error_out = "command_out is required";
        return false;
    }

    const auto* definition = registry_->Find(input.workflow_kind);
    if (definition == nullptr) {
        if (error_out) *error_out = "workflow definition not found: " + input.workflow_kind;
        return false;
    }

    if (validator_ == nullptr) {
        if (error_out) *error_out = "workflow instance validator unavailable";
        return false;
    }
    if (!validator_->ValidateAvailableInputs(*definition, input.available_inputs, error_out)) {
        return false;
    }

    WorkflowCreateInstanceCommand command{};
    command.workflow_kind = input.workflow_kind;
    command.root_scope_kind = input.root_scope_kind;
    command.root_scope_id = input.root_scope_id;
    command.input_ref_kind = input.input_ref_kind;
    command.input_ref_id = input.input_ref_id;
    command.created_by = input.created_by;
    command.created_at_utc = input.created_at_utc;
    command.available_inputs = input.available_inputs;

    for (const auto& definition_step : definition->steps) {
        WorkflowCreateStepSpec step{};
        step.step_key = definition_step.step_key;
        step.step_kind = definition_step.step_kind;
        step.dependencies = definition_step.dependencies;
        step.guard_kind = definition_step.guard_kind;
        step.guard_value = definition_step.guard_value;
        step.max_attempts = definition_step.max_attempts;
        if (!definition_step.required_inputs.empty()) {
            step.input_ref_kind = definition_step.required_inputs.front();
        }
        step.input_ref_id = input.input_ref_id;
        command.steps.push_back(std::move(step));
    }

    *command_out = std::move(command);
    return true;
}

bool WorkflowInstanceBuilder::CreateWorkflowInstance(
    const WorkflowDefinitionInstantiationInput& input,
    IWorkflowOrchestrationCommandService* command_service,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) const {
    if (command_service == nullptr) {
        if (error_out) *error_out = "workflow command service unavailable";
        return false;
    }

    WorkflowCreateInstanceCommand command{};
    if (!BuildCreateCommand(input, &command, error_out)) {
        return false;
    }

    return command_service->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
}

} // namespace simcore::db::execution::workflow
