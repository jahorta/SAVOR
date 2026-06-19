#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <sqlite3.h>

#include "AdapterChainOrchestrator.h"
#include "WorkflowOrchestration.h"
#include "WorkflowRecoveryService.h"
#include "../../Common/Events/EventEnvelope.h"

namespace savor::db::execution::workflow {

struct WorkflowTerminalOutboxSubscriberResult {
    std::int64_t last_scanned_outbox_id = 0;
    std::string last_scanned_event_id;
    int scanned_count = 0;
    int handled_count = 0;
};

class WorkflowTerminalOutboxSubscriber {
public:
    struct StepTerminalContextSnapshot {
        std::int64_t workflow_instance_id = 0;
        std::int64_t workflow_step_id = 0;
        std::int64_t job_set_id = 0;
        std::string workflow_kind;
        std::string step_key;
        std::string step_kind;
        int priority = 0;
        std::optional<std::string> input_ref_kind;
        std::optional<std::int64_t> input_ref_id;
        std::optional<std::string> output_ref_kind;
        std::optional<std::int64_t> output_ref_id;
        StepCompletionSnapshot completion;
        int failed_total = 0;
    };

    WorkflowTerminalOutboxSubscriber(
        sqlite3* db,
        const AdapterChainOrchestrator* orchestrator,
        IWorkflowOrchestrationCommandService* command_service);

    bool ConsumeFromCursor(
        std::int64_t cursor_outbox_id,
        int max_batch_size,
        WorkflowTerminalOutboxSubscriberResult* result_out,
        std::string* error_out) const;

private:
    bool HandleTerminalEvent(const events::EventEnvelope& envelope, std::string* error_out) const;
    bool LoadStepTerminalSnapshotForJob(std::int64_t job_id, StepTerminalContextSnapshot* snapshot_out, std::string* error_out) const;
    bool LoadStepTerminalSnapshotForStep(std::int64_t workflow_step_id, StepTerminalContextSnapshot* snapshot_out, std::string* error_out) const;
    bool LoadStepTerminalSnapshotForWorkflowEvent(std::int64_t workflow_event_id, StepTerminalContextSnapshot* snapshot_out, std::string* error_out) const;
    bool HandleStepTerminalSnapshot(const StepTerminalContextSnapshot& snapshot, bool allow_reconcile_retry, std::string* error_out) const;

    sqlite3* db_ = nullptr;
    const AdapterChainOrchestrator* orchestrator_ = nullptr;
    IWorkflowOrchestrationCommandService* command_service_ = nullptr;
    mutable WorkflowRecoveryService recovery_service_;
};

} // namespace savor::db::execution::workflow
