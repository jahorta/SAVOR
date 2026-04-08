#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "WorkflowMaterializationService.h"

namespace simcore::runner::parallel::simcoredb {

class WorkflowDispatchCoordinator {
public:
    using DispatchToWorkerFn = std::function<bool(std::size_t worker_idx, const ClaimedJobRecord& claimed_job)>;

    WorkflowDispatchCoordinator(
        WorkflowMaterializationService* materialization_service,
        DispatchToWorkerFn dispatch_to_worker);

    bool DispatchNextEligibleForWorker(
        std::size_t worker_idx,
        const std::optional<std::string>& worker_savestate_affinity,
        std::chrono::steady_clock::time_point now);

private:
    static bool BetterDispatchPriority(const ClaimedJobRecord& lhs, const ClaimedJobRecord& rhs);

    WorkflowMaterializationService* materialization_service_ = nullptr;
    DispatchToWorkerFn dispatch_to_worker_;
};

} // namespace simcore::runner::parallel::simcoredb
