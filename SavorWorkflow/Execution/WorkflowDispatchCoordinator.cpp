#include "WorkflowDispatchCoordinator.h"

#include <utility>

namespace savor::runner::parallel::savordb {

WorkflowDispatchCoordinator::WorkflowDispatchCoordinator(
    JobMaterializationService* materialization_service,
    DispatchToWorkerFn dispatch_to_worker)
    : materialization_service_(materialization_service)
    , dispatch_to_worker_(std::move(dispatch_to_worker)) {
}

bool WorkflowDispatchCoordinator::DispatchNextEligibleForWorker(
    std::size_t worker_idx,
    std::chrono::steady_clock::time_point now) {
    if (materialization_service_ == nullptr || !dispatch_to_worker_) {
        return false;
    }

    ClaimedJobRecord candidate{};
    if (!materialization_service_->TrySelectMaterializedJobForWorker(
        MaterializedJobSelectionAffinity{},
        &candidate)) {
        return false;
    }

    if (!candidate.payload.has_value()) {
        return false;
    }
    const bool sent = dispatch_to_worker_(worker_idx, candidate);
    if (!sent) {
        (void)materialization_service_->RequeueMaterializedJob(candidate.job_id);
        return false;
    }
    (void)materialization_service_->MarkDispatched(candidate.job_id, now);
    return true;
}

} // namespace savor::runner::parallel::savordb
