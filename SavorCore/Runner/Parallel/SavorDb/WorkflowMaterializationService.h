#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "WorkflowSchedulerAdapter.h"

namespace savor::runner::parallel::savordb {

class WorkflowMaterializationService {
public:
    using ReadyStepPersistFn = std::function<void(const WorkflowReadyStep&, const ScheduledJobSet&)>;
    using MarkMaterializedFn = std::function<void(const WorkflowReadyStep&, const ScheduledJobSet&)>;

    WorkflowMaterializationService(
        WorkflowSchedulerAdapter* scheduler,
        ReadyStepPersistFn persist_materialization,
        MarkMaterializedFn mark_materialized);

    std::optional<ScheduledJobSet> MaterializeWorkflowStep(const WorkflowReadyStep& step);

private:
    WorkflowSchedulerAdapter* scheduler_ = nullptr;
    ReadyStepPersistFn persist_materialization_;
    MarkMaterializedFn mark_materialized_;
};

} // namespace savor::runner::parallel::savordb
