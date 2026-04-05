#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "Execution/Workflow/WorkflowOrchestration.h"

namespace simcore::db::execution::workflow {

class SqliteWorkflowOrchestrationQueryService final : public IWorkflowOrchestrationQueryService {
public:
    explicit SqliteWorkflowOrchestrationQueryService(sqlite3* db);

    std::vector<WorkflowInstanceRecord> ListWorkflowInstances(
        WorkflowInstanceState state,
        std::int64_t created_at_utc_start,
        std::int64_t created_at_utc_end) const override;

    std::optional<WorkflowGraphSnapshot> GetWorkflowGraph(std::int64_t workflow_instance_id) const override;
    std::vector<WorkflowStepRecord> ListBlockedSteps(std::int64_t workflow_instance_id) const override;
    std::vector<std::pair<std::int64_t, std::int64_t>> GetStepToJobSetMap(std::int64_t workflow_instance_id) const override;

private:
    sqlite3* db_ = nullptr;
};

class SqliteWorkflowOrchestrationCommandService final : public IWorkflowOrchestrationCommandService {
public:
    explicit SqliteWorkflowOrchestrationCommandService(sqlite3* db);

    bool RetryFailedStep(const WorkflowRetryStepCommand& command, std::string* error_out) override;
    bool SkipStep(const WorkflowSkipStepCommand& command, std::string* error_out) override;
    bool CancelWorkflowInstance(const WorkflowCancelInstanceCommand& command, std::string* error_out) override;
    bool ResumeWorkflowInstance(const WorkflowResumeInstanceCommand& command, std::string* error_out) override;

private:
    bool EmitLifecycleEvent(
        std::int64_t workflow_instance_id,
        std::optional<std::int64_t> workflow_step_id,
        const char* event_kind,
        const char* message,
        std::string* error_out);

    sqlite3* db_ = nullptr;
};

} // namespace simcore::db::execution::workflow
