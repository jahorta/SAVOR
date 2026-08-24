#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
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
    std::optional<std::uint64_t> reserved_attempt_id;
    std::optional<std::int64_t> execution_finished_at_utc;
    std::optional<std::string> worker_terminal_status;
    std::optional<std::string> worker_terminal_fingerprint;
    std::optional<std::int64_t> worker_result_blob_id;
    std::optional<std::string> result_processing_state;
    int result_processing_attempts = 0;
    int result_processing_failures = 0;
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
    std::int64_t settled_jobs = 0;
    std::int64_t succeeded_jobs = 0;
    std::int64_t failed_jobs = 0;
    std::int64_t interrupted_jobs = 0;
    std::int64_t superseded_jobs = 0;
    std::int64_t canceled_jobs = 0;
    std::optional<std::int64_t> expected_total;
};

struct ClaimedExecutionJob {
    std::int64_t job_id = 0;
    std::int64_t job_set_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::string workflow_step_key;
    std::string workflow_step_kind;
    int workflow_step_priority = 0;
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
    Interrupted = 0,
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

struct ExecutionWorksetContract {
    std::string contract_key;
    std::string module_canonical_id;
    std::int32_t module_version = 0;
    std::string module_sha256;
    std::string entrypoint;
    std::string verified_dependency_sha256;
    std::string runtime_profile_sha256;
    std::string program_package_sha256;
    std::optional<std::string> execution_affinity_key;
    std::uint64_t estimated_payload_bytes = 0;
};

// Exact immutable observation products selected before workset publication.
// ExecutionDb stores these bytes opaquely; coordination decodes and verifies
// them before invoking any program-kind reconstruction adapter.
struct ExecutionWorksetObservationBindingV1 {
    std::vector<std::uint8_t> capture_binding_payload;
    std::string capture_binding_sha256;
    std::vector<std::uint8_t> progress_plan_payload;
    std::string progress_plan_sha256;
};

// Exact execution-authoritative derived-state selection. It is deliberately
// separate from output-only capture and progress observation products.
struct ExecutionWorksetDerivedStateBindingV1 {
    std::vector<std::uint8_t> binding_payload;
    std::string binding_sha256;
};

struct PublishWorksetCommand {
    std::int64_t job_set_id = 0;
    // Full Phase invocation anchor. The workset's job_set_id must be the flat
    // job set owned by workflow_step_id.
    std::int64_t workflow_step_id = 0;
    std::string workset_key;
    std::int32_t program_kind = 0;
    std::int32_t program_version = 1;
    ExecutionWorksetContract contract;
    ExecutionWorksetDerivedStateBindingV1 derived_state;
    ExecutionWorksetObservationBindingV1 observation;
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

struct PublishWorksetWaveCommand {
    std::int64_t job_set_id = 0;
    int expected_job_count = 0;
    std::vector<PublishWorksetCommand> worksets;
    std::string requested_by;
};

struct PublishWorksetWaveReceipt {
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
    std::vector<PublishWorksetReceipt> worksets;
    int durable_workset_count = 0;
    int durable_job_count = 0;
    std::string materialization_state;
    bool ready_workset_availability_changed = false;
};

// A terminal workset-backed job together with the immutable workset envelope
// required to reconstruct it again. The organizer reads these records, then
// returns a plan that reassigns the same job identities to fresh worksets.
struct FailedWorkflowWorksetJobRecord {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t job_id = 0;
    std::int64_t job_set_id = 0;
    std::int64_t source_workset_id = 0;
    int attempts = 0;
    int priority = 0;
    std::uint64_t estimated_item_payload_bytes = 0;
    PublishWorksetCommand source_workset;
};

struct ReorganizedWorksetPlanEntry {
    std::int64_t workflow_step_id = 0;
    std::int64_t job_set_id = 0;
    std::int32_t program_kind = 0;
    std::int32_t program_version = 0;
    ExecutionWorksetContract contract;
    ExecutionWorksetDerivedStateBindingV1 derived_state;
    ExecutionWorksetObservationBindingV1 observation;
    int priority = 0;
    std::uint64_t estimated_payload_bytes = 0;
    std::vector<std::int64_t> ordered_job_ids;
};

struct WorksetJobReorganizationPlan {
    std::int64_t workflow_instance_id = 0;
    std::vector<ReorganizedWorksetPlanEntry> worksets;
    std::string requested_by;
};

struct WorksetJobReorganizationReceipt {
    int requeued_job_count = 0;
    int created_workset_count = 0;
    std::vector<std::int64_t> workset_ids;
};

struct ClaimPublishedWorksetBatchCommand {
    std::string batch_nonce;
    std::size_t requested_workset_count = 0;
};

struct ExecutionWorkAvailabilitySnapshot {
    std::uint64_t generation = 0;
    bool has_ready_worksets = false;
    bool has_execution_finished_results = false;
    std::int64_t changed_at_utc = 0;

    auto operator<=>(const ExecutionWorkAvailabilitySnapshot&) const = default;
};

using ExecutionWorkAvailabilityCallback =
    std::function<void(const ExecutionWorkAvailabilitySnapshot&)>;
using ExecutionWorkAvailabilitySubscription = std::uint64_t;

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
    std::uint32_t workset_item_ordinal = 0;
    std::uint64_t reserved_attempt_id = 0;
};

struct ClaimedPublishedWorkset {
    std::int64_t workset_id = 0;
    std::int64_t dispatch_attempt_id = 0;
    std::int64_t job_set_id = 0;
    std::int64_t workflow_step_id = 0;
    std::string workset_key;
    std::int32_t program_kind = 0;
    std::int32_t program_version = 0;
    ExecutionWorksetContract contract;
    ExecutionWorksetDerivedStateBindingV1 derived_state;
    ExecutionWorksetObservationBindingV1 observation;
    int priority = 0;
    std::string claim_token;
    std::vector<ClaimedPublishedWorksetItem> items;
};

struct WorksetDispatchLeaseRequest {
    std::int64_t dispatch_attempt_id = 0;
    std::string claim_token;
};

struct RenewActiveWorksetLeasesCommand {
    std::vector<WorksetDispatchLeaseRequest> requests;
    std::int64_t lease_duration_ms = 0;
};

struct WorksetDispatchLeaseReceipt {
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
    std::int64_t dispatch_attempt_id = 0;
    std::optional<std::int64_t> lease_expires_at_utc;
};

struct MarkWorksetActiveCommand {
    std::int64_t dispatch_attempt_id = 0;
    std::string claim_token;
    std::int64_t lease_duration_ms = 0;
    std::string requested_by;
};

struct MarkWorksetDrainingCommand {
    std::int64_t dispatch_attempt_id = 0;
    std::string claim_token;
    std::string requested_by;
};

struct RecoverInterruptedWorksetDispatchesReceipt {
    int dispatches_closed = 0;
    int jobs_interrupted = 0;
    int jobs_requeued = 0;
    int created_worksets = 0;
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
    int jobs_interrupted = 0;
    int created_worksets = 0;
    bool dispatch_closed = false;
    std::optional<std::int64_t> lease_expires_at_utc;
};

struct MarkWorksetJobStartedCommand {
    std::int64_t dispatch_attempt_id = 0;
    std::string claim_token;
    std::int64_t job_id = 0;
    std::uint32_t workset_item_ordinal = 0;
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

struct RecordCanonicalJobProgressCommand {
    std::int64_t dispatch_attempt_id = 0;
    std::string claim_token;
    std::int64_t job_id = 0;
    std::uint32_t workset_item_ordinal = 0;
    std::uint64_t reserved_attempt_id = 0;
    std::uint64_t workset_id = 0;
    std::uint64_t item_id = 0;
    std::uint64_t invocation_id = 0;
    std::uint64_t ordinal = 0;
    std::string library_id;
    std::uint32_t library_revision = 0;
    std::string progress_point_id;
    bool has_routed_provenance = false;
    std::uint64_t routed_sequence = 0;
    std::uint64_t sample_snapshot_id = 0;
    std::uint64_t trigger_epoch = 0;
    std::string schema_id;
    std::uint32_t schema_revision = 0;
    std::string schema_sha256;
    std::vector<std::uint8_t> typed_payload;
    std::string display_text;
    std::string requested_by;
};

struct CanonicalJobProgressReceipt {
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
    std::int64_t job_id = 0;
    std::uint64_t attempt_id = 0;
    std::uint64_t ordinal = 0;
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
    std::uint32_t workset_item_ordinal = 0;
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

using WorkerExecutionEventMutation = std::variant<
    MarkWorksetJobStartedCommand,
    RecordCanonicalJobProgressCommand,
    StageWorkerTerminalCommand>;
using WorkerExecutionEventMutationReceipt = std::variant<
    WorksetJobStartReceipt,
    CanonicalJobProgressReceipt,
    StageWorkerTerminalReceipt>;

struct PersistWorkerExecutionEventsBatchCommand {
    std::vector<WorkerExecutionEventMutation> events;
};

struct PersistWorkerExecutionEventsBatchReceipt {
    std::vector<WorkerExecutionEventMutationReceipt> events;
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

struct ClaimExecutionFinishedJobsBatchCommand {
    std::size_t requested_job_count = 32;
};

struct ClaimedExecutionFinishedJob {
    ExecutionJobRecord job;
    std::int64_t workset_id = 0;
    std::int64_t dispatch_attempt_id = 0;
    std::uint64_t reserved_attempt_id = 0;
    std::uint32_t runtime_item_ordinal = 0;
    std::string workset_key;
    ExecutionWorksetContract contract;
    std::string worker_terminal_status;
    std::string worker_terminal_fingerprint;
    std::string worker_terminal_id;
    std::optional<std::string> worker_terminal_error_code;
    std::optional<std::string> worker_terminal_error_text;
    bool worker_terminal_unstarted = false;
    std::optional<std::string> cancellation_terminal_disposition;
    ExecutionTempBlobRecord result_blob;
    int processing_attempts = 0;
    int processing_failures = 0;
};

struct InterruptedResultProcessingJob {
    ClaimedExecutionFinishedJob claimed;
    bool structural_metadata_complete = false;
    std::string structural_diagnostic;
    bool has_result_blob_record = false;
};

struct ResetInterruptedResultProcessingCommand {
    std::int64_t job_id = 0;
    std::string requested_by;
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

struct ExecutionResultStagingFileSpec {
    std::string relative_path;
    std::string sha256;
    std::uint64_t size_bytes = 0;
};

struct ExecutionCancellationRequestSpec {
    std::int64_t job_id = 0;
    std::string request_key;
    std::string reason_code;
    std::optional<std::string> reason_text;
    std::string requested_by;
    std::optional<std::int64_t> caused_by_job_id;
    std::string terminal_disposition;
};

struct CommitResultFinalizationCommand {
    std::int64_t job_id = 0;
    ExecutionResultFinalizationDisposition disposition =
        ExecutionResultFinalizationDisposition::Final;
    std::optional<std::string> final_state;
    std::optional<std::string> error_code;
    std::optional<std::string> error_text;
    std::vector<ExecutionFinalizationOutput> outputs;
    std::vector<ExecutionCancellationRequestSpec> cancellation_requests;
    std::vector<ExecutionResultStagingFileSpec> staging_files;
    std::vector<std::string> event_lines;
    std::string requested_by;
};

struct CommittedJobCancellation {
    std::int64_t cancellation_request_id = 0;
    std::int64_t job_id = 0;
    std::string request_key;
    std::string reason_code;
    std::optional<std::string> reason_text;
    std::optional<std::int64_t> caused_by_job_id;
    std::string terminal_disposition;
    std::string state;
    std::string durable_job_state;
    std::optional<std::int64_t> workset_id;
    std::optional<std::int64_t> dispatch_attempt_id;
    std::optional<std::string> claim_token;
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
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
    std::vector<CommittedJobCancellation> committed_cancellations;
};

struct CommitResultFinalizationsBatchCommand {
    std::vector<CommitResultFinalizationCommand> finalizations;
};

struct JobCancellationReceipt {
    ExecutionDbOperationDisposition disposition =
        ExecutionDbOperationDisposition::BackendError;
    std::int64_t cancellation_request_id = 0;
    std::int64_t job_id = 0;
    std::string state;
    std::optional<std::string> resolution_code;
};

enum class JobCancellationOutcomeKind {
    ResolveWithoutWorker = 0,
    InitialSidecarApplied,
    WorkerDeliveryAccepted,
    WorkerTerminalResolved,
    DeliveryFailed,
};

struct JobCancellationOutcomeCommand {
    JobCancellationOutcomeKind kind =
        JobCancellationOutcomeKind::ResolveWithoutWorker;
    std::int64_t cancellation_request_id = 0;
    std::int64_t job_id = 0;
    std::optional<std::int64_t> dispatch_attempt_id;
    std::optional<std::string> claim_token;
    std::string resolution_code;
    std::optional<std::string> error_code;
    std::optional<std::string> error_text;
    std::string requested_by;
};

struct MutateJobCancellationsBatchCommand {
    std::vector<JobCancellationOutcomeCommand> mutations;
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

struct ResultStagingCleanupRecord {
    std::int64_t cleanup_id = 0;
    std::int64_t job_id = 0;
    std::string terminal_sha256;
    std::int32_t program_kind = 0;
    std::string relative_path;
    std::string expected_sha256;
    std::uint64_t expected_size_bytes = 0;
    std::string cleanup_state;
    int cleanup_attempts = 0;
};

struct ClaimResultStagingCleanupCommand {
    std::string cleanup_token;
    std::int64_t lease_duration_ms = 0;
};

struct ClaimedResultStagingCleanup {
    ResultStagingCleanupRecord cleanup;
    std::string cleanup_token;
    std::int64_t cleanup_lease_expires_at_utc = 0;
};

enum class ResultStagingCleanupCompletion { Deleted = 0, Retry, Blocked };

struct CompleteResultStagingCleanupCommand {
    std::int64_t cleanup_id = 0;
    std::string cleanup_token;
    ResultStagingCleanupCompletion completion =
        ResultStagingCleanupCompletion::Retry;
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
    virtual bool PublishWorksetWave(
        const PublishWorksetWaveCommand& command,
        PublishWorksetWaveReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "workset wave publication is not supported";
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
    virtual std::vector<WorksetDispatchLeaseReceipt>
    RenewActiveWorksetLeases(
        const RenewActiveWorksetLeasesCommand& command,
        std::string* error_out = nullptr) {
        (void)command;
        if (error_out != nullptr) {
            *error_out = "active workset lease renewal is not supported";
        }
        return {};
    }
    virtual std::optional<ExecutionWorkAvailabilitySnapshot>
    GetExecutionWorkAvailability(
        std::string* error_out = nullptr) const {
        if (error_out != nullptr) {
            *error_out = "execution-work availability is not supported";
        }
        return std::nullopt;
    }
    virtual ExecutionWorkAvailabilitySubscription
    SubscribeExecutionWorkAvailability(
        ExecutionWorkAvailabilityCallback callback) {
        (void)callback;
        return 0;
    }
    virtual void UnsubscribeExecutionWorkAvailability(
        ExecutionWorkAvailabilitySubscription subscription) {
        (void)subscription;
    }
    virtual bool MarkWorksetActive(
        const MarkWorksetActiveCommand& command,
        WorksetDispatchMutationReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "active workset transition is not supported";
        }
        return false;
    }
    virtual bool MarkWorksetDraining(
        const MarkWorksetDrainingCommand& command,
        WorksetDispatchMutationReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "draining workset transition is not supported";
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
    virtual bool PersistWorkerExecutionEventsBatch(
        const PersistWorkerExecutionEventsBatchCommand& command,
        PersistWorkerExecutionEventsBatchReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "worker execution-event batching is not supported";
        }
        return false;
    }
    virtual std::vector<ClaimedExecutionFinishedJob>
    ClaimExecutionFinishedJobsBatch(
        const ClaimExecutionFinishedJobsBatchCommand& command,
        std::string* error_out = nullptr) {
        (void)command;
        if (error_out != nullptr) {
            error_out->clear();
        }
        return {};
    }
    virtual std::vector<InterruptedResultProcessingJob>
    ListInterruptedResultProcessingJobs(
        std::string* error_out = nullptr) {
        if (error_out != nullptr) {
            error_out->clear();
        }
        return {};
    }
    virtual bool ResetInterruptedResultProcessing(
        const ResetInterruptedResultProcessingCommand& command,
        ResultProcessingReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipt_out != nullptr) {
            *receipt_out = {};
        }
        if (error_out != nullptr) {
            *error_out = "interrupted result reset is not supported";
        }
        return false;
    }
    virtual bool CommitResultFinalizationsBatch(
        const CommitResultFinalizationsBatchCommand& command,
        std::vector<ResultProcessingReceipt>* receipts_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipts_out != nullptr) {
            receipts_out->clear();
        }
        if (error_out != nullptr) {
            *error_out = "result finalization batching is not supported";
        }
        return false;
    }
    virtual std::vector<CommittedJobCancellation>
    ListUnresolvedJobCancellations(
        std::string* error_out = nullptr) {
        if (error_out != nullptr) {
            error_out->clear();
        }
        return {};
    }
    virtual bool MutateJobCancellationsBatch(
        const MutateJobCancellationsBatchCommand& command,
        std::vector<JobCancellationReceipt>* receipts_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (receipts_out != nullptr) {
            receipts_out->clear();
        }
        if (error_out != nullptr) {
            *error_out = "cancellation mutation batching is not supported";
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
    virtual std::optional<ClaimedResultStagingCleanup>
    ClaimNextResultStagingCleanup(
        const ClaimResultStagingCleanupCommand& command,
        std::string* error_out = nullptr) {
        (void)command;
        if (error_out != nullptr) error_out->clear();
        return std::nullopt;
    }
    virtual bool CompleteResultStagingCleanup(
        const CompleteResultStagingCleanupCommand& command,
        ExecutionDbOperationDisposition* disposition_out = nullptr,
        std::string* error_out = nullptr) {
        (void)command;
        if (disposition_out != nullptr)
            *disposition_out = ExecutionDbOperationDisposition::BackendError;
        if (error_out != nullptr)
            *error_out = "result-staging cleanup is not supported";
        return false;
    }
    virtual bool GetResultStagingCleanupCount(
        std::int64_t* count_out,
        std::string* error_out = nullptr) const {
        if (count_out != nullptr) *count_out = 0;
        if (error_out != nullptr)
            *error_out = "result-staging cleanup count is not supported";
        return false;
    }
    virtual bool ClearResultStagingCleanupQueue(
        std::int64_t* rows_deleted_out = nullptr,
        std::string* error_out = nullptr) {
        if (rows_deleted_out != nullptr) *rows_deleted_out = 0;
        if (error_out != nullptr)
            *error_out = "result-staging cleanup reset is not supported";
        return false;
    }
    virtual bool RecoverInterruptedWorksetDispatches(
        RecoverInterruptedWorksetDispatchesReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        if (receipt_out != nullptr) {
            *receipt_out = {};
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
    virtual std::optional<ExecutionJobRecord> GetExecutionJob(std::int64_t job_id) const = 0;
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
        const auto job = GetExecutionJob(job_id);
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
    virtual std::vector<FailedWorkflowWorksetJobRecord>
    ListFailedWorkflowWorksetJobs(std::int64_t workflow_instance_id) const {
        (void)workflow_instance_id;
        return {};
    }
    virtual bool ApplyWorksetJobReorganization(
        const WorksetJobReorganizationPlan& plan,
        WorksetJobReorganizationReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) {
        (void)plan;
        if (error_out) {
            *error_out = "workset job reorganization is not supported by this execution db";
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
