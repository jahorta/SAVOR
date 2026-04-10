#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Execution/IExecutionDb.h"
#include "common/NullJobEventCommandService.h"
#include "common/NullWorkflowQueryService.h"
#include "common/RecordingWorkflowCommandService.h"

class RecordingExecutionDb final : public simcore::db::IExecutionDb {
public:
    simcore::db::execution::workflow::IWorkflowOrchestrationQueryService* WorkflowQueryService() override {
        return &query_service;
    }
    simcore::db::execution::workflow::IWorkflowOrchestrationCommandService* WorkflowCommandService() override {
        return &command_service;
    }
    simcore::db::execution::jobs::IJobEventCommandService* JobCommandService() override {
        return &job_command_service;
    }
    bool CreateJobSet(const simcore::db::CreateJobSetCommand& command, std::int64_t* job_set_id_out = nullptr, std::string* error_out = nullptr) override {
        (void)command;
        const auto job_set_id = ++next_job_set_id_;
        if (job_set_id_out) {
            *job_set_id_out = job_set_id;
        }
        if (error_out) {
            error_out->clear();
        }

        return true;
    }
    bool EnqueueJob(const simcore::db::EnqueueJobCommand& command, std::int64_t* job_id_out = nullptr, std::string* error_out = nullptr) override {
        simcore::db::ExecutionJobRecord record{};
        record.job_id = ++next_job_id_;
        record.job_set_id = command.job_set_id;
        record.program_kind = std::to_string(command.program_kind);
        record.program_version = command.program_version;
        record.program_ref_kind = command.program_ref_kind;
        record.program_ref_id = command.program_ref_id;
        record.savestate_id = command.savestate_id;
        record.fingerprint = command.fingerprint;
        record.state = "QUEUED";
        record.priority = command.priority;
        record.attempts = 0;
        record.max_attempts = command.max_attempts;
        record.queued_at_utc = 0;

        jobs_[record.job_id] = record;

        if (job_id_out) {
            *job_id_out = record.job_id;
        }
        if (error_out) {
            error_out->clear();
        }
        return true;
    }
    std::optional<simcore::db::ExecutionJobRecord> GetJob(std::int64_t job_id) const override {
        const auto it = jobs_.find(job_id);
        if (it == jobs_.end()) {
            return std::nullopt;
        }
        return it->second;
    }
    bool MarkQueuedJobsSuperseded(std::int64_t job_set_id, std::int64_t except_job_id, std::string* error_out = nullptr) override {
        for (auto& [job_id, job] : jobs_) {
            if (job.job_set_id == job_set_id && job.job_id != except_job_id && job.state == "QUEUED") {
                job.state = "SUPERSEDED";
            }
        }
        if (error_out) {
            error_out->clear();
        }
        return true;
    }
    simcore::db::retention::OutboxRetentionPreview PreviewOutboxRetention(
        const std::vector<simcore::db::retention::OutboxSubscriptionSnapshot>&,
        simcore::db::types::UtcTimePoint,
        const simcore::db::retention::OutboxRetentionPolicy&) const override {
        return {};
    }
    bool PurgeOutboxThroughRetentionFloor(
        const std::vector<simcore::db::retention::OutboxSubscriptionSnapshot>&,
        simcore::db::types::UtcTimePoint,
        const simcore::db::retention::OutboxRetentionPolicy&,
        int,
        int* rows_deleted_out = nullptr,
        std::string* error_out = nullptr) override {
        if (rows_deleted_out) {
            *rows_deleted_out = 0;
        }
        if (error_out) {
            error_out->clear();
        }
        return true;
    }
    bool PurgeWorkflowHandlerDedupeOlderThan(
        std::int64_t,
        int,
        int* rows_deleted_out = nullptr,
        std::string* error_out = nullptr) override {
        if (rows_deleted_out) {
            *rows_deleted_out = 0;
        }
        if (error_out) {
            error_out->clear();
        }
        return true;
    }
    std::optional<simcore::db::events::ExecutionWorkflowJobPayloadView> ResolveExecutionWorkflowJobPayload(
        const simcore::db::events::EventEnvelope&) const override {
        return std::nullopt;
    }
    std::optional<simcore::db::events::ExecutionWorkflowJobPayloadView> ResolveExecutionWorkflowJobPayload(
        std::string_view,
        int,
        std::string_view,
        std::int64_t) const override {
        return std::nullopt;
    }

    NullWorkflowQueryService query_service;
    RecordingWorkflowCommandService command_service;
    NullJobEventCommandService job_command_service;

private:
    std::int64_t next_job_set_id_ = 1000;
    std::int64_t next_job_id_ = 5000;
    std::unordered_map<std::int64_t, simcore::db::ExecutionJobRecord> jobs_;
};
