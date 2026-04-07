#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include <sqlite3.h>

#include "../IExecutionDb.h"
#include "../Jobs/JobEventOrchestration.h"
#include "SqliteWorkflowOrchestration.h"

namespace simcore::db::execution::workflow {

class SqliteWorkflowOrchestrationQueryService;
class SqliteWorkflowOrchestrationCommandService;

class ExecutionDb final : public simcore::db::IExecutionDb {
public:
    explicit ExecutionDb(sqlite3* db);

    IWorkflowOrchestrationQueryService* WorkflowQueryService() override;
    IWorkflowOrchestrationCommandService* WorkflowCommandService() override;
    jobs::IJobEventCommandService* JobCommandService() override;
    bool CreateJobSet(const CreateJobSetCommand& command, std::int64_t* job_set_id_out = nullptr, std::string* error_out = nullptr) override;
    bool EnqueueJob(const EnqueueJobCommand& command, std::int64_t* job_id_out = nullptr, std::string* error_out = nullptr) override;
    std::optional<ExecutionJobRecord> GetJob(std::int64_t job_id) const override;
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
