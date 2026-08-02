#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "../IExecutionDb.h"
#include "../Jobs/JobEventOrchestration.h"
#include "SqliteWorkflowOrchestration.h"

namespace savor::db::execution::workflow {

class SqliteWorkflowOrchestrationQueryService;
class SqliteWorkflowOrchestrationCommandService;
struct WorkflowInvariantRemediationCommand;

class SqliteExecutionDb final : public savor::db::IExecutionDb {
public:
    explicit SqliteExecutionDb(sqlite3* db);
    bool ValidationExecuteSql(std::string_view sql, std::string* error_out = nullptr) const;
    bool ValidationQueryInt(std::string_view sql, std::int64_t* value_out, std::string* error_out = nullptr) const;
    bool ValidationQueryText(std::string_view sql, std::string* value_out, std::string* error_out = nullptr) const;
    bool ValidationExecuteInvariantRemediation(
        const WorkflowInvariantRemediationCommand& command,
        bool* reopened_out,
        std::string* error_out = nullptr);

    IWorkflowOrchestrationQueryService* WorkflowQueryService() override;
    IWorkflowOrchestrationCommandService* WorkflowCommandService() override;
    jobs::IJobEventCommandService* JobCommandService() override;
    bool CreateWorkflowInstance(
        const WorkflowCreateInstanceCommand& command,
        std::int64_t* workflow_instance_id_out = nullptr,
        std::string* error_out = nullptr) override;
    bool CreateJobSet(const CreateJobSetCommand& command, std::int64_t* job_set_id_out = nullptr, std::string* error_out = nullptr) override;
    bool EnqueueJob(const EnqueueJobCommand& command, std::int64_t* job_id_out = nullptr, std::string* error_out = nullptr) override;
    bool EnsureMaterializingJobSet(
        const EnsureMaterializingJobSetCommand& command,
        EnsureMaterializingJobSetReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool CreatePendingJob(
        const CreatePendingJobCommand& command,
        CreatePendingJobReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool SealJobPopulation(
        const SealJobPopulationCommand& command,
        SealJobPopulationReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool PublishWorksetWave(
        const PublishWorksetWaveCommand& command,
        PublishWorksetWaveReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    std::vector<ClaimedPublishedWorkset> ClaimPublishedWorksetBatch(
        const ClaimPublishedWorksetBatchCommand& command,
        std::string* error_out = nullptr) override;
    std::vector<WorksetDispatchLeaseReceipt>
    RenewActiveWorksetLeases(
        const RenewActiveWorksetLeasesCommand& command,
        std::string* error_out = nullptr) override;
    std::optional<ExecutionWorkAvailabilitySnapshot>
    GetExecutionWorkAvailability(
        std::string* error_out = nullptr) const override;
    bool MarkWorksetActive(
        const MarkWorksetActiveCommand& command,
        WorksetDispatchMutationReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool MarkWorksetDraining(
        const MarkWorksetDrainingCommand& command,
        WorksetDispatchMutationReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool ReleaseWorksetDispatch(
        const ReleaseWorksetDispatchCommand& command,
        WorksetDispatchMutationReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool PersistWorkerExecutionEventsBatch(
        const PersistWorkerExecutionEventsBatchCommand& command,
        PersistWorkerExecutionEventsBatchReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    std::vector<ClaimedExecutionFinishedJob>
    ClaimExecutionFinishedJobsBatch(
        const ClaimExecutionFinishedJobsBatchCommand& command,
        std::string* error_out = nullptr) override;
    std::vector<InterruptedResultProcessingJob>
    ListInterruptedResultProcessingJobs(
        std::string* error_out = nullptr) override;
    bool ResetInterruptedResultProcessing(
        const ResetInterruptedResultProcessingCommand& command,
        ResultProcessingReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool RequeueLostResultProcessing(
        const RequeueLostResultProcessingCommand& command,
        ResultProcessingReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool RecordResultProcessingFailure(
        const RecordResultProcessingFailureCommand& command,
        ResultProcessingReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool CommitResultFinalizationsBatch(
        const CommitResultFinalizationsBatchCommand& command,
        std::vector<ResultProcessingReceipt>* receipts_out = nullptr,
        std::string* error_out = nullptr) override;
    std::vector<CommittedJobCancellation>
    ListUnresolvedJobCancellations(
        std::string* error_out = nullptr) override;
    bool MutateJobCancellationsBatch(
        const MutateJobCancellationsBatchCommand& command,
        std::vector<JobCancellationReceipt>* receipts_out = nullptr,
        std::string* error_out = nullptr) override;
    bool IsTempBlobTracked(
        std::string_view relative_path,
        bool* tracked_out,
        std::string* error_out = nullptr) const override;
    std::optional<ClaimedTempBlobCleanup> ClaimNextTempBlobCleanup(
        const ClaimTempBlobCleanupCommand& command,
        std::string* error_out = nullptr) override;
    bool CompleteTempBlobCleanup(
        const CompleteTempBlobCleanupCommand& command,
        ExecutionDbOperationDisposition* disposition_out = nullptr,
        std::string* error_out = nullptr) override;
    bool RecoverInterruptedWorksetDispatches(
        RecoverInterruptedWorksetDispatchesReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<ClaimedExecutionJob> ClaimNextReadyExecutionJob(
        std::string_view claimed_by_token,
        std::int64_t lease_duration_ms,
        std::string* error_out = nullptr) override;
    std::vector<ClaimedExecutionJob> ClaimBatchReadyExecutionJobs(
        std::string_view claimed_by_token,
        int requested_jobs,
        std::int64_t lease_duration_ms,
        std::string* error_out = nullptr) override;
    bool RenewExecutionJobLease(
        std::int64_t job_id,
        std::string_view claimed_by_token,
        std::int64_t lease_duration_ms,
        bool* renewed_out = nullptr,
        std::string* error_out = nullptr) override;
    std::vector<ExecutionJobLeaseRenewalReceipt> RenewExecutionJobLeases(
        const std::vector<ExecutionJobLeaseRequest>& requests,
        std::int64_t lease_duration_ms,
        std::string* error_out = nullptr) override;
    bool MarkExecutionJobStarted(
        std::int64_t job_id,
        std::string_view claimed_by_token,
        std::string_view requested_by,
        ExecutionJobStartReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool ValidateExecutionJobStartAuthoritySet(
        const std::vector<ExecutionJobLeaseRequest>& requests,
        ExecutionJobStartAuthoritySetReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool ConfirmExecutionJobTerminalAuthority(
        std::int64_t job_id,
        std::string_view claimed_by_token,
        std::uint64_t durable_attempt_id,
        std::int64_t lease_duration_ms,
        ExecutionJobTerminalAuthorityReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool RecoverExecutionJobAfterWorkerLoss(
        std::int64_t job_id,
        std::string_view claimed_by_token,
        std::uint64_t durable_attempt_id,
        std::string_view message,
        ExecutionJobWorkerLossRecoveryReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool RequeueExpiredExecutionLeases(
        int* rows_requeued_out = nullptr,
        std::string* error_out = nullptr) override;
    bool RequeueExpiredClaimedExecutionJobs(
        int* rows_requeued_out = nullptr,
        std::string* error_out = nullptr) override;
    bool RequeueClaimedExecutionJob(
        std::int64_t job_id,
        std::string_view claimed_by_token,
        std::string_view message,
        std::string* error_out = nullptr) override;
    bool RequeueInterruptedExecutionJobs(
        int* rows_requeued_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<ExecutionJobRecord> GetJob(std::int64_t job_id) const override;
    std::vector<ExecutionJobEventRecord> ListJobEvents(std::int64_t job_id, int limit = 128) const override;
    bool RecordJobOutput(const RecordExecutionJobOutputCommand& command, std::string* error_out = nullptr) override;
    std::vector<ExecutionJobOutputRecord> ListJobOutputsForWorkflowStep(std::int64_t workflow_step_id) const override;
    std::optional<std::string> GetJobInputIni(std::int64_t job_id, std::string* error_out = nullptr) const override;
    bool RequeueJob(std::int64_t job_id, std::string* error_out = nullptr) override;
    bool RestartFailedJob(std::int64_t job_id, std::optional<std::string> input_ini_override = std::nullopt, std::string* error_out = nullptr) override;
    bool CancelQueuedOrClaimedJob(std::int64_t job_id, std::string* error_out = nullptr) override;
    std::optional<ExecutionJobSetProgressDetails> GetJobSetProgress(std::int64_t job_set_id) const override;
    std::vector<ExecutionJobSetJobRecord> ListJobsInJobSet(
        std::int64_t job_set_id) const override;
    std::vector<ExecutionJobSetJobRecord>
    ListJobsByProgramReference(
        std::int32_t program_kind,
        std::string_view program_ref_kind,
        std::int64_t program_ref_id) const override;
    std::optional<ExecutionJobSetMaterializationRecord>
    GetJobSetByMaterializationKey(
        std::string_view materialization_key) const override;
    std::vector<ExecutionChildJobSetProgressDetails> GetChildJobSetProgress(std::int64_t parent_job_set_id) const override;
    bool MarkQueuedJobsSuperseded(
        std::int64_t job_set_id,
        std::int64_t except_job_id,
        std::string* error_out = nullptr,
        int* rows_superseded_out = nullptr) override;
    retention::OutboxRetentionPreview PreviewOutboxRetention(
        const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
        types::UtcTimePoint now_utc,
        const retention::OutboxRetentionPolicy& policy) const override;
    bool PurgeOutboxThroughRetentionFloor(
        const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
        types::UtcTimePoint now_utc,
        const retention::OutboxRetentionPolicy& policy,
        int max_rows,
        int* rows_deleted_out = nullptr,
        std::string* error_out = nullptr) override;
    bool PurgeWorkflowHandlerDedupeOlderThan(
        std::int64_t last_seen_at_utc_exclusive,
        int max_rows,
        int* rows_deleted_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<events::ExecutionWorkflowJobPayloadView> ResolveExecutionWorkflowJobPayload(
        const events::EventEnvelope& envelope) const override;
    std::optional<events::ExecutionWorkflowJobPayloadView> ResolveExecutionWorkflowJobPayload(
        std::string_view event_type,
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;

private:
    bool PublishWorkset(
        const PublishWorksetCommand& command,
        PublishWorksetReceipt* receipt_out,
        std::string* error_out);
    bool CompleteWorksetPublication(
        const CompleteWorksetPublicationCommand& command,
        CompleteWorksetPublicationReceipt* receipt_out,
        std::string* error_out);
    bool MarkWorksetJobStarted(
        const MarkWorksetJobStartedCommand& command,
        WorksetJobStartReceipt* receipt_out,
        std::string* error_out);
    bool StageWorkerTerminal(
        const StageWorkerTerminalCommand& command,
        StageWorkerTerminalReceipt* receipt_out,
        std::string* error_out);
    std::optional<ClaimedExecutionFinishedJob>
    ClaimNextExecutionFinishedJob(
        std::string* error_out);
    bool CommitResultFinalization(
        const CommitResultFinalizationCommand& command,
        ResultProcessingReceipt* receipt_out,
        std::string* error_out);
    bool ApplyJobCancellationOutcomeInTransaction(
        const JobCancellationOutcomeCommand& command,
        JobCancellationReceipt* receipt_out,
        std::string* error_out);

    sqlite3* db_ = nullptr;
    std::unique_ptr<SqliteWorkflowOrchestrationQueryService> query_service_;
    std::unique_ptr<SqliteWorkflowOrchestrationCommandService> command_service_;
    std::unique_ptr<jobs::SqliteJobEventCommandService> job_command_service_;
};

} // namespace savor::db::execution::workflow
