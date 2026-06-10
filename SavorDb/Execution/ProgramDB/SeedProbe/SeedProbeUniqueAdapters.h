#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "../ProgramKindDescriptor.h"
#include "SeedProbeContracts.h"
#include "SeedProbeGridAdapters.h"
#include "../../../Authoring/IAuthoringDb.h"
#include "../../../Analysis/IAnalysisDb.h"
#include "../../IExecutionDb.h"

namespace savor::db::execution::programdb::seedprobe {

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
        savor::db::IExecutionDb* execution_db,
        savor::db::IAnalysisDb* analysis_db,
        savor::db::IAuthoringDb* authoring_db,
        SeedProbeGridBlueprintConfig blueprint,
        UniqueIni unique_ini);

    WorkflowStepScheduleResult EncodeForQueueing(std::int64_t domain_ref_id) const override;
    std::int64_t DecodeDomainRefId(const JobPersistenceRecord& persisted) const override;

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    savor::db::IAuthoringDb* authoring_db_ = nullptr;
    SeedProbeGridBlueprintConfig blueprint_{};
    UniqueIni unique_ini_{};

    SeedProbeGridBlueprintConfig ResolveBlueprintForRun(std::int64_t probe_run_id) const;
    UniqueIni ResolveUniqueSpecForRun(std::int64_t probe_run_id) const;
};

class SeedProbeUniqueRuntimeInitAdapter final : public IRuntimeInitAdapter {
public:
    SeedProbeUniqueRuntimeInitAdapter(savor::db::IExecutionDb* execution_db, const savor::db::IAnalysisDb* analysis_db);
    RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const override;
    std::optional<savor::PSJob> MaterializePsJob(std::int64_t job_id, const RuntimeInitRequest& request) const override;

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    const savor::db::IAnalysisDb* analysis_db_ = nullptr;
};

class SeedProbeUniqueResultMapper final : public IResultMapper {
public:
    SeedProbeUniqueResultMapper(savor::db::IExecutionDb* execution_db, savor::db::IAnalysisDb* analysis_db);
    std::string BuildResultIniFromPrResult(std::int64_t job_id, const savor::PRResult& result) const override;
    ResultMapPayload MapPrimaryResult(std::int64_t job_id, const std::string& result_ini) const override;
    std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t job_id) const override;

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
};

ProgramKindDescriptor BuildSeedProbeUniqueDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    SeedProbeGridBlueprintConfig blueprint,
    UniqueIni unique_ini,
    SeedProbeUniqueTransitionHandler::CompletionGateFn completion_gate = {},
    savor::db::IAuthoringDb* authoring_db = nullptr);

} // namespace savor::db::execution::programdb::seedprobe
