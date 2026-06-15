#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#include "../Common/QueuedDb.h"
#include "IExecutionDb.h"
#include "Jobs/JobEventOrchestration.h"
#include "Workflow/WorkflowOrchestration.h"

namespace savor::db::execution {

struct ExecutionQueueConfig {
    std::size_t write_capacity = 4096;
    std::size_t read_capacity = 4096;
};

struct ExecutionQueueTelemetrySnapshot {
    core::QueuedDbTelemetrySnapshot queued;
    std::size_t write_depth = 0;
    std::size_t read_depth = 0;
    std::size_t write_capacity = 0;
    std::size_t read_capacity = 0;
    std::size_t write_high_water_depth = 0;
    std::size_t read_high_water_depth = 0;
    std::uint64_t write_oldest_queued_age_ms = 0;
    std::uint64_t read_oldest_queued_age_ms = 0;
    std::uint64_t write_enqueued = 0;
    std::uint64_t read_enqueued = 0;
    std::uint64_t write_rejected = 0;
    std::uint64_t read_rejected = 0;
    std::uint64_t write_completed = 0;
    std::uint64_t read_completed = 0;
    std::uint64_t write_failed = 0;
    std::uint64_t read_failed = 0;
    std::uint64_t sqlite_busy = 0;
    std::uint64_t sqlite_locked = 0;
};

class QueuedExecutionDb final : public savor::db::IExecutionDb, private savor::db::core::QueuedDbExecutor {
public:
    explicit QueuedExecutionDb(
        savor::db::IExecutionDb* inner,
        ExecutionQueueConfig config = {});
    ~QueuedExecutionDb() override;

    QueuedExecutionDb(const QueuedExecutionDb&) = delete;
    QueuedExecutionDb& operator=(const QueuedExecutionDb&) = delete;

    bool Start(std::string* error_out = nullptr);
    void Stop();
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] ExecutionQueueTelemetrySnapshot GetTelemetrySnapshot() const;

    workflow::IWorkflowOrchestrationQueryService* WorkflowQueryService() override;
    workflow::IWorkflowOrchestrationCommandService* WorkflowCommandService() override;
    jobs::IJobEventCommandService* JobCommandService() override;

    bool CreateWorkflowInstance(
        const workflow::WorkflowCreateInstanceCommand& command,
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
    class QueuedWorkflowQueryService;
    class QueuedWorkflowCommandService;
    class QueuedJobCommandService;

    template <typename Result, typename Fn>
    Result ExecuteRead(
        Fn&& fn,
        Result fallback,
        std::string* error_out = nullptr,
        const std::source_location& location = std::source_location::current()) const;

    template <typename Result, typename Fn>
    Result ExecuteWrite(
        Fn&& fn,
        Result fallback,
        std::string* error_out = nullptr,
        const std::source_location& location = std::source_location::current()) const;

    savor::db::IExecutionDb* inner_ = nullptr;
    ExecutionQueueConfig config_{};

    mutable std::mutex sqlite_call_mtx_;
    std::unique_ptr<core::QueuedDbLane> read_lane_;
    std::unique_ptr<core::QueuedDbLane> write_lane_;
    std::unique_ptr<QueuedWorkflowQueryService> workflow_query_service_;
    std::unique_ptr<QueuedWorkflowCommandService> workflow_command_service_;
    std::unique_ptr<QueuedJobCommandService> job_command_service_;
};

} // namespace savor::db::execution
