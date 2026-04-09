#pragma once

#include <cstdint>
#include <string>

#include <sqlite3.h>

namespace simcore::db::execution::workflow {

struct WorkflowRecoveryResult {
    int completed_steps = 0;
    int failed_steps = 0;
};

struct WorkflowInvariantRemediationCommand {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
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
    bool PlanInvariantRemediation(
        const WorkflowInvariantRemediationCommand& command,
        WorkflowInvariantRemediationDecision* decision_out,
        std::string* error_out);

private:
    sqlite3* db_ = nullptr;
};

} // namespace simcore::db::execution::workflow
