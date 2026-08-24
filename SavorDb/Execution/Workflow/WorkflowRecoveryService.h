#pragma once

#include <cstdint>
#include <string>

#include <sqlite3.h>

namespace savor::db::execution::workflow {

struct IWorkflowOrchestrationCommandService;

struct WorkflowRecoveryResult {
    int completed_steps = 0;
    int failed_steps = 0;
    int interrupted_steps = 0;
    int canceled_steps = 0;
};

struct WorkflowInvariantRemediationCommand {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::string violation_reason;
    std::string requested_by;
};

struct WorkflowInvariantRemediationDecision {
    bool can_reopen = false;
    std::string reason;
    std::string failure_code;
    std::string failure_message;
};

class WorkflowRecoveryService {
public:
    explicit WorkflowRecoveryService(sqlite3* db);

    bool ReconcileInFlightInstances(WorkflowRecoveryResult* result_out, std::string* error_out);
    bool PurgeHandlerDedupeOlderThan(
        std::int64_t last_seen_at_utc_exclusive,
        int max_rows,
        int* rows_deleted_out,
        std::string* error_out);
    bool PlanInvariantRemediation(
        const WorkflowInvariantRemediationCommand& command,
        WorkflowInvariantRemediationDecision* decision_out,
        std::string* error_out);
    bool ExecuteInvariantRemediation(
        const WorkflowInvariantRemediationCommand& command,
        IWorkflowOrchestrationCommandService* command_service,
        bool* reopened_out,
        std::string* error_out);

private:
    sqlite3* db_ = nullptr;
};

} // namespace savor::db::execution::workflow
