#pragma once

#include "Execution/IExecutionDb.h"
#include "Runner/Runtime/Worksets/WorksetTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace savor::runner::parallel::savordb {

class WorksetJobOrganizer final {
public:
    struct Config {
        std::uint32_t maximum_items_per_workset =
            savor::runtime::WorkerWorksetLimits{}.maximum_items_per_workset;
        std::uint64_t maximum_encoded_workset_bytes =
            savor::runtime::WorkerWorksetLimits{}.maximum_encoded_workset_bytes;
    };

    explicit WorksetJobOrganizer(Config config = {});

    bool Organize(
        std::int64_t workflow_instance_id,
        std::vector<savor::db::FailedWorkflowWorksetJobRecord> jobs,
        savor::db::WorksetJobReorganizationPlan* plan_out,
        std::string* error_out) const;

private:
    Config config_;
};

} // namespace savor::runner::parallel::savordb
