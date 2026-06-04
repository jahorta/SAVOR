#pragma once

#include <functional>

#include "DBWorkflowWorkerCoordinator.h"

namespace simcore::runner::parallel::simcoredb {

DBWorkflowWorkerCoordinator BuildDbBackedWorkflowCoordinator(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IStateDb* state_db,
    DBWorkflowWorkerCoordinatorConfig worker_cfg,
    CoordinatorIntegrationConfig integration_cfg,
    const simcore::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    DBWorkflowWorkerCoordinator::ReadyStepPersistFn persist_materialization_fn = {});

} // namespace simcore::runner::parallel::simcoredb
