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
}

namespace simcore::db::execution::jobs {
struct IJobEventCommandService;
}

namespace simcore::db {

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
};

struct CreateJobSetCommand {
    std::optional<std::int64_t> parent_job_set_id;
    std::int32_t program_kind = 0;
    std::string purpose;
    std::string created_by;
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
    std::int32_t program_version = 0;
    std::string program_ref_kind;
    std::int64_t program_ref_id = 0;
    std::optional<std::int64_t> savestate_id;
    std::string fingerprint;
    int priority = 0;
    int attempts = 0;
    int max_attempts = 1;
    std::int64_t queued_at_utc = 0;
};

struct IExecutionDb {
    virtual ~IExecutionDb() = default;

    virtual execution::workflow::IWorkflowOrchestrationQueryService* WorkflowQueryService() = 0;
    virtual execution::workflow::IWorkflowOrchestrationCommandService* WorkflowCommandService() = 0;
    virtual execution::jobs::IJobEventCommandService* JobCommandService() = 0;
    virtual std::optional<ExecutionJobRecord> GetJobRecord(std::int64_t job_id) const = 0;
    virtual bool CreateJobSet(
        const CreateJobSetCommand& command,
        std::int64_t* job_set_id_out = nullptr,
        std::string* error_out = nullptr) = 0;
    virtual bool EnqueueJob(
        const EnqueueJobCommand& command,
        std::int64_t* job_id_out = nullptr,
        std::string* error_out = nullptr) = 0;


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
