#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "../../../SimCore/Runner/Script/PhaseScriptVM.h"

namespace simcore {
struct PRResult;
}

namespace simcore::db::execution::programdb {

struct JobPersistenceRecord {
    std::string program_ref_kind;
    std::int64_t program_ref_id = 0;
    std::string fingerprint;
    std::int32_t program_version = 1;
};

struct WorkflowStepScheduleResult {
    JobPersistenceRecord persistence;
    std::int64_t root_job_set_id = 0;
};

struct RuntimeInitRequest {
    std::string savestate_ref_kind;
    std::int64_t savestate_ref_id = 0;
    std::string bootstrap_profile;
};

struct ResultArtifactRef {
    std::string artifact_role;
    std::int64_t artifact_id = 0;
};

struct ResultMapPayload {
    std::string result_kind;
    std::int64_t result_ref_id = 0;
};

struct IResultPayloadWriter {
    virtual ~IResultPayloadWriter() = default;
    virtual bool Persist(const ResultMapPayload& payload, std::string* error_out) = 0;
};

struct WorkflowTransitionContext {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t job_set_id = 0;
    std::string workflow_kind;
    std::string step_key;
};

struct WorkflowTransitionDecision {
    bool should_advance = false;
    std::optional<std::string> blocked_reason;
    std::optional<std::string> next_step_key;
};

struct IJobPersistenceAdapter {
    virtual ~IJobPersistenceAdapter() = default;
    virtual WorkflowStepScheduleResult EncodeForQueueing(std::int64_t domain_ref_id) const = 0;
    virtual std::int64_t DecodeDomainRefId(const JobPersistenceRecord& persisted) const = 0;
};

struct IRuntimeInitAdapter {
    virtual ~IRuntimeInitAdapter() = default;
    virtual RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const = 0;
    virtual std::optional<simcore::PSJob> MaterializePsJob(std::int64_t job_id, const RuntimeInitRequest& request) const = 0;
};

struct IResultMapper {
    virtual ~IResultMapper() = default;
    virtual std::string BuildResultIniFromPrResult(std::int64_t job_id, const simcore::PRResult& result) const = 0;
    virtual ResultMapPayload MapPrimaryResult(std::int64_t job_id, const std::string& result_ini) const = 0;
    virtual std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t job_id) const = 0;
};

struct IWorkflowTransitionHandler {
    virtual ~IWorkflowTransitionHandler() = default;
    virtual WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const = 0;
};

struct ProgramKindDescriptor {
    std::int32_t program_kind = 0;
    std::string program_name;

    std::shared_ptr<IJobPersistenceAdapter> job_persistence;
    std::shared_ptr<IRuntimeInitAdapter> runtime_init;
    std::shared_ptr<IResultMapper> result_mapper;
    std::shared_ptr<IResultPayloadWriter> result_payload_writer;
    std::shared_ptr<IWorkflowTransitionHandler> workflow_transition;

    bool supports_workflow_orchestration = false;
};

} // namespace simcore::db::execution::programdb
