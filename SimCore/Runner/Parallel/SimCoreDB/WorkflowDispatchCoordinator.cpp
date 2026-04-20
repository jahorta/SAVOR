#include "WorkflowDispatchCoordinator.h"

#include <algorithm>
#include <utility>

namespace simcore::runner::parallel::simcoredb {

WorkflowDispatchCoordinator::WorkflowDispatchCoordinator(
    JobMaterializationService* materialization_service,
    DispatchToWorkerFn dispatch_to_worker)
    : materialization_service_(materialization_service)
    , dispatch_to_worker_(std::move(dispatch_to_worker)) {
}

bool WorkflowDispatchCoordinator::DispatchNextEligibleForWorker(
    std::size_t worker_idx,
    const std::optional<std::string>& worker_savestate_affinity,
    std::chrono::steady_clock::time_point now) {
    if (materialization_service_ == nullptr || !dispatch_to_worker_) {
        return false;
    }

    auto eligible = materialization_service_->ListByState(ClaimedJobLifecycleState::EligibleForDispatch);
    if (eligible.empty()) {
        return false;
    }

    std::stable_sort(eligible.begin(), eligible.end(), [&](const ClaimedJobRecord& lhs, const ClaimedJobRecord& rhs) {
        const bool lhs_savestate_match = worker_savestate_affinity.has_value()
            && lhs.affinity.savestate_affinity_key.has_value()
            && lhs.affinity.savestate_affinity_key.value() == worker_savestate_affinity.value();
        const bool rhs_savestate_match = worker_savestate_affinity.has_value()
            && rhs.affinity.savestate_affinity_key.has_value()
            && rhs.affinity.savestate_affinity_key.value() == worker_savestate_affinity.value();
        if (lhs_savestate_match != rhs_savestate_match) {
            return lhs_savestate_match;
        }

        return BetterDispatchPriority(lhs, rhs);
    });

    for (const auto& candidate : eligible) {
        if (!candidate.payload.has_value()) {
            continue;
        }
        const bool sent = dispatch_to_worker_(worker_idx, candidate);
        if (!sent) {
            continue;
        }
        (void)materialization_service_->MarkDispatched(candidate.job_id, now);
        return true;
    }

    return false;
}

bool WorkflowDispatchCoordinator::BetterDispatchPriority(const ClaimedJobRecord& lhs, const ClaimedJobRecord& rhs) {
    const bool lhs_savestate = lhs.affinity.savestate_affinity_key.has_value() && !lhs.affinity.savestate_affinity_key->empty();
    const bool rhs_savestate = rhs.affinity.savestate_affinity_key.has_value() && !rhs.affinity.savestate_affinity_key->empty();
    if (lhs_savestate != rhs_savestate) {
        return lhs_savestate;
    }

    const bool lhs_runtime = lhs.affinity.program_runtime_affinity_key.has_value() && !lhs.affinity.program_runtime_affinity_key->empty();
    const bool rhs_runtime = rhs.affinity.program_runtime_affinity_key.has_value() && !rhs.affinity.program_runtime_affinity_key->empty();
    if (lhs_runtime != rhs_runtime) {
        return lhs_runtime;
    }

    return lhs.claim_sequence < rhs.claim_sequence;
}

} // namespace simcore::runner::parallel::simcoredb
