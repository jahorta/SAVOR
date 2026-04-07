#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "../ProgramKindDescriptor.h"
#include "SeedProbeContracts.h"
#include "SeedProbeGridAdapters.h"
#include "../../../Analysis/IAnalysisDb.h"
#include "../../IExecutionDb.h"

namespace simcore::db::execution::programdb::seedprobe {

class SeedProbeUniqueTransitionHandler final : public IWorkflowTransitionHandler {
public:
    using CompletionGateFn = std::function<bool(const WorkflowTransitionContext& context)>;

    explicit SeedProbeUniqueTransitionHandler(CompletionGateFn completion_gate);
    WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const override;

private:
    CompletionGateFn completion_gate_{};
};

class SeedProbeUniqueJobPersistenceAdapter final : public IJobPersistenceAdapter {
public:
    SeedProbeUniqueJobPersistenceAdapter(
        simcore::db::IExecutionDb* execution_db,
        simcore::db::IAnalysisDb* analysis_db,
        SeedProbeGridBlueprintConfig blueprint,
        UniqueIni unique_ini);

    JobPersistenceRecord EncodeForQueueing(std::int64_t domain_ref_id) const override;
    std::int64_t DecodeDomainRefId(const JobPersistenceRecord& persisted) const override;

private:
    simcore::db::IExecutionDb* execution_db_ = nullptr;
    simcore::db::IAnalysisDb* analysis_db_ = nullptr;
    SeedProbeGridBlueprintConfig blueprint_{};
    UniqueIni unique_ini_{};
};

class SeedProbeUniqueRuntimeInitAdapter final : public IRuntimeInitAdapter {
public:
    SeedProbeUniqueRuntimeInitAdapter(simcore::db::IExecutionDb* execution_db, const simcore::db::IAnalysisDb* analysis_db);
    RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const override;

private:
    simcore::db::IExecutionDb* execution_db_ = nullptr;
    const simcore::db::IAnalysisDb* analysis_db_ = nullptr;
};

class SeedProbeUniqueResultMapper final : public IResultMapper {
public:
    SeedProbeUniqueResultMapper(simcore::db::IExecutionDb* execution_db, simcore::db::IAnalysisDb* analysis_db);
    ResultMapPayload MapPrimaryResult(std::int64_t job_id, const std::string& result_ini) const override;
    std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t job_id) const override;

private:
    simcore::db::IExecutionDb* execution_db_ = nullptr;
    simcore::db::IAnalysisDb* analysis_db_ = nullptr;
};

} // namespace simcore::db::execution::programdb::seedprobe
