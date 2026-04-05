#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <utility>

namespace simcore::db::execution::workflow {

enum class WorkflowExecutionMode {
    LegacyOnly = 0,
    DualWriteObserve = 1,
    WorkflowPrimary = 2,
    WorkflowOnly = 3,
};

enum class WorkflowInstanceState {
    Pending = 0,
    Running = 1,
    Completed = 2,
    Failed = 3,
    Canceled = 4,
};

enum class WorkflowStepState {
    Waiting = 0,
    Ready = 1,
    Materialized = 2,
    Running = 3,
    Completed = 4,
    Failed = 5,
    Skipped = 6,
};

struct WorkflowInstanceRecord {
    std::int64_t workflow_instance_id = 0;
    std::string workflow_kind;
    WorkflowInstanceState state = WorkflowInstanceState::Pending;
    std::string root_scope_kind;
    std::optional<std::int64_t> root_scope_id;
    std::optional<std::string> input_ref_kind;
    std::optional<std::int64_t> input_ref_id;
};

struct WorkflowStepRecord {
    std::int64_t workflow_step_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::string step_key;
    std::string step_kind;
    WorkflowStepState state = WorkflowStepState::Waiting;
    std::optional<std::string> blocked_reason;
    std::optional<std::int64_t> job_set_id;
    int priority = 0;
    int attempts = 0;
    int max_attempts = 1;
};

struct WorkflowEdgeRecord {
    std::int64_t workflow_edge_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t from_step_id = 0;
    std::int64_t to_step_id = 0;
    std::optional<std::string> condition_kind;
    std::optional<std::string> condition_value;
};

struct WorkflowGraphSnapshot {
    WorkflowInstanceRecord instance;
    std::vector<WorkflowStepRecord> steps;
    std::vector<WorkflowEdgeRecord> edges;
};

struct WorkflowRetryStepCommand {
    std::int64_t workflow_step_id = 0;
    std::string requested_by;
};

struct WorkflowSkipStepCommand {
    std::int64_t workflow_step_id = 0;
    std::string reason;
    std::string requested_by;
};

struct WorkflowCancelInstanceCommand {
    std::int64_t workflow_instance_id = 0;
    std::string reason;
    std::string requested_by;
};

struct WorkflowResumeInstanceCommand {
    std::int64_t workflow_instance_id = 0;
    std::string requested_by;
};

struct IWorkflowOrchestrationQueryService {
    virtual ~IWorkflowOrchestrationQueryService() = default;

    virtual std::vector<WorkflowInstanceRecord> ListWorkflowInstances(
        WorkflowInstanceState state,
        std::int64_t created_at_utc_start,
        std::int64_t created_at_utc_end) const = 0;

    virtual std::optional<WorkflowGraphSnapshot> GetWorkflowGraph(std::int64_t workflow_instance_id) const = 0;
    virtual std::vector<WorkflowStepRecord> ListBlockedSteps(std::int64_t workflow_instance_id) const = 0;
    virtual std::vector<std::pair<std::int64_t, std::int64_t>> GetStepToJobSetMap(std::int64_t workflow_instance_id) const = 0;
};

struct IWorkflowOrchestrationCommandService {
    virtual ~IWorkflowOrchestrationCommandService() = default;

    virtual bool RetryFailedStep(const WorkflowRetryStepCommand& command, std::string* error_out) = 0;
    virtual bool SkipStep(const WorkflowSkipStepCommand& command, std::string* error_out) = 0;
    virtual bool CancelWorkflowInstance(const WorkflowCancelInstanceCommand& command, std::string* error_out) = 0;
    virtual bool ResumeWorkflowInstance(const WorkflowResumeInstanceCommand& command, std::string* error_out) = 0;
};

} // namespace simcore::db::execution::workflow
