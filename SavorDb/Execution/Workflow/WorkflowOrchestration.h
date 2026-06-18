#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace savor::db::execution::workflow {

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

enum class WorkflowUnitActivationState {
    Waiting = 0,
    Ready = 1,
    Running = 2,
    Completed = 3,
    Failed = 4,
    Skipped = 5,
    Canceled = 6,
};

struct WorkflowInstanceRecord {
    std::int64_t workflow_instance_id = 0;
    std::string workflow_kind;
    WorkflowInstanceState state = WorkflowInstanceState::Pending;
    std::string root_scope_kind;
    std::optional<std::int64_t> root_scope_id;
    std::optional<std::int64_t> workflow_graph_revision_id;
};

struct WorkflowUnitActivationRecord {
    std::int64_t workflow_unit_activation_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::optional<std::int64_t> parent_workflow_unit_activation_id;
    std::string activation_key;
    std::string graph_node_key;
    std::string unit_kind;
    std::string display_name;
    WorkflowUnitActivationState state = WorkflowUnitActivationState::Waiting;
    std::string activation_params_json;
    std::optional<std::string> authored_ref_kind;
    std::optional<std::int64_t> authored_ref_id;
    std::optional<std::string> failure_code;
    std::optional<std::string> failure_text;
};

struct WorkflowStepRecord {
    std::int64_t workflow_step_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::optional<std::int64_t> workflow_unit_activation_id;
    std::string step_key;
    std::string graph_node_key;
    std::string step_kind;
    WorkflowStepState state = WorkflowStepState::Waiting;
    std::optional<std::string> blocked_reason;
    std::optional<std::int64_t> job_set_id;
    std::optional<std::string> input_ref_kind;
    std::optional<std::int64_t> input_ref_id;
    std::optional<std::string> output_ref_kind;
    std::optional<std::int64_t> output_ref_id;
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

struct WorkflowUnitActivationEdgeRecord {
    std::int64_t workflow_unit_activation_edge_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t from_workflow_unit_activation_id = 0;
    std::int64_t to_workflow_unit_activation_id = 0;
    std::optional<std::string> output_key;
    std::optional<std::string> input_key;
    std::optional<std::string> condition_kind;
    std::optional<std::string> condition_value;
};

struct WorkflowInstanceInputBindingRecord {
    std::int64_t workflow_instance_input_binding_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_graph_revision_id = 0;
    std::string node_key;
    std::string input_key;
    std::string data_kind;
    std::string ref_kind;
    std::int64_t ref_id = 0;
    std::string source_kind;
    std::int64_t created_at_utc = 0;
};

struct WorkflowInstanceArgumentRecord {
    std::int64_t workflow_instance_argument_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::string node_key;
    std::string argument_key;
    std::string value_type;
    std::optional<std::int64_t> integer_value;
    std::optional<std::string> text_value;
    std::string source_kind;
    std::int64_t created_at_utc = 0;
};

struct WorkflowGraphSnapshot {
    WorkflowInstanceRecord instance;
    std::vector<WorkflowUnitActivationRecord> unit_activations;
    std::vector<WorkflowUnitActivationEdgeRecord> unit_activation_edges;
    std::vector<WorkflowStepRecord> steps;
    std::vector<WorkflowEdgeRecord> edges;
    std::vector<WorkflowInstanceInputBindingRecord> input_bindings;
    std::vector<WorkflowInstanceArgumentRecord> arguments;
};

struct WorkflowReadyStepRecord {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::optional<std::int64_t> workflow_unit_activation_id;
    std::string step_key;
    std::string graph_node_key;
    std::string step_kind;
    int priority = 0;
    std::optional<std::string> input_ref_kind;
    std::optional<std::int64_t> input_ref_id;
};

struct WorkflowStepTerminalSnapshot {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::optional<std::int64_t> workflow_unit_activation_id;
    std::int64_t job_set_id = 0;
    std::string workflow_kind;
    std::optional<std::int64_t> workflow_graph_revision_id;
    std::string step_key;
    std::string graph_node_key;
    std::string step_kind;
    std::optional<std::string> input_ref_kind;
    std::optional<std::int64_t> input_ref_id;
    std::optional<std::string> output_ref_kind;
    std::optional<std::int64_t> output_ref_id;
    int expected_total = 0;
    int discovered_total = 0;
    int terminal_total = 0;
    int failed_total = 0;
};

struct WorkflowStepOutputRecord {
    std::int64_t workflow_step_output_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::string graph_node_key;
    std::string output_key;
    std::string data_kind;
    std::string ref_kind;
    std::int64_t ref_id = 0;
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

struct WorkflowCompleteInstanceCommand {
    std::int64_t workflow_instance_id = 0;
    std::string requested_by;
};

struct WorkflowPauseInstanceCommand {
    std::int64_t workflow_instance_id = 0;
    std::string reason;
    std::string failure_code;
    std::string requested_by;
};

struct WorkflowTerminalFailInstanceCommand {
    std::int64_t workflow_instance_id = 0;
    std::string failure_code;
    std::string failure_message;
    std::string requested_by;
};

struct WorkflowMarkStepMaterializedCommand {
    std::int64_t workflow_step_id = 0;
    std::int64_t job_set_id = 0;
    std::optional<std::string> input_ref_kind;
    std::optional<std::int64_t> input_ref_id;
    std::string requested_by;
};

struct WorkflowMarkStepTerminalCommand {
    std::int64_t workflow_step_id = 0;
    std::string terminal_state; // COMPLETED | FAILED
    std::optional<std::string> output_ref_kind;
    std::optional<std::int64_t> output_ref_id;
    std::string requested_by;
};

struct WorkflowRecordStepOutputCommand {
    std::int64_t workflow_step_id = 0;
    std::string output_key;
    std::string output_data_kind;
    std::string output_ref_kind;
    std::int64_t output_ref_id = 0;
    std::string requested_by;
};

struct WorkflowRecordInputBindingCommand {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_graph_revision_id = 0;
    std::string node_key;
    std::string input_key;
    std::string data_kind;
    std::string ref_kind;
    std::int64_t ref_id = 0;
    std::string source_kind;
    std::string requested_by;
};

struct WorkflowMarkStepBlockedCommand {
    std::int64_t workflow_step_id = 0;
    std::optional<std::string> blocked_reason;
    std::string requested_by;
};

struct WorkflowMarkStepReadyCommand {
    std::int64_t workflow_instance_id = 0;
    std::string step_key;
    std::string requested_by;
};

struct WorkflowAppendDynamicStepSpec {
    std::string step_key;
    std::string step_kind;
    std::optional<std::string> input_ref_kind;
    std::optional<std::int64_t> input_ref_id;
    std::optional<std::string> guard_kind;
    std::optional<std::string> guard_value;
    int priority = 0;
    int max_attempts = 1;
};

struct WorkflowCreateUnitStepSpec {
    std::string step_key;
    std::string step_key_suffix;
    std::string step_kind;
    std::vector<std::string> dependencies;
    std::optional<std::string> guard_kind;
    std::optional<std::string> guard_value;
    int priority = 0;
    int max_attempts = 1;
    std::optional<std::string> input_ref_kind;
    std::optional<std::int64_t> input_ref_id;
};

struct WorkflowCreateUnitActivationSpec {
    std::string activation_key;
    std::optional<std::string> parent_activation_key;
    std::string graph_node_key;
    std::string unit_kind;
    std::string display_name;
    std::string activation_params_json;
    std::optional<std::string> authored_ref_kind;
    std::optional<std::int64_t> authored_ref_id;
    std::vector<std::string> dependencies;
    std::vector<WorkflowCreateUnitStepSpec> steps;
};

struct WorkflowScheduleUnitActivationCommand {
    std::int64_t workflow_instance_id = 0;
    std::optional<std::int64_t> parent_workflow_unit_activation_id;
    std::optional<std::int64_t> source_workflow_step_id;
    std::optional<std::int64_t> source_workflow_unit_activation_id;
    std::string activation_key;
    std::string graph_node_key;
    std::string unit_kind;
    std::string display_name;
    std::string activation_params_json;
    std::optional<std::string> authored_ref_kind;
    std::optional<std::int64_t> authored_ref_id;
    std::vector<WorkflowCreateUnitStepSpec> steps;
    std::string requested_by;
};

struct WorkflowAppendDynamicStepsCommand {
    std::int64_t workflow_instance_id = 0;
    std::optional<std::int64_t> parent_workflow_step_id;
    std::vector<WorkflowAppendDynamicStepSpec> steps;
    std::string requested_by;
};

struct WorkflowAppendLifecycleEventCommand {
    std::int64_t workflow_instance_id = 0;
    std::optional<std::int64_t> workflow_step_id;
    std::string event_kind;
    std::optional<std::string> message;
    std::string requested_by;
};

struct WorkflowCreateInstanceInputBindingSpec {
    std::string node_key;
    std::string input_key;
    std::string data_kind;
    std::string ref_kind;
    std::int64_t ref_id = 0;
    std::string source_kind;
};

struct WorkflowCreateInstanceArgumentSpec {
    std::string node_key;
    std::string argument_key;
    std::string value_type = "text";
    std::optional<std::int64_t> integer_value;
    std::optional<std::string> text_value;
    std::string source_kind;
};

struct WorkflowCreateInstanceCommand {
    std::string workflow_kind;
    std::string root_scope_kind;
    std::optional<std::int64_t> root_scope_id;
    std::optional<std::int64_t> workflow_graph_revision_id;
    std::string created_by;
    std::int64_t created_at_utc = 0;
    std::vector<WorkflowCreateUnitActivationSpec> unit_activations;
    std::vector<WorkflowCreateInstanceInputBindingSpec> input_bindings;
    std::vector<WorkflowCreateInstanceArgumentSpec> arguments;
};

struct IWorkflowOrchestrationQueryService {
    virtual ~IWorkflowOrchestrationQueryService() = default;

    virtual std::vector<WorkflowInstanceRecord> ListWorkflowInstances(
        WorkflowInstanceState state,
        std::int64_t created_at_utc_start,
        std::int64_t created_at_utc_end) const = 0;

    virtual std::vector<WorkflowReadyStepRecord> ListReadySteps(std::size_t limit) const = 0;
    virtual std::int64_t CountActiveMaterializedWorkflows() const = 0;
    virtual std::optional<WorkflowGraphSnapshot> GetWorkflowGraph(std::int64_t workflow_instance_id) const = 0;
    virtual std::optional<WorkflowStepTerminalSnapshot> GetStepTerminalSnapshotForJob(std::int64_t job_id) const = 0;
    virtual std::vector<WorkflowStepTerminalSnapshot> ListTerminalReadyStepSnapshots(std::size_t limit) const = 0;
    virtual std::vector<WorkflowStepRecord> ListBlockedSteps(std::int64_t workflow_instance_id) const = 0;
    virtual std::vector<std::pair<std::int64_t, std::int64_t>> GetStepToJobSetMap(std::int64_t workflow_instance_id) const = 0;
    virtual std::vector<WorkflowStepOutputRecord> ListStepOutputs(std::int64_t workflow_instance_id) const = 0;
};

struct IWorkflowOrchestrationCommandService {
    virtual ~IWorkflowOrchestrationCommandService() = default;

    virtual bool CreateWorkflowInstance(
        const WorkflowCreateInstanceCommand& command,
        std::int64_t* workflow_instance_id_out,
        std::string* error_out) = 0;
    virtual bool RetryFailedStep(const WorkflowRetryStepCommand& command, std::string* error_out) = 0;
    virtual bool SkipStep(const WorkflowSkipStepCommand& command, std::string* error_out) = 0;
    virtual bool CancelWorkflowInstance(const WorkflowCancelInstanceCommand& command, std::string* error_out) = 0;
    virtual bool ResumeWorkflowInstance(const WorkflowResumeInstanceCommand& command, std::string* error_out) = 0;
    virtual bool CompleteWorkflowInstance(const WorkflowCompleteInstanceCommand& command, std::string* error_out) = 0;
    virtual bool PauseWorkflowInstance(const WorkflowPauseInstanceCommand& command, std::string* error_out) = 0;
    virtual bool TerminalFailWorkflowInstance(const WorkflowTerminalFailInstanceCommand& command, std::string* error_out) = 0;
    virtual bool MarkStepMaterialized(const WorkflowMarkStepMaterializedCommand& command, std::string* error_out) = 0;
    virtual bool MarkStepTerminal(const WorkflowMarkStepTerminalCommand& command, std::string* error_out) = 0;
    virtual bool RecordStepOutput(const WorkflowRecordStepOutputCommand& command, std::string* error_out) = 0;
    virtual bool RecordInputBinding(const WorkflowRecordInputBindingCommand& command, std::string* error_out) = 0;
    virtual bool MarkStepBlocked(const WorkflowMarkStepBlockedCommand& command, std::string* error_out) = 0;
    virtual bool MarkStepReady(const WorkflowMarkStepReadyCommand& command, std::string* error_out) = 0;
    virtual bool ScheduleUnitActivation(const WorkflowScheduleUnitActivationCommand& command, std::string* error_out) = 0;
    virtual bool AppendDynamicSteps(const WorkflowAppendDynamicStepsCommand& command, std::string* error_out) = 0;
    virtual bool AppendLifecycleEvent(const WorkflowAppendLifecycleEventCommand& command, std::string* error_out) = 0;
};

} // namespace savor::db::execution::workflow
