#include "DBWorkflowCoordinatorFactory.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>

#include "../../../../SimCoreDB/Execution/Workflow/AdapterChainOrchestrator.h"
#include "../../../../SimCoreDB/Execution/Workflow/WorkflowOrchestration.h"

namespace simcore::runner::parallel::simcoredb {
namespace {

std::optional<simcore::PSJob> MaterializePsJob(
    const simcore::db::execution::programdb::ProgramKindDescriptor* descriptor,
    std::int64_t job_id) {
    if (descriptor == nullptr || descriptor->runtime_init == nullptr) {
        return std::nullopt;
    }
    const auto init_request = descriptor->runtime_init->BuildRuntimeInit(job_id);
    return descriptor->runtime_init->MaterializePsJob(job_id, init_request);
}

} // namespace

DBWorkflowWorkerCoordinator BuildDbBackedWorkflowCoordinator(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::execution::workflow::IWorkflowModeProvider* mode_provider,
    DBWorkflowWorkerCoordinatorConfig worker_cfg,
    CoordinatorIntegrationConfig integration_cfg,
    const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    DBWorkflowWorkerCoordinator::ReadyStepPersistFn persist_materialization_fn) {
    if (mode_provider == nullptr) {
        throw std::invalid_argument("BuildDbBackedWorkflowCoordinator requires a non-null mode provider");
    }

    const auto mode = mode_provider->GetModeSelection().mode;
    if (mode == simcore::db::execution::workflow::WorkflowExecutionMode::Workflow) {
        if (execution_db == nullptr) {
            throw std::invalid_argument("workflow mode requires a non-null execution_db");
        }
        if (program_kind_registry == nullptr) {
            throw std::invalid_argument("workflow mode requires a non-null program_kind_registry");
        }
        if (execution_db->WorkflowQueryService() == nullptr || execution_db->WorkflowCommandService() == nullptr) {
            throw std::invalid_argument("workflow mode requires workflow query/command services");
        }
    }

    auto adapter_chain_orchestrator = std::make_shared<simcore::db::execution::workflow::AdapterChainOrchestrator>(
        program_kind_registry,
        nullptr);

    auto schedule_fn = [execution_db, program_kind_registry, adapter_chain_orchestrator](const WorkflowReadyStep& step) -> ScheduledJobSet {
        if (execution_db == nullptr || program_kind_registry == nullptr) {
            return {};
        }

        const auto* descriptor = program_kind_registry->FindForStepKind(step.step_kind);
        if (descriptor == nullptr || descriptor->job_persistence == nullptr || adapter_chain_orchestrator == nullptr) {
            return {};
        }
        if (!step.input_ref_id.has_value()) {
            return {};
        }
        const auto domain_ref_id = *step.input_ref_id;
        const auto persisted = adapter_chain_orchestrator->OnInputComplete(step.step_kind, domain_ref_id);
        if (!persisted.has_value()) {
            return {};
        }
        if (persisted->root_job_set_id <= 0) {
            return {};
        }
        return ScheduledJobSet{ .job_set_id = persisted->root_job_set_id, .workflow_step_id = step.workflow_step_id };
    };

    auto claim_jobs_fn = [execution_db](std::size_t max_claims) {
        std::vector<ClaimedJobSeed> claims;
        if (execution_db == nullptr || max_claims == 0) {
            return claims;
        }
        std::string error;
        const auto claimed_jobs = execution_db->ClaimBatchReadyExecutionJobs(
            "workflow_job_materializer",
            static_cast<int>(max_claims),
            30000,
            &error);
        claims.reserve(claimed_jobs.size());
        for (const auto& claimed : claimed_jobs) {
            claims.push_back(ClaimedJobSeed{
                .step = WorkflowReadyStep{
                    .workflow_instance_id = claimed.workflow_instance_id,
                    .workflow_step_id = claimed.workflow_step_id,
                    .step_key = claimed.workflow_step_key,
                    .step_kind = claimed.workflow_step_kind,
                    .priority = claimed.workflow_step_priority,
                },
                .job_set_id = claimed.job_set_id,
                .job_id = claimed.job_id,
                .affinity = ClaimedJobAffinity{
                    .savestate_affinity_key = claimed.savestate_affinity_key,
                    .program_runtime_affinity_key = claimed.program_runtime_affinity_key,
                },
            });
        }
        return claims;
    };

    auto build_job_payload_fn = [execution_db, program_kind_registry](std::int64_t job_id, const WorkflowReadyStep& step) -> std::optional<simcore::PSJob> {
        if (execution_db == nullptr || program_kind_registry == nullptr) {
            return std::nullopt;
        }

        const auto job_record = execution_db->GetJob(job_id);
        if (!job_record.has_value()) {
            return std::nullopt;
        }

        const auto* descriptor = program_kind_registry->FindForStepKind(step.step_kind);
        if (descriptor == nullptr) {
            return std::nullopt;
        }

        return MaterializePsJob(descriptor, job_id);
    };

    return DBWorkflowWorkerCoordinator(
        execution_db,
        mode_provider,
        std::move(worker_cfg),
        integration_cfg,
        std::move(schedule_fn),
        std::move(claim_jobs_fn),
        std::move(build_job_payload_fn),
        std::move(persist_materialization_fn),
        program_kind_registry);
}

} // namespace simcore::runner::parallel::simcoredb
