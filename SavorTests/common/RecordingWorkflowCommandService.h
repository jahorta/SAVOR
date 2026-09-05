#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Execution/Workflow/SqliteExecutionDb.h"
#include "NullWorkflowQueryService.h"

class RecordingWorkflowQueryService final : public NullWorkflowQueryService {
public:
    std::optional<savor::db::execution::workflow::WorkflowTransitionActivationRecord>
    GetWorkflowTransitionActivation(
        std::int64_t workflow_instance_id,
        std::string_view activation_key) const override {
        if (activation
            && activation->workflow_instance_id == workflow_instance_id
            && activation->activation_key == activation_key) return activation;
        return std::nullopt;
    }

    std::optional<savor::db::execution::workflow::WorkflowTransitionActivationRecord>
        activation;
};

class RecordingWorkflowCommandService final : public savor::db::execution::workflow::IWorkflowOrchestrationCommandService {
public:
    explicit RecordingWorkflowCommandService(
        RecordingWorkflowQueryService* query_service = nullptr)
        : query_service_(query_service) {}
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
    bool FreezeTransitionActivation(
        const savor::db::execution::workflow::WorkflowFreezeTransitionActivationCommand& command,
        savor::db::execution::workflow::WorkflowFreezeTransitionActivationReceipt* receipt,
        std::string*) override {
        freeze_transition_calls.push_back(command);
        if (receipt) {
            receipt->created = true;
            receipt->activation.workflow_transition_activation_id =
                ++next_transition_activation_id_;
            receipt->activation.workflow_instance_id = command.workflow_instance_id;
            receipt->activation.source_workflow_step_id = command.source_workflow_step_id;
            receipt->activation.activation_kind = command.activation_kind;
            receipt->activation.activation_key = command.activation_key;
            receipt->activation.trigger_fingerprint = command.trigger_fingerprint;
            receipt->activation.decision_payload = command.decision_payload;
            receipt->activation.decision_sha256 = command.decision_sha256;
            if (query_service_) query_service_->activation = receipt->activation;
        }
        return true;
    }
    bool ApplyTransitionActivation(
        const savor::db::execution::workflow::WorkflowApplyTransitionActivationCommand& command,
        std::string*) override {
        apply_transition_calls.push_back(command);
        if (query_service_ && query_service_->activation
            && query_service_->activation->workflow_transition_activation_id
                == command.workflow_transition_activation_id) {
            query_service_->activation->state =
                savor::db::execution::workflow::
                    WorkflowTransitionActivationState::Applied;
            query_service_->activation->disposition = command.disposition;
        }
        return true;
    }
    bool RecordTransitionActivationFailure(
        const savor::db::execution::workflow::WorkflowRecordTransitionActivationFailureCommand& command,
        std::string*) override {
        transition_failure_calls.push_back(command);
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
    std::vector<savor::db::execution::workflow::WorkflowFreezeTransitionActivationCommand> freeze_transition_calls;
    std::vector<savor::db::execution::workflow::WorkflowApplyTransitionActivationCommand> apply_transition_calls;
    std::vector<savor::db::execution::workflow::WorkflowRecordTransitionActivationFailureCommand> transition_failure_calls;

private:
    std::int64_t next_workflow_instance_id_ = 100;
    std::int64_t next_transition_activation_id_ = 1000;
    RecordingWorkflowQueryService* query_service_ = nullptr;
};
