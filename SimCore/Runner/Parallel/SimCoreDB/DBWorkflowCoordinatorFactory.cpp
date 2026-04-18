#include "DBWorkflowCoordinatorFactory.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>

#include "../../../../SimCoreDB/Execution/Workflow/AdapterChainOrchestrator.h"
#include "../../../../SimCoreDB/Execution/Workflow/WorkflowOrchestration.h"

namespace simcore::runner::parallel::simcoredb {
namespace {

std::int64_t UtcNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

struct CoordinatedJobState {
    mutable std::mutex mutex;
    std::deque<ClaimedJobSeed> claimable_jobs;
};

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

    auto shared_state = std::make_shared<CoordinatedJobState>();

    auto adapter_chain_orchestrator = std::make_shared<simcore::db::execution::workflow::AdapterChainOrchestrator>(
        program_kind_registry,
        nullptr);

    auto schedule_fn = [execution_db, program_kind_registry, shared_state, adapter_chain_orchestrator](const WorkflowReadyStep& step) -> ScheduledJobSet {
        if (execution_db == nullptr || program_kind_registry == nullptr) {
            return {};
        }

        const auto* descriptor = program_kind_registry->FindForStepKind(step.step_kind);
        if (descriptor == nullptr || descriptor->job_persistence == nullptr || adapter_chain_orchestrator == nullptr) {
            return {};
        }
        const auto persisted = adapter_chain_orchestrator->OnInputComplete(step.step_kind, step.workflow_step_id);
        if (!persisted.has_value()) {
            return {};
        }

        simcore::db::CreateJobSetCommand create_set{};
        create_set.program_kind = descriptor->program_kind;
        create_set.purpose = "workflow";
        create_set.created_by = std::string("DBWorkflowCoordinatorFactory");
        create_set.created_at_utc = UtcNowMs();
        create_set.expected_total = 1;
        if (!persisted->program_ref_kind.empty()) {
            create_set.domain_ref_kind = persisted->program_ref_kind;
        }
        if (persisted->program_ref_id > 0) {
            create_set.domain_ref_id = persisted->program_ref_id;
        }
        create_set.meta_note = "workflow_step_id=" + std::to_string(step.workflow_step_id);

        std::int64_t job_set_id = 0;
        std::string error;
        if (!execution_db->CreateJobSet(create_set, &job_set_id, &error) || job_set_id <= 0) {
            return {};
        }

        simcore::db::EnqueueJobCommand enqueue{};
        enqueue.job_set_id = job_set_id;
        enqueue.program_kind = descriptor->program_kind;
        enqueue.program_version = persisted->program_version;
        enqueue.program_ref_kind = persisted->program_ref_kind;
        enqueue.program_ref_id = persisted->program_ref_id;
        enqueue.fingerprint = persisted->fingerprint.empty() ? (step.step_kind + ":" + std::to_string(step.workflow_step_id)) : persisted->fingerprint;
        enqueue.priority = step.priority;
        enqueue.max_attempts = 1;

        std::int64_t job_id = 0;
        if (!execution_db->EnqueueJob(enqueue, &job_id, &error) || job_id <= 0) {
            return {};
        }

        {
            std::lock_guard<std::mutex> lock(shared_state->mutex);
            shared_state->claimable_jobs.push_back(ClaimedJobSeed{
                .step = step,
                .job_set_id = job_set_id,
                .job_id = job_id,
            });
        }

        return ScheduledJobSet{ .job_set_id = job_set_id, .workflow_step_id = step.workflow_step_id };
    };

    auto claim_jobs_fn = [shared_state]() {
        std::vector<ClaimedJobSeed> claims;
        std::lock_guard<std::mutex> lock(shared_state->mutex);
        claims.reserve(shared_state->claimable_jobs.size());
        while (!shared_state->claimable_jobs.empty()) {
            claims.push_back(shared_state->claimable_jobs.front());
            shared_state->claimable_jobs.pop_front();
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
