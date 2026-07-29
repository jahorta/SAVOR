#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../../SavorCore/Runner/Script/PhaseScriptVM.h"

namespace savor {
struct PRResult;
}

namespace savor::db::execution::programdb {

struct JobPersistenceRecord {
    std::string program_ref_kind;
    std::int64_t program_ref_id = 0;
    std::string fingerprint;
    std::int32_t program_version = 1;
};

struct WorkflowStepScheduleResult {
    JobPersistenceRecord persistence;
    std::int64_t root_job_set_id = 0;
    std::vector<std::string> event_lines;
};

struct WorkflowGraphInputBinding {
    std::string node_key;
    std::string input_key;
    std::string data_kind;
    std::string ref_kind;
    std::int64_t ref_id = 0;
    std::string source_kind;
};

struct WorkflowGraphArgument {
    std::string node_key;
    std::string argument_key;
    std::string value_type;
    std::optional<std::int64_t> integer_value;
    std::optional<std::string> text_value;
    std::string source_kind;
};

struct WorkflowStepScheduleContext {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::string step_key;
    std::string step_kind;
    std::int64_t domain_ref_id = 0;
    int step_priority = 0;
};

struct WorkflowGraphStepScheduleContext {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::optional<std::int64_t> workflow_graph_revision_id;
    std::string step_key;
    std::string step_kind;
    std::optional<std::int64_t> workflow_unit_activation_id;
    std::string activation_key;
    std::string activation_graph_node_key;
    std::string unit_kind;
    std::string unit_variant;
    std::string breakpoint_profile_key;
    std::string activation_params_json;
    int step_priority = 0;
    std::vector<WorkflowGraphInputBinding> input_bindings;
    std::vector<WorkflowGraphArgument> arguments;
};

struct RuntimeInitRequest {
    std::string savestate_ref_kind;
    std::int64_t savestate_ref_id = 0;
    std::string bootstrap_profile;
    savor::DBuf derived_buffer_type = savor::DBuf::DK_None;
    // Complete adapter-declared workset compatibility identity. Empty means
    // the job is intentionally singleton until its native phase adapter can
    // name the exact savestate/movie/derived-state baseline and policies.
    std::optional<std::string> workset_execution_key;
};

struct ResultArtifactRef {
    std::string artifact_role;
    std::int64_t artifact_id = 0;
};

struct ResultMapPayload {
    std::string result_kind;
    std::int64_t result_ref_id = 0;
    std::string output_key;
    std::string output_data_kind;
    std::string output_ref_kind;
    std::int64_t output_ref_id = 0;
    std::vector<std::string> event_lines;
};

struct IResultPayloadWriter {
    virtual ~IResultPayloadWriter() = default;
    virtual bool Persist(const ResultMapPayload& payload, std::string* error_out) = 0;
};

struct WorkflowTransitionContext {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t job_set_id = 0;
    int expected_total = 0;
    int discovered_total = 0;
    int terminal_total = 0;
    int failed_total = 0;
    int priority = 0;
    std::string workflow_kind;
    std::optional<std::int64_t> workflow_graph_revision_id;
    std::string step_key;
    std::optional<std::string> input_ref_kind;
    std::optional<std::int64_t> input_ref_id;
    std::optional<std::string> output_ref_kind;
    std::optional<std::int64_t> output_ref_id;
};

struct WorkflowTransitionDecision {
    bool should_advance = false;
    // A handler-level validation failure is terminal. This is distinct from a
    // temporarily blocked transition: the coordinator must fail the step and
    // workflow instead of polling the same terminal job set forever.
    bool terminal_failure = false;
    std::optional<std::string> blocked_reason;
    std::optional<std::string> next_step_key;
    struct DynamicStep {
        std::string step_key;
        std::string step_kind;
        std::optional<std::string> input_ref_kind;
        std::optional<std::int64_t> input_ref_id;
        std::optional<std::string> guard_kind;
        std::optional<std::string> guard_value;
        int priority = 0;
        int max_attempts = 1;
    };
    std::vector<DynamicStep> spawn_steps;
};

struct IJobPersistenceAdapter {
    virtual ~IJobPersistenceAdapter() = default;
    virtual WorkflowStepScheduleResult EncodeForQueueing(const WorkflowStepScheduleContext& context) const = 0;
    virtual std::int64_t DecodeDomainRefId(const JobPersistenceRecord& persisted) const = 0;
};

struct IWorkflowGraphJobPersistenceAdapter {
    virtual ~IWorkflowGraphJobPersistenceAdapter() = default;
    virtual WorkflowStepScheduleResult EncodeForGraphQueueing(
        const WorkflowGraphStepScheduleContext& context) const = 0;
};

struct IRuntimeInitAdapter {
    virtual ~IRuntimeInitAdapter() = default;
    virtual RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const = 0;
    virtual std::optional<savor::PSJob> MaterializePsJob(std::int64_t job_id, const RuntimeInitRequest& request) const = 0;
};

struct IResultMapper {
    virtual ~IResultMapper() = default;
    virtual std::string BuildResultIniFromPrResult(std::int64_t job_id, const savor::PRResult& result) const = 0;
    virtual ResultMapPayload MapPrimaryResult(std::int64_t job_id, const std::string& result_ini) const = 0;
    virtual std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t job_id) const = 0;
};

struct IWorkflowTransitionHandler {
    virtual ~IWorkflowTransitionHandler() = default;
    virtual WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const = 0;
};

struct ProgramKindDescriptor {
    std::int32_t program_kind = 0;
    std::string program_name;

    std::shared_ptr<IJobPersistenceAdapter> job_persistence;
    std::shared_ptr<IWorkflowGraphJobPersistenceAdapter> graph_job_persistence;
    std::shared_ptr<IRuntimeInitAdapter> runtime_init;
    std::shared_ptr<IResultMapper> result_mapper;
    std::shared_ptr<IResultPayloadWriter> result_payload_writer;
    std::shared_ptr<IWorkflowTransitionHandler> workflow_transition;

    bool supports_workflow_orchestration = false;
    bool allow_mixed_success_failed_transition = false;
};

} // namespace savor::db::execution::programdb
