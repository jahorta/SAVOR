#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../../SavorCore/Runner/Runtime/Worksets/WorksetTypes.h"
#include "../../../SavorCore/Runner/Runtime/FullPhase/FullPhaseProgram.h"

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
    std::optional<std::string> domain_ref_kind;
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
    std::optional<std::string> authored_ref_kind;
    std::optional<std::int64_t> authored_ref_id;
    int step_priority = 0;
    std::vector<WorkflowGraphInputBinding> input_bindings;
    std::vector<WorkflowGraphArgument> arguments;
};

struct ProgramJobMaterializationContext {
    WorkflowStepScheduleContext step;
    std::optional<WorkflowGraphStepScheduleContext> graph;
};

enum class ProgramJobContinuationDisposition {
    AddedWork = 0,
    Complete,
    Failed,
};

struct ProgramJobContinuationOutput {
    std::string output_key;
    std::string data_kind;
    std::string ref_kind;
    std::int64_t ref_id = 0;
};

struct ProgramJobContinuationContext {
    ProgramJobMaterializationContext materialization;
    std::int64_t root_job_set_id = 0;
    int expected_total = 0;
    int discovered_total = 0;
    int terminal_total = 0;
    int failed_total = 0;
};

struct ProgramJobContinuationResult {
    ProgramJobContinuationDisposition disposition =
        ProgramJobContinuationDisposition::Complete;
    std::optional<ProgramJobContinuationOutput> output;
    std::optional<std::string> failure_code;
    std::optional<std::string> failure_text;
    std::vector<std::string> event_lines;
};

struct IProgramJobMaterializer {
    virtual ~IProgramJobMaterializer() = default;

    // The descriptor owns incremental fanout and durable grouping. A
    // successful return means it has:
    //   1. ensured the materializing job set,
    //   2. created the complete PENDING_WORKSET population,
    //   3. sealed that population,
    //   4. published every immutable workset, and
    //   5. completed workset publication.
    // Implementations must be idempotent so the workflow coordinator can
    // invoke the same step again after interruption.
    virtual bool Materialize(
        const ProgramJobMaterializationContext& context,
        WorkflowStepScheduleResult* result_out,
        std::string* error_out) const = 0;

    // Called after every currently published descendant job is business-final.
    // A descriptor may idempotently publish another child job set, finish the
    // workflow step, or fail it. Other program kinds need no special handling
    // and therefore complete by default.
    virtual bool Continue(
        const ProgramJobContinuationContext& context,
        ProgramJobContinuationResult* result_out,
        std::string* error_out) const {
        (void)context;
        if (result_out != nullptr) {
            *result_out = ProgramJobContinuationResult{};
        }
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }
};

// Durable workset membership and dispatch authority are selected by the
// execution DB. A descriptor may reconstruct runtime payloads for that exact
// ordered membership, but it may not regroup, omit, or add jobs here.
struct WorksetReconstructionItem {
    std::int64_t job_id = 0;
    std::uint32_t workset_item_ordinal = 0;
    std::uint64_t reserved_attempt_id = 0;
    std::string claim_token;
    std::int32_t program_kind = 0;
    std::int32_t program_version = 0;
    std::string program_ref_kind;
    std::int64_t program_ref_id = 0;
    std::optional<std::int64_t> savestate_id;
    std::string fingerprint;
    std::string input_ini;
};

struct WorksetReconstructionContext {
    std::int64_t workset_id = 0;
    std::int64_t dispatch_attempt_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t root_job_set_id = 0;
    std::string dispatch_token;
    std::string contract_key;
    savor::runtime::ArtifactCompatibilityToken state_compatibility;
    savor::runtime::derived::WorksetDerivedStateBindingV1 derived_state;
    std::optional<savor::runtime::WorksetCaptureBindingV1> capture;
    savor::runtime::progress::ProgressPlanV1 progress_plan;
    std::vector<WorksetReconstructionItem> items;
};

struct WorksetReconstructionResult {
    savor::runtime::WorkerWorksetDefinition workset;
    // This redundant identity list makes the no-regrouping invariant explicit
    // and cheap for the JobExecutionCoordinator to validate before submission.
    std::vector<std::int64_t> ordered_job_ids;
};

struct IWorksetReconstructionAdapter {
    virtual ~IWorksetReconstructionAdapter() = default;
    virtual std::optional<WorksetReconstructionResult> Reconstruct(
        const WorksetReconstructionContext& context,
        std::string* error_out) const = 0;
};

struct WorkerTerminalObservation {
    std::int64_t job_id = 0;
    std::int64_t workset_id = 0;
    std::int64_t dispatch_attempt_id = 0;
    std::uint64_t reserved_attempt_id = 0;
    std::string format;
    std::string sha256;
    std::vector<std::uint8_t> envelope;
};

struct ProgramResultOutput {
    std::string output_key;
    std::string data_kind;
    std::string ref_kind;
    std::int64_t ref_id = 0;
};

struct ProgramResultCancellation {
    std::int64_t job_id = 0;
    std::string request_key;
    std::string reason_code;
    std::string reason_text;
    std::string terminal_disposition;
};

struct ProgramResultStagingFile {
    std::string relative_path;
    std::string sha256;
    std::uint64_t size_bytes = 0;
};

enum class ProgramResultDisposition {
    Finalize = 0,
    RetryExecution,
};

struct ProgramResultDecision {
    ProgramResultDisposition disposition = ProgramResultDisposition::Finalize;
    // Set only after the handler has durably persisted the terminal's domain
    // outcome. Malformed or otherwise unpersisted results must leave this off.
    bool cleanup_worker_staging = false;
    // Used only for Finalize. Program kinds own the business outcome; the
    // generic result processor deliberately does not infer success/failure
    // from a worker terminal.
    std::string final_job_state;
    std::optional<std::string> error_code;
    std::optional<std::string> error_text;
    std::vector<ProgramResultOutput> outputs;
    std::vector<ProgramResultCancellation> cancellations;
    std::vector<ProgramResultStagingFile> staging_files;
    std::vector<std::string> event_lines;
};

struct ProgramResultProcessingContext {
    std::int64_t job_id = 0;
    std::int64_t job_set_id = 0;
    std::int32_t program_kind = 0;
    std::int32_t program_version = 0;
    std::string program_ref_kind;
    std::int64_t program_ref_id = 0;
    std::string fingerprint;
    std::string input_ini;
    WorkerTerminalObservation terminal;
};

struct IProgramResultHandler {
    virtual ~IProgramResultHandler() = default;
    // Domain writes performed here must be idempotent. Returning a decision
    // means those writes are complete; the processor commits the execution
    // job's final state only afterwards.
    virtual ProgramResultDecision Process(
        const ProgramResultProcessingContext& context) const = 0;
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

struct IWorkflowTransitionHandler {
    virtual ~IWorkflowTransitionHandler() = default;
    virtual WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const = 0;
};

struct ProgramKindDescriptor {
    std::int32_t program_kind = 0;
    std::string program_name;
    std::filesystem::path result_staging_root;
    std::optional<savor::runtime::fullphase::FullPhaseProgramIdentity>
        full_phase_identity;
    // Presence is mandatory, including when the explicit default is empty.
    // Coordination resolves these registered libraries into a concrete
    // workset ProgressPlanV1 before dispatch.
    std::optional<std::vector<std::string>>
        default_progress_library_ids;
    // Presence is mandatory, including when no derived-state block is needed.
    std::optional<std::vector<std::string>>
        default_derived_state_block_ids;
    std::vector<std::uint32_t> default_progress_runtime_trigger_pcs;

    std::shared_ptr<IWorkflowTransitionHandler> workflow_transition;

    // Complete production coordination contracts.
    std::shared_ptr<IProgramJobMaterializer> job_materializer;
    std::shared_ptr<IWorksetReconstructionAdapter> workset_reconstruction;
    std::shared_ptr<IProgramResultHandler> result_handler;

    bool supports_workflow_orchestration = false;
    bool allow_mixed_success_failed_transition = false;
};

} // namespace savor::db::execution::programdb
