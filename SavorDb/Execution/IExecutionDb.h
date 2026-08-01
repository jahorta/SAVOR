#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../Common/Events/EventEnvelope.h"
#include "../Common/Events/EventPayloadViews.h"
#include "../Common/Retention/OutboxRetention.h"
#include "../Common/Types/UtcTimestamp.h"

namespace savor::db::execution::workflow {
struct IWorkflowOrchestrationCommandService;
struct IWorkflowOrchestrationQueryService;
struct WorkflowCreateInstanceCommand;
}

namespace savor::db::execution::jobs {
struct IJobEventCommandService;
}

namespace savor::db {

struct CreateJobSetCommand {
    std::optional<std::int64_t> parent_job_set_id;
    std::int32_t program_kind = 0;
    std::string purpose;
    std::optional<std::string> created_by;
    std::int64_t created_at_utc = 0;
    int priority_boost = 0;
    std::optional<int> expected_total;
    std::optional<std::string> domain_ref_kind;
    std::optional<std::int64_t> domain_ref_id;
    std::optional<std::string> meta_note;
};

struct EnqueueJobCommand {
    std::int64_t job_set_id = 0;
    std::optional<std::int64_t> parent_job_id;
    std::int32_t program_kind = 0;
    std::int32_t program_version = 1;
    std::string program_ref_kind;
    std::int64_t program_ref_id = 0;
    std::optional<std::int64_t> savestate_id;
    std::string fingerprint;
    std::int32_t priority = 0;
    std::int32_t max_attempts = 1;
    std::string input_ini;
    bool pending_until_workflow_materialized = false;
};

struct ExecutionJobRecord {
    std::int64_t job_id = 0;
    std::int64_t job_set_id = 0;
    std::int32_t program_kind = 0;
    std::int32_t program_version = 0;
    std::string program_ref_kind;
    std::int64_t program_ref_id = 0;
    std::optional<std::int64_t> savestate_id;
    std::string fingerprint;
    std::string state;
    int priority = 0;
    int attempts = 0;
    int max_attempts = 1;
    std::int64_t queued_at_utc = 0;
    std::string input_ini;
    std::optional<std::string> claimed_by_token;
    std::optional<std::int64_t> lease_expires_at_utc;
    std::optional<std::int64_t> workset_id;
    std::optional<int> workset_item_ordinal;
    std::optional<std::int64_t> dispatch_attempt_id;
    std::optional<std::uint32_t> dispatch_item_ordinal;
    std::optional<std::uint64_t> reserved_attempt_id;
    std::optional<std::int64_t> execution_finished_at_utc;
    std::optional<std::string> worker_terminal_status;
    std::optional<std::string> worker_terminal_fingerprint;
    std::optional<std::int64_t> worker_result_blob_id;
    std::optional<std::string> result_processing_state;
    int result_processing_attempts = 0;
    int result_processing_failures = 0;
    std::optional<std::int64_t> result_processing_retry_after_utc;
    std::optional<std::string> cancellation_state;
    std::optional<std::string> cancellation_group_key;
};

struct ExecutionJobSetJobRecord {
    std::int64_t job_id = 0;
    std::string state;
    std::string input_ini;
    std::optional<std::string> cancellation_group_key;
};

struct ExecutionJobSetMaterializationRecord {
    std::int64_t job_set_id = 0;
    std::string materialization_key;
    std::string materialization_state;
    std::optional<std::int64_t> parent_job_set_id;
    std::optional<std::string> domain_ref_kind;
    std::optional<std::int64_t> domain_ref_id;
    std::string purpose;
    std::optional<int> expected_total;
};

struct ExecutionJobEventRecord {
    std::int64_t job_event_id = 0;
    std::int64_t job_id = 0;
    std::string event_kind;
    std::int64_t event_ts_utc = 0;
    std::string message;
    std::optional<std::int64_t> artifact_id;
};

struct ExecutionJobOutputRecord {
    std::int64_t job_output_id = 0;
    std::int64_t job_id = 0;
    std::string output_key;
    std::string data_kind;
    std::string ref_kind;
    std::int64_t ref_id = 0;
    std::int64_t created_at_utc = 0;
};

struct RecordExecutionJobOutputCommand {
    std::int64_t job_id = 0;
    std::string output_key;
    std::string data_kind;
    std::string ref_kind;
    std::int64_t ref_id = 0;
    std::string requested_by;
};

struct ExecutionJobSetProgressDetails {
    std::int64_t job_set_id = 0;
    std::int64_t total_jobs = 0;
    std::int64_t completed_jobs = 0;
    std::int64_t succeeded_jobs = 0;
    std::int64_t failed_jobs = 0;
    std::int64_t canceled_jobs = 0;
    std::optional<std::int64_t> expected_total;
};

struct ExecutionChildJobSetProgressDetails : ExecutionJobSetProgressDetails {
    std::optional<std::int64_t> expected_delta;
    std::string purpose;
    std::string meta_note;
};

struct ClaimedExecutionJob {
    std::int64_t job_id = 0;
    std::int64_t job_set_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::string workflow_step_key;
    std::string workflow_step_kind;
    int workflow_step_priority = 0;
    std::optional<std::string> savestate_affinity_key;
    std::optional<std::string> program_runtime_affinity_key;
    std::string claimed_by_token;
    std::int64_t lease_expires_at_utc = 0;
    // The next durable exec_job.attempts value reserved by this exact claim.
    // It is committed only when the worker's ItemStarted event is accepted.
    std::uint64_t durable_attempt_id = 0;
    int priority = 0;
    std::int64_t queued_at_utc = 0;
    std::optional<std::string> previous_claimed_by_token;
    std::optional<std::int64_t> previous_lease_expires_at_utc;
};

struct ExecutionJobLeaseRequest {
    std::int64_t job_id = 0;
    std::string claimed_by_token;
};

enum class ExecutionJobLeaseRenewalDisposition {
    Renewed = 0,
    Missing,
    WrongState,
    TokenMismatch,
    Expired,
    InvalidRequest,
    BackendError,
};

struct ExecutionJobLeaseRenewalReceipt {
    std::int64_t job_id = 0;
    ExecutionJobLeaseRenewalDisposition disposition =
        ExecutionJobLeaseRenewalDisposition::BackendError;
    std::optional<std::int64_t> lease_expires_at_utc;
};

enum class ExecutionJobStartDisposition {
    Started = 0,
    AlreadyRunning,
    Missing,
    WrongState,
    TokenMismatch,
    Expired,
    InvalidRequest,
    BackendError,
};

struct ExecutionJobStartReceipt {
    std::int64_t job_id = 0;
    ExecutionJobStartDisposition disposition =
        ExecutionJobStartDisposition::BackendError;
    std::optional<std::int64_t> started_at_utc;
    std::optional<std::int64_t> lease_expires_at_utc;
    std::optional<std::uint64_t> durable_attempt_id;
};

enum class ExecutionJobStartAuthorityDisposition {
    Valid = 0,
    Missing,
    WrongState,
    TokenMismatch,
    Expired,
    DuplicateJob,
    InvalidRequest,
    BackendError,
};

struct ExecutionJobStartAuthorityItemReceipt {
    std::int64_t job_id = 0;
    ExecutionJobStartAuthorityDisposition disposition =
        ExecutionJobStartAuthorityDisposition::BackendError;
    std::optional<std::int64_t> lease_expires_at_utc;
};

struct ExecutionJobStartAuthoritySetReceipt {
    bool all_valid = false;
    std::int64_t validated_at_utc = 0;
    std::vector<ExecutionJobStartAuthorityItemReceipt> items;
};

enum class ExecutionJobTerminalAuthorityDisposition {
    Valid = 0,
    Missing,
    WrongState,
    TokenMismatch,
    AttemptMismatch,
    Expired,
    InvalidRequest,
    BackendError,
};

struct ExecutionJobTerminalAuthorityReceipt {
    std::int64_t job_id = 0;
    ExecutionJobTerminalAuthorityDisposition disposition =
        ExecutionJobTerminalAuthorityDisposition::BackendError;
    std::optional<std::uint64_t> durable_attempt_id;
    std::optional<std::int64_t> lease_expires_at_utc;
};

enum class ExecutionJobWorkerLossRecoveryDisposition {
    Requeued = 0,
    AttemptsExhaustedFailed,
    AlreadyDurable,
    Missing,
    TokenMismatch,
    AttemptMismatch,
    WrongState,
    InvalidRequest,
    BackendError,
};

struct ExecutionJobWorkerLossRecoveryReceipt {
    std::int64_t job_id = 0;
    ExecutionJobWorkerLossRecoveryDisposition disposition =
        ExecutionJobWorkerLossRecoveryDisposition::BackendError;
    std::optional<std::uint64_t> durable_attempt_id;
    std::string durable_state;
};

enum class ExecutionDbOperationDisposition {
    Applied = 0,
    AlreadyApplied,
    Missing,
    WrongState,
    TokenMismatch,
    AttemptMismatch,
    LeaseExpired,
    Conflict,
    InvalidRequest,
    BackendError,
};

struct EnsureMaterializingJobSetCommand {
    std::string materialization_key;
    std::optional<std::int64_t> parent_job_set_id;
    std::int32_t program_kind = 0;
    std::string purpose;
    std::optional<std::string> created_by;
    std::int64_t created_at_utc = 0;
    int priority_boost = 0;
    std::optional<int> expected_total;
    std::optional<std::string> domain_ref_kind;
    std::optional<std::int64_t> domain_ref_id;
    std::optional<std::string> meta_note;
};

struct EnsureMaterializingJobSetReceipt {
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
    std::int64_t job_set_id = 0;
    std::string materialization_state;
};

struct CreatePendingJobCommand {
    std::int64_t job_set_id = 0;
    std::optional<std::int64_t> parent_job_id;
    std::int32_t program_kind = 0;
    std::int32_t program_version = 1;
    std::string program_ref_kind;
    std::int64_t program_ref_id = 0;
    std::optional<std::int64_t> savestate_id;
    std::string fingerprint;
    std::int32_t priority = 0;
    std::int32_t max_attempts = 1;
    std::string input_ini;
    std::optional<std::string> cancellation_group_key;
};

struct CreatePendingJobReceipt {
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
    std::int64_t job_id = 0;
};

struct SealJobPopulationCommand {
    std::int64_t job_set_id = 0;
    int expected_job_count = 0;
    std::string requested_by;
};

struct SealJobPopulationReceipt {
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
    int durable_job_count = 0;
    std::string materialization_state;
};

struct ExecutionWorksetCompatibility {
    std::string compatibility_key;
    std::string module_canonical_id;
    std::int32_t module_version = 0;
    std::string module_sha256;
    std::string entrypoint;
    std::string verified_dependency_sha256;
    std::string runtime_profile_sha256;
    std::uint64_t required_capability_mask = 0;
    std::optional<std::string> execution_affinity_key;
    std::optional<std::string> baseline_affinity_key;
    std::uint64_t estimated_payload_bytes = 0;
};

struct PublishWorksetCommand {
    std::int64_t job_set_id = 0;
    // Full Phase invocation anchor. It may be supplied before the workflow
    // coordinator marks the step MATERIALIZED; the execution DB verifies the
    // root against job-set ancestry and the step against its durable row.
    std::int64_t workflow_step_id = 0;
    std::int64_t root_job_set_id = 0;
    std::string workset_key;
    std::int32_t program_kind = 0;
    std::int32_t program_version = 1;
    ExecutionWorksetCompatibility compatibility;
    int priority = 0;
    std::vector<std::int64_t> ordered_job_ids;
    std::string requested_by;
};

struct PublishWorksetReceipt {
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
    std::int64_t workset_id = 0;
    int item_count = 0;
};

struct CompleteWorksetPublicationCommand {
    std::int64_t job_set_id = 0;
    int expected_workset_count = 0;
    int expected_job_count = 0;
    std::string requested_by;
};

struct CompleteWorksetPublicationReceipt {
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
    int durable_workset_count = 0;
    int durable_job_count = 0;
    std::string materialization_state;
};

struct ReadyWorkerSupportedModule {
    std::string module_canonical_id;
    std::int32_t module_version = 0;
    std::string module_sha256;
    std::vector<std::string> entrypoints;
    std::string verified_dependency_sha256;
    std::string runtime_profile_sha256;
};

struct ReadyWorksetCompatibilityProfile {
    std::uint64_t available_capability_mask = 0;
    std::uint32_t max_workset_items = 0;
    std::uint64_t max_payload_bytes = 0;
    std::vector<std::int32_t> supported_program_kinds;
    std::vector<ReadyWorkerSupportedModule> supported_modules;
};

struct ClaimPublishedWorksetBatchCommand {
    std::string batch_nonce;
    std::size_t requested_workset_count = 0;
    std::int64_t lease_duration_ms = 0;
    ReadyWorksetCompatibilityProfile compatibility;
};

struct ClaimedPublishedWorksetItem {
    std::int64_t job_id = 0;
    std::int64_t job_set_id = 0;
    std::int32_t program_kind = 0;
    std::int32_t program_version = 0;
    std::string program_ref_kind;
    std::int64_t program_ref_id = 0;
    std::optional<std::int64_t> savestate_id;
    std::string fingerprint;
    int priority = 0;
    int attempts = 0;
    int max_attempts = 1;
    std::int64_t queued_at_utc = 0;
    std::string input_ini;
    int item_ordinal = 0;
    std::uint32_t dispatch_item_ordinal = 0;
    std::uint64_t reserved_attempt_id = 0;
};

struct ClaimedPublishedWorkset {
    std::int64_t workset_id = 0;
    std::int64_t dispatch_attempt_id = 0;
    std::int64_t job_set_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t root_job_set_id = 0;
    std::string workset_key;
    std::int32_t program_kind = 0;
    std::int32_t program_version = 0;
    ExecutionWorksetCompatibility compatibility;
    int priority = 0;
    std::string claim_token;
    std::int64_t lease_expires_at_utc = 0;
    std::vector<ClaimedPublishedWorksetItem> items;
};

struct RenewWorksetDispatchLeaseCommand {
    std::int64_t dispatch_attempt_id = 0;
    std::string claim_token;
    std::int64_t lease_duration_ms = 0;
};

struct WorksetDispatchLeaseReceipt {
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
    std::int64_t dispatch_attempt_id = 0;
    std::optional<std::int64_t> lease_expires_at_utc;
};

struct MarkWorksetDispatchedCommand {
    std::int64_t dispatch_attempt_id = 0;
    std::string claim_token;
    std::string requested_by;
};

struct ReleaseWorksetDispatchCommand {
    std::int64_t dispatch_attempt_id = 0;
    std::string claim_token;
    std::string reason_code;
    std::optional<std::string> reason_text;
    std::string requested_by;
};

struct WorksetDispatchMutationReceipt {
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
    std::int64_t dispatch_attempt_id = 0;
    int jobs_requeued = 0;
    int jobs_failed = 0;
    bool dispatch_closed = false;
};

struct MarkWorksetJobStartedCommand {
    std::int64_t dispatch_attempt_id = 0;
    std::string claim_token;
    std::int64_t job_id = 0;
    std::uint32_t dispatch_item_ordinal = 0;
    std::uint64_t reserved_attempt_id = 0;
    std::string worker_invocation_id;
    std::string requested_by;
};

struct WorksetJobStartReceipt {
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
    std::int64_t job_id = 0;
    std::optional<std::uint64_t> durable_attempt_id;
};

struct ExecutionTempBlobSpec {
    std::string relative_path;
    std::string sha256;
    std::uint64_t size_bytes = 0;
    std::string format;
};

struct StageWorkerTerminalCommand {
    std::int64_t dispatch_attempt_id = 0;
    std::string claim_token;
    std::int64_t job_id = 0;
    std::uint32_t dispatch_item_ordinal = 0;
    std::uint64_t reserved_attempt_id = 0;
    std::string terminal_status;
    std::string terminal_fingerprint;
    std::string terminal_id;
    std::optional<std::string> error_code;
    std::optional<std::string> error_text;
    bool unstarted = false;
    ExecutionTempBlobSpec result_blob;
    std::string requested_by;
};

struct StageWorkerTerminalReceipt {
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
    std::int64_t job_id = 0;
    std::int64_t temp_blob_id = 0;
    std::string durable_job_state;
    bool dispatch_closed = false;
};

struct ExecutionTempBlobRecord {
    std::int64_t temp_blob_id = 0;
    std::string relative_path;
    std::string sha256;
    std::uint64_t size_bytes = 0;
    std::string format;
    std::string cleanup_state;
    std::int64_t created_at_utc = 0;
};

struct ClaimExecutionFinishedJobCommand {
    std::string processor_token;
    std::int64_t lease_duration_ms = 0;
    int max_total_processing_attempts = 1;
};

struct ClaimedExecutionFinishedJob {
    ExecutionJobRecord job;
    std::int64_t workset_id = 0;
    std::int64_t dispatch_attempt_id = 0;
    std::uint64_t reserved_attempt_id = 0;
    std::uint32_t runtime_item_ordinal = 0;
    std::string workset_key;
    ExecutionWorksetCompatibility compatibility;
    std::string worker_terminal_status;
    std::string worker_terminal_fingerprint;
    std::string worker_terminal_id;
    std::optional<std::string> worker_terminal_error_code;
    std::optional<std::string> worker_terminal_error_text;
    bool worker_terminal_unstarted = false;
    ExecutionTempBlobRecord result_blob;
    std::string processor_token;
    std::int64_t processor_lease_expires_at_utc = 0;
    int processing_attempts = 0;
    int processing_failures = 0;
};

struct RenewResultProcessingLeaseCommand {
    std::int64_t job_id = 0;
    std::string processor_token;
    std::int64_t lease_duration_ms = 0;
};

struct ParkResultProcessingCommand {
    std::int64_t job_id = 0;
    std::string processor_token;
    std::string error_code;
    std::string error_text;
};

enum class ExecutionResultFinalizationDisposition {
    Final = 0,
    Retry,
};

struct ExecutionFinalizationOutput {
    std::string output_key;
    std::string data_kind;
    std::string ref_kind;
    std::int64_t ref_id = 0;
};

struct ExecutionCancellationRequestSpec {
    std::int64_t job_id = 0;
    std::string request_key;
    std::string reason_code;
    std::optional<std::string> reason_text;
    std::string requested_by;
    std::optional<std::int64_t> caused_by_job_id;
};

struct CommitResultFinalizationCommand {
    std::int64_t job_id = 0;
    std::string processor_token;
    ExecutionResultFinalizationDisposition disposition =
        ExecutionResultFinalizationDisposition::Final;
    std::optional<std::string> final_state;
    std::optional<std::string> error_code;
    std::optional<std::string> error_text;
    std::vector<ExecutionFinalizationOutput> outputs;
    std::vector<ExecutionCancellationRequestSpec> cancellation_requests;
    std::vector<std::string> event_lines;
    std::string requested_by;
};

struct ResultProcessingReceipt {
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
    std::int64_t job_id = 0;
    std::string durable_job_state;
    std::string processing_state;
    int processing_failures = 0;
    std::int64_t commit_sequence = 0;
    std::int64_t workflow_step_id = 0;
};

struct RequestJobCancellationCommand {
    std::int64_t job_id = 0;
    std::string request_key;
    std::string reason_code;
    std::optional<std::string> reason_text;
    std::string requested_by;
    std::optional<std::int64_t> caused_by_job_id;
};

struct JobCancellationReceipt {
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
    std::int64_t cancellation_request_id = 0;
    std::int64_t job_id = 0;
    std::string state;
    std::optional<std::string> resolution_code;
};

enum class JobCancellationExecutionAction {
    CancelWithoutWorker = 0,
    ReleaseClaimedWorkset,
    DeliverToWorker,
    ResolveNoLongerExecutable,
};

struct ClaimJobCancellationCommand {
    std::string delivery_token;
    std::int64_t lease_duration_ms = 0;
};

struct ClaimedJobCancellation {
    std::int64_t cancellation_request_id = 0;
    std::int64_t job_id = 0;
    std::string request_key;
    std::string reason_code;
    std::optional<std::string> reason_text;
    std::string job_state;
    std::optional<std::int64_t> workset_id;
    std::optional<std::int64_t> dispatch_attempt_id;
    std::optional<std::string> workset_claim_token;
    JobCancellationExecutionAction action =
        JobCancellationExecutionAction::ResolveNoLongerExecutable;
    std::string delivery_token;
    std::int64_t delivery_lease_expires_at_utc = 0;
};

struct MarkJobCancellationDeliveredCommand {
    std::int64_t cancellation_request_id = 0;
    std::string delivery_token;
};

struct ResolveJobCancellationCommand {
    std::int64_t cancellation_request_id = 0;
    std::optional<std::string> delivery_token;
    std::string resolution_code;
    bool finalize_job_canceled = false;
    std::string requested_by;
};

struct ClaimTempBlobCleanupCommand {
    std::string cleanup_token;
    std::int64_t lease_duration_ms = 0;
};

struct ClaimedTempBlobCleanup {
    ExecutionTempBlobRecord blob;
    std::string cleanup_token;
    std::int64_t cleanup_lease_expires_at_utc = 0;
    int cleanup_attempts = 0;
};

struct CompleteTempBlobCleanupCommand {
    std::int64_t temp_blob_id = 0;
    std::string cleanup_token;
    bool deleted = false;
    std::optional<std::string> cleanup_error;
};

struct IExecutionDb {
    virtual ~IExecutionDb() = default;

    virtual execution::workflow::IWorkflowOrchestrationQueryService* WorkflowQueryService() = 0;
    virtual execution::workflow::IWorkflowOrchestrationCommandService* WorkflowCommandService() = 0;
    virtual execution::jobs::IJobEventCommandService* JobCommandService() = 0;
    virtual bool CreateWorkflowInstance(
        const execution::workflow::WorkflowCreateInstanceCommand& command,
        std::int64_t* workflow_instance_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual bool CreateJobSet(const CreateJobSetCommand& command, std::int64_t* job_set_id_out = nullptr, std::string* error_out = nullptr) = 0;
    virtual bool EnqueueJob(const EnqueueJobCommand& command, std::int64_t* job_id_out = nullptr, std::string* error_out = nullptr) = 0;
    virtual bool EnsureMaterializingJobSet(
        const EnsureMaterializingJobSetCommand& command,
        EnsureMaterializingJobSetReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "materializing job sets are not supported";
        }
        return false;
    }
    virtual bool CreatePendingJob(
        const CreatePendingJobCommand& command,
        CreatePendingJobReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "pending workset jobs are not supported";
        }
        return false;
    }
    virtual bool SealJobPopulation(
        const SealJobPopulationCommand& command,
        SealJobPopulationReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "job population sealing is not supported";
        }
        return false;
    }
    virtual bool PublishWorkset(
        const PublishWorksetCommand& command,
        PublishWorksetReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "workset publication is not supported";
        }
        return false;
    }
    virtual bool CompleteWorksetPublication(
        const CompleteWorksetPublicationCommand& command,
        CompleteWorksetPublicationReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "workset publication completion is not supported";
        }
        return false;
    }
    virtual std::vector<ClaimedPublishedWorkset> ClaimPublishedWorksetBatch(
        const ClaimPublishedWorksetBatchCommand& command,
        std::string* error_out = nullptr) {
        (void)command;
        if (error_out != nullptr) {
            error_out->clear();
        }
        return {};
    }
    virtual bool RenewWorksetDispatchLease(
        const RenewWorksetDispatchLeaseCommand& command,
        WorksetDispatchLeaseReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "workset lease renewal is not supported";
        }
        return false;
    }
    virtual bool MarkWorksetDispatched(
        const MarkWorksetDispatchedCommand& command,
        WorksetDispatchMutationReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "workset dispatch is not supported";
        }
        return false;
    }
    virtual bool ReleaseWorksetDispatch(
        const ReleaseWorksetDispatchCommand& command,
        WorksetDispatchMutationReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "workset release is not supported";
        }
        return false;
    }
    virtual bool MarkWorksetJobStarted(
        const MarkWorksetJobStartedCommand& command,
        WorksetJobStartReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "workset item start is not supported";
        }
        return false;
    }
    virtual bool StageWorkerTerminal(
        const StageWorkerTerminalCommand& command,
        StageWorkerTerminalReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "worker terminal staging is not supported";
        }
        return false;
    }
    virtual std::optional<ClaimedExecutionFinishedJob> ClaimNextExecutionFinishedJob(
        const ClaimExecutionFinishedJobCommand& command,
        std::string* error_out = nullptr) {
        (void)command;
        if (error_out != nullptr) {
            error_out->clear();
        }
        return std::nullopt;
    }
    virtual bool RenewResultProcessingLease(
        const RenewResultProcessingLeaseCommand& command,
        ResultProcessingReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "result-processing lease renewal is not supported";
        }
        return false;
    }
    virtual bool ParkResultProcessing(
        const ParkResultProcessingCommand& command,
        ResultProcessingReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "result parking is not supported";
        }
        return false;
    }
    virtual bool CommitResultFinalization(
        const CommitResultFinalizationCommand& command,
        ResultProcessingReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "result finalization is not supported";
        }
        return false;
    }
    virtual bool RequestJobCancellation(
        const RequestJobCancellationCommand& command,
        JobCancellationReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "durable cancellation requests are not supported";
        }
        return false;
    }
    virtual std::optional<ClaimedJobCancellation> ClaimNextJobCancellation(
        const ClaimJobCancellationCommand& command,
        std::string* error_out = nullptr) {
        (void)command;
        if (error_out != nullptr) {
            error_out->clear();
        }
        return std::nullopt;
    }
    virtual bool MarkJobCancellationDelivered(
        const MarkJobCancellationDeliveredCommand& command,
        JobCancellationReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "cancellation delivery is not supported";
        }
        return false;
    }
    virtual bool ResolveJobCancellation(
        const ResolveJobCancellationCommand& command,
        JobCancellationReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "cancellation resolution is not supported";
        }
        return false;
    }
    virtual bool IsTempBlobTracked(
        std::string_view relative_path,
        bool* tracked_out,
        std::string* error_out = nullptr) const {
        (void)relative_path;
        if (tracked_out != nullptr) {
            *tracked_out = false;
        }
        if (error_out != nullptr) {
            *error_out = "temporary-blob tracking query is not supported";
        }
        return false;
    }
    virtual std::optional<ClaimedTempBlobCleanup> ClaimNextTempBlobCleanup(
        const ClaimTempBlobCleanupCommand& command,
        std::string* error_out = nullptr) {
        (void)command;
        if (error_out != nullptr) {
            error_out->clear();
        }
        return std::nullopt;
    }
    virtual bool CompleteTempBlobCleanup(
        const CompleteTempBlobCleanupCommand& command,
        ExecutionDbOperationDisposition* disposition_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (disposition_out != nullptr) {
            *disposition_out = ExecutionDbOperationDisposition::BackendError;
        }
        if (error_out != nullptr) {
            *error_out = "temporary-blob cleanup is not supported";
        }
        return false;
    }
    virtual bool RecoverExpiredWorksetDispatches(
        int max_dispatches,
        int* dispatches_recovered_out = nullptr,
        std::string* error_out = nullptr) {
        (void)max_dispatches;
        if (dispatches_recovered_out != nullptr) {
            *dispatches_recovered_out = 0;
        }
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }
    virtual bool RecoverExpiredResultProcessingLeases(
        int max_jobs,
        int* jobs_recovered_out = nullptr,
        std::string* error_out = nullptr) {
        (void)max_jobs;
        if (jobs_recovered_out != nullptr) {
            *jobs_recovered_out = 0;
        }
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }
    virtual bool RecoverExpiredCancellationDeliveryLeases(
        int max_requests,
        int* requests_recovered_out = nullptr,
        std::string* error_out = nullptr) {
        (void)max_requests;
        if (requests_recovered_out != nullptr) {
            *requests_recovered_out = 0;
        }
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }
    virtual std::optional<ClaimedExecutionJob> ClaimNextReadyExecutionJob(
        std::string_view claimed_by_token,
        std::int64_t lease_duration_ms,
        std::string* error_out = nullptr) {
        (void)claimed_by_token;
        (void)lease_duration_ms;
        if (error_out) {
            error_out->clear();
        }
        return std::nullopt;
    }
    virtual std::vector<ClaimedExecutionJob> ClaimBatchReadyExecutionJobs(
        // A caller-unique run/claim nonce. Implementations derive one exact
        // authority token per returned job without persisting a batch parent.
        std::string_view claimed_by_token,
        int requested_jobs,
        std::int64_t lease_duration_ms,
        std::string* error_out = nullptr) {
        (void)claimed_by_token;
        (void)requested_jobs;
        (void)lease_duration_ms;
        if (error_out) {
            error_out->clear();
        }
        return {};
    }
    virtual bool RenewExecutionJobLease(
        std::int64_t job_id,
        std::string_view claimed_by_token,
        std::int64_t lease_duration_ms,
        bool* renewed_out = nullptr,
        std::string* error_out = nullptr) {
        (void)job_id;
        (void)claimed_by_token;
        (void)lease_duration_ms;
        if (renewed_out) {
            *renewed_out = false;
        }
        if (error_out) {
            error_out->clear();
        }
        return true;
    }
    virtual std::vector<ExecutionJobLeaseRenewalReceipt> RenewExecutionJobLeases(
        const std::vector<ExecutionJobLeaseRequest>& requests,
        std::int64_t lease_duration_ms,
        std::string* error_out = nullptr) {
        std::vector<ExecutionJobLeaseRenewalReceipt> receipts;
        receipts.reserve(requests.size());
        for (const auto& request : requests) {
            bool renewed = false;
            std::string error;
            const bool ok = RenewExecutionJobLease(
                request.job_id,
                request.claimed_by_token,
                lease_duration_ms,
                &renewed,
                &error);
            receipts.push_back(ExecutionJobLeaseRenewalReceipt{
                .job_id = request.job_id,
                .disposition = ok && renewed
                    ? ExecutionJobLeaseRenewalDisposition::Renewed
                    : ok
                        ? ExecutionJobLeaseRenewalDisposition::WrongState
                        : ExecutionJobLeaseRenewalDisposition::BackendError,
            });
            if (!ok && error_out != nullptr && error_out->empty()) {
                *error_out = std::move(error);
            }
        }
        return receipts;
    }
    virtual bool MarkExecutionJobStarted(
        std::int64_t job_id,
        std::string_view claimed_by_token,
        std::string_view requested_by,
        ExecutionJobStartReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)job_id;
        (void)claimed_by_token;
        (void)requested_by;
        if (receipt_out != nullptr) {
            *receipt_out = ExecutionJobStartReceipt{
                .job_id = job_id,
                .disposition = ExecutionJobStartDisposition::BackendError,
            };
        }
        if (error_out != nullptr) {
            *error_out = "atomic execution-job start is not supported";
        }
        return false;
    }
    virtual bool ValidateExecutionJobStartAuthoritySet(
        const std::vector<ExecutionJobLeaseRequest>& requests,
        ExecutionJobStartAuthoritySetReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        ExecutionJobStartAuthoritySetReceipt receipt{};
        receipt.items.reserve(requests.size());
        for (const auto& request : requests) {
            receipt.items.push_back(
                ExecutionJobStartAuthorityItemReceipt{
                    .job_id = request.job_id,
                    .disposition =
                        ExecutionJobStartAuthorityDisposition::
                            BackendError,
                });
        }
        if (receipt_out != nullptr) {
            *receipt_out = std::move(receipt);
        }
        if (error_out != nullptr) {
            *error_out =
                "exact-set execution-job start authority validation "
                "is not supported";
        }
        return false;
    }
    virtual bool ConfirmExecutionJobTerminalAuthority(
        std::int64_t job_id,
        std::string_view claimed_by_token,
        std::uint64_t durable_attempt_id,
        std::int64_t lease_duration_ms,
        ExecutionJobTerminalAuthorityReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)claimed_by_token;
        (void)durable_attempt_id;
        (void)lease_duration_ms;
        if (receipt_out != nullptr) {
            *receipt_out = ExecutionJobTerminalAuthorityReceipt{
                .job_id = job_id,
                .disposition =
                    ExecutionJobTerminalAuthorityDisposition::BackendError,
            };
        }
        if (error_out != nullptr) {
            *error_out =
                "exact terminal authority confirmation is not supported";
        }
        return false;
    }
    virtual bool RecoverExecutionJobAfterWorkerLoss(
        std::int64_t job_id,
        std::string_view claimed_by_token,
        std::uint64_t durable_attempt_id,
        std::string_view message,
        ExecutionJobWorkerLossRecoveryReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)claimed_by_token;
        (void)durable_attempt_id;
        (void)message;
        if (receipt_out != nullptr) {
            *receipt_out = ExecutionJobWorkerLossRecoveryReceipt{
                .job_id = job_id,
                .disposition =
                    ExecutionJobWorkerLossRecoveryDisposition::BackendError,
            };
        }
        if (error_out != nullptr) {
            *error_out =
                "exact worker-loss recovery is not supported";
        }
        return false;
    }
    virtual bool RequeueExpiredExecutionLeases(
        int* rows_requeued_out = nullptr,
        std::string* error_out = nullptr) {
        if (rows_requeued_out) {
            *rows_requeued_out = 0;
        }
        if (error_out) {
            error_out->clear();
        }
        return true;
    }
    virtual bool RequeueExpiredClaimedExecutionJobs(
        int* rows_requeued_out = nullptr,
        std::string* error_out = nullptr) {
        if (rows_requeued_out) {
            *rows_requeued_out = 0;
        }
        if (error_out) {
            error_out->clear();
        }
        return true;
    }
    virtual bool RequeueClaimedExecutionJob(
        std::int64_t job_id,
        std::string_view claimed_by_token,
        std::string_view message,
        std::string* error_out = nullptr) {
        // The exact token is the authority boundary. Implementations may
        // recover either CLAIMED work that never started or RUNNING work
        // after its exact worker invocation has terminalized.
        (void)job_id;
        (void)claimed_by_token;
        (void)message;
        if (error_out) {
            error_out->clear();
        }
        return true;
    }
    virtual bool RequeueInterruptedExecutionJobs(
        int* rows_requeued_out = nullptr,
        std::string* error_out = nullptr) {
        if (rows_requeued_out) {
            *rows_requeued_out = 0;
        }
        if (error_out) {
            error_out->clear();
        }
        return true;
    }
    virtual std::optional<ExecutionJobRecord> GetJob(std::int64_t job_id) const = 0;
    virtual std::vector<ExecutionJobEventRecord> ListJobEvents(std::int64_t job_id, int limit = 128) const {
        (void)job_id;
        (void)limit;
        return {};
    }
    virtual bool RecordJobOutput(const RecordExecutionJobOutputCommand& command, std::string* error_out = nullptr) {
        (void)command;
        if (error_out) {
            *error_out = "record job output is not supported by this execution db";
        }
        return false;
    }
    virtual std::vector<ExecutionJobOutputRecord> ListJobOutputsForWorkflowStep(std::int64_t workflow_step_id) const {
        (void)workflow_step_id;
        return {};
    }
    virtual std::optional<std::string> GetJobInputIni(std::int64_t job_id, std::string* error_out = nullptr) const {
        const auto job = GetJob(job_id);
        if (!job.has_value()) {
            if (error_out) {
                *error_out = "job not found";
            }
            return std::nullopt;
        }
        if (error_out) {
            error_out->clear();
        }
        return job->input_ini;
    }
    virtual bool RequeueJob(std::int64_t job_id, std::string* error_out = nullptr) {
        (void)job_id;
        if (error_out) {
            *error_out = "requeue job is not supported by this execution db";
        }
        return false;
    }
    virtual bool RestartFailedJob(std::int64_t job_id, std::optional<std::string> input_ini_override = std::nullopt, std::string* error_out = nullptr) {
        (void)job_id;
        (void)input_ini_override;
        if (error_out) {
            *error_out = "restart job is not supported by this execution db";
        }
        return false;
    }
    virtual bool CancelQueuedOrClaimedJob(std::int64_t job_id, std::string* error_out = nullptr) {
        (void)job_id;
        if (error_out) {
            *error_out = "cancel job is not supported by this execution db";
        }
        return false;
    }
    virtual std::optional<ExecutionJobSetProgressDetails> GetJobSetProgress(std::int64_t job_set_id) const {
        (void)job_set_id;
        return std::nullopt;
    }
    virtual std::vector<ExecutionJobSetJobRecord> ListJobsInJobSet(
        std::int64_t job_set_id) const {
        (void)job_set_id;
        return {};
    }
    virtual std::vector<ExecutionJobSetJobRecord>
    ListJobsByProgramReference(
        std::int32_t program_kind,
        std::string_view program_ref_kind,
        std::int64_t program_ref_id) const {
        (void)program_kind;
        (void)program_ref_kind;
        (void)program_ref_id;
        return {};
    }
    virtual std::optional<ExecutionJobSetMaterializationRecord>
    GetJobSetByMaterializationKey(
        std::string_view materialization_key) const {
        (void)materialization_key;
        return std::nullopt;
    }
    virtual std::vector<ExecutionChildJobSetProgressDetails> GetChildJobSetProgress(std::int64_t parent_job_set_id) const {
        (void)parent_job_set_id;
        return {};
    }
    virtual bool MarkQueuedJobsSuperseded(
        std::int64_t job_set_id,
        std::int64_t except_job_id,
        std::string* error_out = nullptr,
        int* rows_superseded_out = nullptr) = 0;


    virtual retention::OutboxRetentionPreview PreviewOutboxRetention(
        const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
        types::UtcTimePoint now_utc,
        const retention::OutboxRetentionPolicy& policy) const = 0;

    virtual bool PurgeOutboxThroughRetentionFloor(
        const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
        types::UtcTimePoint now_utc,
        const retention::OutboxRetentionPolicy& policy,
        int max_rows,
        int* rows_deleted_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool PurgeWorkflowHandlerDedupeOlderThan(
        std::int64_t last_seen_at_utc_exclusive,
        int max_rows,
        int* rows_deleted_out = nullptr,
        std::string* error_out = nullptr) = 0;

    // Resolves execution workflow/job payload references to typed v1 view fields.
    virtual std::optional<events::ExecutionWorkflowJobPayloadView> ResolveExecutionWorkflowJobPayload(
        const events::EventEnvelope& envelope) const = 0;

    // Dispatch-key variant for projector/consumer code paths that already split key fields.
    virtual std::optional<events::ExecutionWorkflowJobPayloadView> ResolveExecutionWorkflowJobPayload(
        std::string_view event_type,
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;
};

} // namespace savor::db
