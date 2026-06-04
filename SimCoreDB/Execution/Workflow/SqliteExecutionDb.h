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

namespace simcore::db::execution::workflow {

class SqliteWorkflowOrchestrationQueryService;
class SqliteWorkflowOrchestrationCommandService;
struct WorkflowInvariantRemediationCommand;
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

class SqliteExecutionDb final : public simcore::db::IExecutionDb {
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
    bool RequeueExpiredExecutionLeases(
        int* rows_requeued_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<ExecutionJobRecord> GetJob(std::int64_t job_id) const override;
    std::optional<ExecutionJobSetProgressDetails> GetJobSetProgress(std::int64_t job_set_id) const;
    std::vector<ExecutionChildJobSetProgressDetails> GetChildJobSetProgress(std::int64_t parent_job_set_id) const;
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
    sqlite3* db_ = nullptr;
    std::unique_ptr<SqliteWorkflowOrchestrationQueryService> query_service_;
    std::unique_ptr<SqliteWorkflowOrchestrationCommandService> command_service_;
    std::unique_ptr<jobs::SqliteJobEventCommandService> job_command_service_;
};

} // namespace simcore::db::execution::workflow
