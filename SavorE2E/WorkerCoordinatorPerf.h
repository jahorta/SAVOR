#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Cli.h"
#include "Common/Performance/DbPerfReport.h"
#include "Execution/DBWorkflowWorkerCoordinator.h"

namespace savor::e2e {

class WorkerCoordinatorPerfAccumulator {
public:
    void RecordSample(
        const savor::runner::parallel::savordb::WorkflowCoordinatorTelemetry& telemetry,
        const std::vector<WorkerSnapshot>& workers);

    [[nodiscard]] savor::db::perf::WorkerCoordinatorPerfSummary BuildSummary() const;

private:
    struct WorkerTotals {
        std::int64_t dispatch_success_count = 0;
        std::int64_t program_kind_switch_count = 0;
    };

    mutable std::mutex mutex_;
    bool available_ = false;
    std::int64_t active_samples_ = 0;
    std::int64_t running_worker_sample_total_ = 0;
    std::int64_t idle_worker_sample_total_ = 0;
    std::int64_t enough_work_samples_ = 0;
    std::int64_t enough_work_full_utilization_samples_ = 0;
    std::int64_t max_worker_target_ = 0;
    savor::runner::parallel::savordb::WorkflowCoordinatorTelemetry latest_telemetry_{};
    std::unordered_map<std::int64_t, WorkerTotals> worker_totals_;
};

void RecordWorkerCoordinatorPerfSample(
    const CliOptions& options,
    const savor::runner::parallel::savordb::WorkflowCoordinatorTelemetry& telemetry,
    const std::vector<WorkerSnapshot>& workers);

} // namespace savor::e2e
