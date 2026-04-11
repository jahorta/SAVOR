#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "Execution/IExecutionDb.h"
#include "common/RecordingJobEventCommandService.h"

class MapperExecutionDb final : public simcore::db::IExecutionDb {
public:
    simcore::db::execution::workflow::IWorkflowOrchestrationQueryService* WorkflowQueryService() override { return nullptr; }
    simcore::db::execution::workflow::IWorkflowOrchestrationCommandService* WorkflowCommandService() override { return nullptr; }
    simcore::db::execution::jobs::IJobEventCommandService* JobCommandService() override { return &job_events; }
    bool CreateWorkflowInstance(const simcore::db::execution::workflow::WorkflowCreateInstanceCommand&, std::int64_t*, std::string*) override {
        return false;
    }
    bool CreateJobSet(const simcore::db::CreateJobSetCommand&, std::int64_t*, std::string*) override { return false; }
    bool EnqueueJob(const simcore::db::EnqueueJobCommand&, std::int64_t*, std::string*) override { return false; }
    std::optional<simcore::db::ExecutionJobRecord> GetJob(std::int64_t) const override { return std::nullopt; }
    bool MarkQueuedJobsSuperseded(std::int64_t, std::int64_t, std::string*) override { return false; }
    simcore::db::retention::OutboxRetentionPreview PreviewOutboxRetention(
        const std::vector<simcore::db::retention::OutboxSubscriptionSnapshot>&,
        simcore::db::types::UtcTimePoint,
        const simcore::db::retention::OutboxRetentionPolicy&) const override { return {}; }
    bool PurgeOutboxThroughRetentionFloor(
        const std::vector<simcore::db::retention::OutboxSubscriptionSnapshot>&,
        simcore::db::types::UtcTimePoint,
        const simcore::db::retention::OutboxRetentionPolicy&,
        int,
        int*,
        std::string*) override { return false; }
    bool PurgeWorkflowHandlerDedupeOlderThan(std::int64_t, int, int*, std::string*) override { return false; }
    std::optional<simcore::db::events::ExecutionWorkflowJobPayloadView> ResolveExecutionWorkflowJobPayload(
        const simcore::db::events::EventEnvelope&) const override { return std::nullopt; }
    std::optional<simcore::db::events::ExecutionWorkflowJobPayloadView> ResolveExecutionWorkflowJobPayload(
        std::string_view,
        int,
        std::string_view,
        std::int64_t) const override { return std::nullopt; }

    RecordingJobEventCommandService job_events;
};
