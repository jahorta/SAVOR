#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Execution/Workflow/SqliteExecutionDb.h"

class RecordingWorkflowCommandService final : public simcore::db::execution::workflow::IWorkflowOrchestrationCommandService {
public:
    bool CreateWorkflowInstance(
        const simcore::db::execution::workflow::WorkflowCreateInstanceCommand& command,
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
    bool RetryFailedStep(const simcore::db::execution::workflow::WorkflowRetryStepCommand&, std::string*) override { return true; }
    bool SkipStep(const simcore::db::execution::workflow::WorkflowSkipStepCommand&, std::string*) override { return true; }
    bool CancelWorkflowInstance(const simcore::db::execution::workflow::WorkflowCancelInstanceCommand&, std::string*) override { return true; }
    bool ResumeWorkflowInstance(const simcore::db::execution::workflow::WorkflowResumeInstanceCommand&, std::string*) override { return true; }
    bool CompleteWorkflowInstance(
        const simcore::db::execution::workflow::WorkflowCompleteInstanceCommand& command,
        std::string*) override {
        complete_workflow_instance_calls.push_back(command);
        return true;
    }
    bool PauseWorkflowInstance(const simcore::db::execution::workflow::WorkflowPauseInstanceCommand&, std::string*) override { return true; }
    bool TerminalFailWorkflowInstance(const simcore::db::execution::workflow::WorkflowTerminalFailInstanceCommand&, std::string*) override { return true; }

    bool MarkStepMaterialized(
        const simcore::db::execution::workflow::WorkflowMarkStepMaterializedCommand& command,
        std::string*) override {
        materialized_calls.push_back(command);
        return true;
    }

    bool MarkStepTerminal(
        const simcore::db::execution::workflow::WorkflowMarkStepTerminalCommand& command,
        std::string*) override {
        terminal_calls.push_back(command);
        return true;
    }
    bool MarkStepBlocked(
        const simcore::db::execution::workflow::WorkflowMarkStepBlockedCommand& command,
        std::string*) override {
        blocked_calls.push_back(command);
        return true;
    }
    bool MarkStepReady(
        const simcore::db::execution::workflow::WorkflowMarkStepReadyCommand& command,
        std::string*) override {
        ready_calls.push_back(command);
        return true;
    }
    bool AppendDynamicSteps(
        const simcore::db::execution::workflow::WorkflowAppendDynamicStepsCommand& command,
        std::string*) override {
        dynamic_step_calls.push_back(command);
        return true;
    }
    bool AppendLifecycleEvent(
        const simcore::db::execution::workflow::WorkflowAppendLifecycleEventCommand& command,
        std::string*) override {
        lifecycle_events.push_back(command);
        return true;
    }

    std::vector<simcore::db::execution::workflow::WorkflowMarkStepMaterializedCommand> materialized_calls;
    std::vector<simcore::db::execution::workflow::WorkflowMarkStepTerminalCommand> terminal_calls;
    std::vector<simcore::db::execution::workflow::WorkflowMarkStepBlockedCommand> blocked_calls;
    std::vector<simcore::db::execution::workflow::WorkflowMarkStepReadyCommand> ready_calls;
    std::vector<simcore::db::execution::workflow::WorkflowAppendDynamicStepsCommand> dynamic_step_calls;
    std::vector<simcore::db::execution::workflow::WorkflowAppendLifecycleEventCommand> lifecycle_events;
    std::vector<simcore::db::execution::workflow::WorkflowCreateInstanceCommand> create_workflow_instance_calls;
    std::vector<simcore::db::execution::workflow::WorkflowCompleteInstanceCommand> complete_workflow_instance_calls;

private:
    std::int64_t next_workflow_instance_id_ = 100;
};
