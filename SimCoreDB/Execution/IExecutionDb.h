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

namespace simcore::db::execution::workflow {
struct IWorkflowOrchestrationCommandService;
struct IWorkflowOrchestrationQueryService;
struct WorkflowCreateInstanceCommand;
}

namespace simcore::db::execution::jobs {
struct IJobEventCommandService;
}

namespace simcore::db {

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
};

struct ExecutionJobRecord {
    std::int64_t job_id = 0;
    std::int64_t job_set_id = 0;
    std::string program_kind;
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
    std::optional<std::string> previous_claimed_by_token;
    std::optional<std::int64_t> previous_lease_expires_at_utc;
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
    virtual std::optional<ExecutionJobRecord> GetJob(std::int64_t job_id) const = 0;
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

} // namespace simcore::db
