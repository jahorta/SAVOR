#include "WorkflowSchedulerAdapter.h"

#include <utility>

namespace simcore::runner::parallel::simcoredb {

WorkflowSchedulerAdapter::WorkflowSchedulerAdapter(ScheduleFn schedule_fn)
    : schedule_fn_(std::move(schedule_fn)) {
}

ScheduledJobSet WorkflowSchedulerAdapter::MaterializeReadyStep(const WorkflowReadyStep& step) const {
    return schedule_fn_(step);
}

} // namespace simcore::runner::parallel::simcoredb
