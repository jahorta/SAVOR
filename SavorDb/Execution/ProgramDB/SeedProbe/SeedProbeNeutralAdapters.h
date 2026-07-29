#pragma once

#include <memory>

#include "../../IExecutionDb.h"
#include "../../../Analysis/IAnalysisDb.h"
#include "../ProgramKindDescriptor.h"
#include "../../../../SavorCore/Phases/Programs/SeedProbe/SeedProbePayload.h"

namespace savor::db::execution::programdb::seedprobe {

class NeutralProbeJobPersistenceAdapter final : public IJobPersistenceAdapter {
public:
    NeutralProbeJobPersistenceAdapter(
        savor::db::IExecutionDb* execution_db,
        savor::seedprobe::SeedProbeTarget target = savor::seedprobe::SeedProbeTarget::PreBattle);

    WorkflowStepScheduleResult EncodeForQueueing(const WorkflowStepScheduleContext& context) const override;
    std::int64_t DecodeDomainRefId(const JobPersistenceRecord& persisted) const override;

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::seedprobe::SeedProbeTarget target_ = savor::seedprobe::SeedProbeTarget::PreBattle;
};

class RequiredSavestateRuntimeInitAdapter final : public IRuntimeInitAdapter {
public:
    RequiredSavestateRuntimeInitAdapter(
        savor::db::IExecutionDb* execution_db,
        savor::db::IAnalysisDb* analysis_db);

    RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const override;
    std::optional<savor::PSJob> MaterializePsJob(std::int64_t job_id, const RuntimeInitRequest& request) const override;

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
};

class NeutralSeedResultMapper final : public IResultMapper {
public:
    NeutralSeedResultMapper(
        savor::db::IExecutionDb* execution_db,
        savor::db::IAnalysisDb* analysis_db);

    std::string BuildResultIniFromPrResult(std::int64_t job_id, const savor::PRResult& result) const override;
    ResultMapPayload MapPrimaryResult(std::int64_t job_id, const std::string& result_ini) const override;
    std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t job_id) const override;

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
};

class NeutralToGridTransitionHandler final : public IWorkflowTransitionHandler {
public:
    WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const override;
};

ProgramKindDescriptor BuildSeedProbeNeutralDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db);

} // namespace savor::db::execution::programdb::seedprobe
