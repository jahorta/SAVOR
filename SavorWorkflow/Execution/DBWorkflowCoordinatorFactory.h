#pragma once

#include <functional>

#include "DBWorkflowWorkerCoordinator.h"

namespace savor::runner::parallel::savordb {

DBWorkflowWorkerCoordinator BuildDbBackedWorkflowCoordinator(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    DBWorkflowWorkerCoordinatorConfig worker_cfg,
    CoordinatorIntegrationConfig integration_cfg,
    const savor::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
    DBWorkflowWorkerCoordinator::ReadyStepPersistFn persist_materialization_fn = {});

} // namespace savor::runner::parallel::savordb
