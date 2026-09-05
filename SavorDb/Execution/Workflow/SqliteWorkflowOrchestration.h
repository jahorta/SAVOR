#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "WorkflowOrchestration.h"

namespace savor::db::execution::workflow {

class SqliteWorkflowOrchestrationQueryService final : public IWorkflowOrchestrationQueryService {
public:
    explicit SqliteWorkflowOrchestrationQueryService(sqlite3* db);

    std::vector<WorkflowInstanceRecord> ListWorkflowInstances(
        WorkflowInstanceState state,
        std::int64_t created_at_utc_start,
        std::int64_t created_at_utc_end) const override;

    std::vector<WorkflowReadyStepRecord> ListReadySteps(std::size_t limit) const override;
    std::int64_t CountActiveMaterializedWorkflows() const override;
    std::optional<WorkflowGraphSnapshot> GetWorkflowGraph(std::int64_t workflow_instance_id) const override;
    std::optional<WorkflowStepSettlementSnapshot> GetStepSettlementSnapshotForJob(std::int64_t job_id) const override;
    std::vector<WorkflowStepSettlementSnapshot> ListSettlementReadyStepSnapshots(std::size_t limit) const override;
    std::vector<WorkflowStepRecord> ListBlockedSteps(std::int64_t workflow_instance_id) const override;
    std::vector<std::pair<std::int64_t, std::int64_t>> GetStepToJobSetMap(std::int64_t workflow_instance_id) const override;
    std::vector<WorkflowStepOutputRecord> ListStepOutputs(std::int64_t workflow_instance_id) const override;
    std::optional<WorkflowTransitionActivationRecord>
        GetWorkflowTransitionActivation(
            std::int64_t workflow_instance_id,
            std::string_view activation_key) const override;

private:
    sqlite3* db_ = nullptr;
};

class SqliteWorkflowOrchestrationCommandService final : public IWorkflowOrchestrationCommandService {
public:
    explicit SqliteWorkflowOrchestrationCommandService(sqlite3* db);

    bool CreateWorkflowInstance(
        const WorkflowCreateInstanceCommand& command,
        std::int64_t* workflow_instance_id_out,
        std::string* error_out) override;
    bool RetryFailedStep(const WorkflowRetryStepCommand& command, std::string* error_out) override;
    bool SkipStep(const WorkflowSkipStepCommand& command, std::string* error_out) override;
    bool CancelWorkflowInstance(const WorkflowCancelInstanceCommand& command, std::string* error_out) override;
    bool ResumeWorkflowInstance(const WorkflowResumeInstanceCommand& command, std::string* error_out) override;
    bool CompleteWorkflowInstance(const WorkflowCompleteInstanceCommand& command, std::string* error_out) override;
    bool PauseWorkflowInstance(const WorkflowPauseInstanceCommand& command, std::string* error_out) override;
    bool FailWorkflowInstance(const WorkflowFailInstanceCommand& command, std::string* error_out) override;
    bool InterruptWorkflowInstance(const WorkflowInterruptInstanceCommand& command, std::string* error_out) override;
    bool MarkStepMaterialized(const WorkflowMarkStepMaterializedCommand& command, std::string* error_out) override;
    bool CompleteWorkflowStep(const WorkflowCompleteStepCommand& command, std::string* error_out) override;
    bool RecordStepOutput(const WorkflowRecordStepOutputCommand& command, std::string* error_out) override;
    bool RecordInputBinding(const WorkflowRecordInputBindingCommand& command, std::string* error_out) override;
    bool MarkStepBlocked(const WorkflowMarkStepBlockedCommand& command, std::string* error_out) override;
    bool MarkStepReady(const WorkflowMarkStepReadyCommand& command, std::string* error_out) override;
    bool ScheduleUnitActivation(const WorkflowScheduleUnitActivationCommand& command, std::string* error_out) override;
    bool AppendDynamicSteps(const WorkflowAppendDynamicStepsCommand& command, std::string* error_out) override;
    bool AppendLifecycleEvent(const WorkflowAppendLifecycleEventCommand& command, std::string* error_out) override;
    bool FreezeTransitionActivation(
        const WorkflowFreezeTransitionActivationCommand& command,
        WorkflowFreezeTransitionActivationReceipt* receipt_out,
        std::string* error_out) override;
    bool ApplyTransitionActivation(
        const WorkflowApplyTransitionActivationCommand& command,
        std::string* error_out) override;
    bool RecordTransitionActivationFailure(
        const WorkflowRecordTransitionActivationFailureCommand& command,
        std::string* error_out) override;

private:
    bool EmitLifecycleEvent(
        std::int64_t workflow_instance_id,
        std::optional<std::int64_t> workflow_step_id,
        const char* event_kind,
        const char* message,
        std::string* error_out);

    sqlite3* db_ = nullptr;
};

} // namespace savor::db::execution::workflow
