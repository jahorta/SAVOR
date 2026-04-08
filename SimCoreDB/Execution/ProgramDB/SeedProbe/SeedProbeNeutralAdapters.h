#pragma once

#include <memory>

#include "../../IExecutionDb.h"
#include "../../../../Analysis/IAnalysisDb.h"
#include "../ProgramKindDescriptor.h"

namespace simcore::db::execution::programdb::seedprobe {

class NeutralProbeJobPersistenceAdapter final : public IJobPersistenceAdapter {
public:
    NeutralProbeJobPersistenceAdapter(
        simcore::db::IExecutionDb* execution_db,
        simcore::db::IAnalysisDb* analysis_db);

    JobPersistenceRecord EncodeForQueueing(std::int64_t domain_ref_id) const override;
    std::int64_t DecodeDomainRefId(const JobPersistenceRecord& persisted) const override;

private:
    simcore::db::IExecutionDb* execution_db_ = nullptr;
    simcore::db::IAnalysisDb* analysis_db_ = nullptr;
};

class RequiredSavestateRuntimeInitAdapter final : public IRuntimeInitAdapter {
public:
    RequiredSavestateRuntimeInitAdapter(
        simcore::db::IExecutionDb* execution_db,
        simcore::db::IAnalysisDb* analysis_db);

    RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const override;

private:
    simcore::db::IExecutionDb* execution_db_ = nullptr;
    simcore::db::IAnalysisDb* analysis_db_ = nullptr;
};

class NeutralSeedResultMapper final : public IResultMapper {
public:
    NeutralSeedResultMapper(
        simcore::db::IExecutionDb* execution_db,
        simcore::db::IAnalysisDb* analysis_db);

    std::string BuildResultIniFromPrResult(std::int64_t job_id, const simcore::PRResult& result) const override;
    ResultMapPayload MapPrimaryResult(std::int64_t job_id, const std::string& result_ini) const override;
    std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t job_id) const override;

private:
    simcore::db::IExecutionDb* execution_db_ = nullptr;
    simcore::db::IAnalysisDb* analysis_db_ = nullptr;
};

class NeutralToGridTransitionHandler final : public IWorkflowTransitionHandler {
public:
    WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const override;
};

ProgramKindDescriptor BuildSeedProbeNeutralDescriptor(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IAnalysisDb* analysis_db);

} // namespace simcore::db::execution::programdb::seedprobe
