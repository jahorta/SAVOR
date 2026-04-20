#include "WorkflowMaterializationService.h"

#include <utility>

namespace simcore::runner::parallel::simcoredb {

WorkflowMaterializationService::WorkflowMaterializationService(
    WorkflowSchedulerAdapter* scheduler,
    ReadyStepPersistFn persist_materialization,
    MarkMaterializedFn mark_materialized)
    : scheduler_(scheduler)
    , persist_materialization_(std::move(persist_materialization))
    , mark_materialized_(std::move(mark_materialized)) {
}

std::optional<ScheduledJobSet> WorkflowMaterializationService::MaterializeWorkflowStep(const WorkflowReadyStep& step) {
    if (scheduler_ == nullptr) {
        return std::nullopt;
    }

    const auto scheduled = scheduler_->MaterializeReadyStep(step);
    if (mark_materialized_) {
        mark_materialized_(step, scheduled);
    }
    if (persist_materialization_) {
        persist_materialization_(step, scheduled);
    }
    return scheduled;
}

} // namespace simcore::runner::parallel::simcoredb
