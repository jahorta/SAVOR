#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "JobMaterializationService.h"

namespace savor::runner::parallel::savordb {

class WorkflowDispatchCoordinator {
public:
    using DispatchToWorkerFn = std::function<bool(std::size_t worker_idx, const ClaimedJobRecord& claimed_job)>;

    WorkflowDispatchCoordinator(
        JobMaterializationService* materialization_service,
        DispatchToWorkerFn dispatch_to_worker);

    bool DispatchNextEligibleForWorker(
        std::size_t worker_idx,
        std::chrono::steady_clock::time_point now);

private:
    JobMaterializationService* materialization_service_ = nullptr;
    DispatchToWorkerFn dispatch_to_worker_;
};

} // namespace savor::runner::parallel::savordb
