#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Execution/Workflow/SqliteExecutionDb.h"

class RecordingWorkflowCommandService final : public savor::db::execution::workflow::IWorkflowOrchestrationCommandService {
public:
    bool CreateWorkflowInstance(
        const savor::db::execution::workflow::WorkflowCreateInstanceCommand& command,
        std::int64_t* workflow_instance_id_out,
        std::string* error_out) override {
        create_workflow_instance_calls.push_back(command);
        const auto workflow_instance_id = ++next_workflow_instance_id_;
        if (workflow_instance_id_out) {
            *workflow_instance_id_out = workflow_instance_id;
        }
        if (error_out) {
            error_out->clear();
        }
        return true;
    }
    bool RetryFailedStep(const savor::db::execution::workflow::WorkflowRetryStepCommand&, std::string*) override { return true; }
    bool SkipStep(const savor::db::execution::workflow::WorkflowSkipStepCommand&, std::string*) override { return true; }
    bool CancelWorkflowInstance(const savor::db::execution::workflow::WorkflowCancelInstanceCommand&, std::string*) override { return true; }
    bool ResumeWorkflowInstance(const savor::db::execution::workflow::WorkflowResumeInstanceCommand&, std::string*) override { return true; }
    bool CompleteWorkflowInstance(
        const savor::db::execution::workflow::WorkflowCompleteInstanceCommand& command,
        std::string*) override {
        complete_workflow_instance_calls.push_back(command);
        return true;
    }
    bool PauseWorkflowInstance(const savor::db::execution::workflow::WorkflowPauseInstanceCommand&, std::string*) override { return true; }
    bool FailWorkflowInstance(const savor::db::execution::workflow::WorkflowFailInstanceCommand&, std::string*) override { return true; }
    bool InterruptWorkflowInstance(const savor::db::execution::workflow::WorkflowInterruptInstanceCommand&, std::string*) override { return true; }

    bool MarkStepMaterialized(
        const savor::db::execution::workflow::WorkflowMarkStepMaterializedCommand& command,
        std::string*) override {
        materialized_calls.push_back(command);
        return true;
    }

    bool CompleteWorkflowStep(
        const savor::db::execution::workflow::WorkflowCompleteStepCommand& command,
        std::string*) override {
        completion_calls.push_back(command);
        return true;
    }
    bool RecordStepOutput(
        const savor::db::execution::workflow::WorkflowRecordStepOutputCommand& command,
        std::string*) override {
        step_output_calls.push_back(command);
        return true;
    }
    bool RecordInputBinding(
        const savor::db::execution::workflow::WorkflowRecordInputBindingCommand& command,
        std::string*) override {
        input_binding_calls.push_back(command);
        return true;
    }
    bool MarkStepBlocked(
        const savor::db::execution::workflow::WorkflowMarkStepBlockedCommand& command,
        std::string*) override {
        blocked_calls.push_back(command);
        return true;
    }
    bool MarkStepReady(
        const savor::db::execution::workflow::WorkflowMarkStepReadyCommand& command,
        std::string*) override {
        ready_calls.push_back(command);
        return true;
    }
    bool AppendDynamicSteps(
        const savor::db::execution::workflow::WorkflowAppendDynamicStepsCommand& command,
        std::string*) override {
        dynamic_step_calls.push_back(command);
        return true;
    }
    bool ScheduleUnitActivation(
        const savor::db::execution::workflow::WorkflowScheduleUnitActivationCommand& command,
        std::string*) override {
        schedule_unit_activation_calls.push_back(command);
        return true;
    }
    bool AppendLifecycleEvent(
        const savor::db::execution::workflow::WorkflowAppendLifecycleEventCommand& command,
        std::string*) override {
        lifecycle_events.push_back(command);
        return true;
    }

    std::vector<savor::db::execution::workflow::WorkflowMarkStepMaterializedCommand> materialized_calls;
    std::vector<savor::db::execution::workflow::WorkflowCompleteStepCommand> completion_calls;
    std::vector<savor::db::execution::workflow::WorkflowRecordStepOutputCommand> step_output_calls;
    std::vector<savor::db::execution::workflow::WorkflowRecordInputBindingCommand> input_binding_calls;
    std::vector<savor::db::execution::workflow::WorkflowMarkStepBlockedCommand> blocked_calls;
    std::vector<savor::db::execution::workflow::WorkflowMarkStepReadyCommand> ready_calls;
    std::vector<savor::db::execution::workflow::WorkflowAppendDynamicStepsCommand> dynamic_step_calls;
    std::vector<savor::db::execution::workflow::WorkflowScheduleUnitActivationCommand> schedule_unit_activation_calls;
    std::vector<savor::db::execution::workflow::WorkflowAppendLifecycleEventCommand> lifecycle_events;
    std::vector<savor::db::execution::workflow::WorkflowCreateInstanceCommand> create_workflow_instance_calls;
    std::vector<savor::db::execution::workflow::WorkflowCompleteInstanceCommand> complete_workflow_instance_calls;

private:
    std::int64_t next_workflow_instance_id_ = 100;
};
