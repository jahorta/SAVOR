#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace simcore::runner::parallel::simcoredb {

struct WorkflowReadyStep {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::string step_key;
    std::string step_kind;
    int priority = 0;
    std::optional<std::int64_t> input_ref_id;
};

struct ScheduledJobSet {
    std::int64_t job_set_id = 0;
    std::int64_t workflow_step_id = 0;
    std::string program_ref_kind;
    std::int64_t program_ref_id = 0;
    std::vector<std::string> event_lines;
};

class WorkflowSchedulerAdapter {
public:
    using ScheduleFn = std::function<ScheduledJobSet(const WorkflowReadyStep& step)>;

    explicit WorkflowSchedulerAdapter(ScheduleFn schedule_fn);

    ScheduledJobSet MaterializeReadyStep(const WorkflowReadyStep& step) const;

private:
    ScheduleFn schedule_fn_;
};

} // namespace simcore::runner::parallel::simcoredb
