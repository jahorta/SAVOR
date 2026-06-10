#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "Execution/IExecutionDb.h"
#include "common/RecordingJobEventCommandService.h"

class MapperExecutionDb final : public savor::db::IExecutionDb {
public:
    savor::db::execution::workflow::IWorkflowOrchestrationQueryService* WorkflowQueryService() override { return nullptr; }
    savor::db::execution::workflow::IWorkflowOrchestrationCommandService* WorkflowCommandService() override { return nullptr; }
    savor::db::execution::jobs::IJobEventCommandService* JobCommandService() override { return &job_events; }
    bool CreateWorkflowInstance(const savor::db::execution::workflow::WorkflowCreateInstanceCommand&, std::int64_t*, std::string*) override {
        return false;
    }
    bool CreateJobSet(const savor::db::CreateJobSetCommand&, std::int64_t*, std::string*) override { return false; }
    bool EnqueueJob(const savor::db::EnqueueJobCommand&, std::int64_t*, std::string*) override { return false; }
    std::optional<savor::db::ExecutionJobRecord> GetJob(std::int64_t) const override { return std::nullopt; }
    bool MarkQueuedJobsSuperseded(std::int64_t, std::int64_t, std::string*, int*) override { return false; }
    savor::db::retention::OutboxRetentionPreview PreviewOutboxRetention(
        const std::vector<savor::db::retention::OutboxSubscriptionSnapshot>&,
        savor::db::types::UtcTimePoint,
        const savor::db::retention::OutboxRetentionPolicy&) const override { return {}; }
    bool PurgeOutboxThroughRetentionFloor(
        const std::vector<savor::db::retention::OutboxSubscriptionSnapshot>&,
        savor::db::types::UtcTimePoint,
        const savor::db::retention::OutboxRetentionPolicy&,
        int,
        int*,
        std::string*) override { return false; }
    bool PurgeWorkflowHandlerDedupeOlderThan(std::int64_t, int, int*, std::string*) override { return false; }
    std::optional<savor::db::events::ExecutionWorkflowJobPayloadView> ResolveExecutionWorkflowJobPayload(
        const savor::db::events::EventEnvelope&) const override { return std::nullopt; }
    std::optional<savor::db::events::ExecutionWorkflowJobPayloadView> ResolveExecutionWorkflowJobPayload(
        std::string_view,
        int,
        std::string_view,
        std::int64_t) const override { return std::nullopt; }

    RecordingJobEventCommandService job_events;
};
