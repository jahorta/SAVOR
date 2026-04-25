#include "DBWorkflowCoordinatorFactory.h"

#include <stdexcept>

namespace simcore::runner::parallel::simcoredb {

DBWorkflowWorkerCoordinator BuildDbBackedWorkflowCoordinator(
    simcore::db::IExecutionDb* execution_db,
    DBWorkflowWorkerCoordinatorConfig worker_cfg,
    CoordinatorIntegrationConfig integration_cfg,
    const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    DBWorkflowWorkerCoordinator::ReadyStepPersistFn persist_materialization_fn) {
    if (execution_db == nullptr) {
        throw std::invalid_argument("BuildDbBackedWorkflowCoordinator requires a non-null execution_db");
    }
    if (program_kind_registry == nullptr) {
        throw std::invalid_argument("BuildDbBackedWorkflowCoordinator requires a non-null program_kind_registry");
    }
    if (execution_db->WorkflowQueryService() == nullptr || execution_db->WorkflowCommandService() == nullptr) {
        throw std::invalid_argument("workflow mode requires workflow query/command services");
    }

    return DBWorkflowWorkerCoordinator(
        execution_db,
        std::move(worker_cfg),
        integration_cfg,
        program_kind_registry,
        std::move(persist_materialization_fn));
}

} // namespace simcore::runner::parallel::simcoredb
