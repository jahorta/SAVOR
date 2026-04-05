#include "ProgramKindLegacyCodecShim.h"

namespace simcore::db::execution::programdb {

namespace {

class LegacyJobPersistenceAdapter final : public IJobPersistenceAdapter {
public:
    explicit LegacyJobPersistenceAdapter(std::shared_ptr<ILegacyProgramCodec> legacy)
        : legacy_(std::move(legacy)) {}

    JobPersistenceRecord EncodeForQueueing(std::int64_t domain_ref_id) const override {
        return legacy_->EncodeForQueueing(domain_ref_id);
    }

    std::int64_t DecodeDomainRefId(const JobPersistenceRecord& persisted) const override {
        return legacy_->DecodeDomainRefId(persisted);
    }

private:
    std::shared_ptr<ILegacyProgramCodec> legacy_;
};

class LegacyRuntimeInitAdapter final : public IRuntimeInitAdapter {
public:
    explicit LegacyRuntimeInitAdapter(std::shared_ptr<ILegacyProgramCodec> legacy)
        : legacy_(std::move(legacy)) {}

    RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const override {
        return legacy_->BuildRuntimeInit(job_id);
    }

private:
    std::shared_ptr<ILegacyProgramCodec> legacy_;
};

class LegacyResultMapper final : public IResultMapper {
public:
    explicit LegacyResultMapper(std::shared_ptr<ILegacyProgramCodec> legacy)
        : legacy_(std::move(legacy)) {}

    ResultMapPayload MapPrimaryResult(std::int64_t job_id) const override {
        return legacy_->MapPrimaryResult(job_id);
    }

    std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t job_id) const override {
        return legacy_->MapPrimaryArtifact(job_id);
    }

private:
    std::shared_ptr<ILegacyProgramCodec> legacy_;
};

class LegacyWorkflowTransitionHandler final : public IWorkflowTransitionHandler {
public:
    explicit LegacyWorkflowTransitionHandler(std::shared_ptr<ILegacyProgramCodec> legacy)
        : legacy_(std::move(legacy)) {}

    WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const override {
        return legacy_->EvaluateTransition(context);
    }

private:
    std::shared_ptr<ILegacyProgramCodec> legacy_;
};

} // namespace

ProgramKindDescriptor BuildDescriptorFromLegacyCodec(
    std::int32_t program_kind,
    std::string program_name,
    const std::shared_ptr<ILegacyProgramCodec>& legacy_codec,
    bool supports_workflow_orchestration,
    bool supports_legacy_trigger_bridge) {
    ProgramKindDescriptor descriptor;
    descriptor.program_kind = program_kind;
    descriptor.program_name = std::move(program_name);
    descriptor.job_persistence = std::make_shared<LegacyJobPersistenceAdapter>(legacy_codec);
    descriptor.runtime_init = std::make_shared<LegacyRuntimeInitAdapter>(legacy_codec);
    descriptor.result_mapper = std::make_shared<LegacyResultMapper>(legacy_codec);
    descriptor.workflow_transition = std::make_shared<LegacyWorkflowTransitionHandler>(legacy_codec);
    descriptor.supports_workflow_orchestration = supports_workflow_orchestration;
    descriptor.supports_legacy_trigger_bridge = supports_legacy_trigger_bridge;
    return descriptor;
}

} // namespace simcore::db::execution::programdb
