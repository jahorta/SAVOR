#include "WorkerCoordinatorPerf.h"

#include <algorithm>
#include <limits>

namespace savor::e2e {
namespace {

bool IsActiveWorker(const WorkerSnapshot& worker) {
    return worker.state != WorkerStateKind::Dead && worker.state != WorkerStateKind::Stopping;
}

bool IsRunningWorker(const WorkerSnapshot& worker) {
    return worker.job_id.has_value() || worker.state == WorkerStateKind::Running;
}

} // namespace

void WorkerCoordinatorPerfAccumulator::RecordSample(
    const savor::runner::parallel::savordb::WorkflowCoordinatorTelemetry& telemetry,
    const std::vector<WorkerSnapshot>& workers) {
    std::lock_guard<std::mutex> lock(mutex_);
    available_ = true;
    latest_telemetry_ = telemetry;

    std::int64_t active = 0;
    std::int64_t running = 0;
    std::int64_t idle = 0;
    for (const auto& worker : workers) {
        if (!IsActiveWorker(worker)) {
            continue;
        }
        ++active;
        if (IsRunningWorker(worker)) {
            ++running;
        } else {
            ++idle;
        }
    }

    if (active > 0) {
        ++active_samples_;
        running_worker_sample_total_ += running;
        idle_worker_sample_total_ += idle;
        max_worker_target_ = std::max(max_worker_target_, active);

        const auto outstanding_known_work = std::max<std::int64_t>(
            0,
            telemetry.claimed_job_count - telemetry.results_received_count);
        if (outstanding_known_work >= active) {
            ++enough_work_samples_;
            if (running >= active) {
                ++enough_work_full_utilization_samples_;
            }
        }
    }

    for (const auto& worker : telemetry.workers) {
        auto& totals = worker_totals_[worker.worker_id];
        totals.dispatch_success_count = std::max(totals.dispatch_success_count, worker.dispatch_success_count);
        totals.program_kind_switch_count = std::max(totals.program_kind_switch_count, worker.program_kind_switch_count);
    }
}

savor::db::perf::WorkerCoordinatorPerfSummary WorkerCoordinatorPerfAccumulator::BuildSummary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    savor::db::perf::WorkerCoordinatorPerfSummary summary{};
    summary.available = available_;
    if (!available_) {
        return summary;
    }

    summary.dispatch_attempts = latest_telemetry_.dispatch_attempt_count;
    summary.dispatch_successes = latest_telemetry_.dispatch_success_count;
    summary.dispatch_misses = latest_telemetry_.dispatch_miss_count;
    summary.dispatch_miss_rate_basis_points = latest_telemetry_.dispatch_miss_rate_basis_points;
    summary.active_samples = active_samples_;
    summary.avg_running_workers = active_samples_ > 0
        ? static_cast<double>(running_worker_sample_total_) / static_cast<double>(active_samples_)
        : 0.0;
    summary.avg_idle_workers = active_samples_ > 0
        ? static_cast<double>(idle_worker_sample_total_) / static_cast<double>(active_samples_)
        : 0.0;
    summary.enough_work_samples = enough_work_samples_;
    summary.enough_work_full_utilization_samples = enough_work_full_utilization_samples_;
    summary.enough_work_full_utilization_pct = enough_work_samples_ > 0
        ? (static_cast<double>(enough_work_full_utilization_samples_) * 100.0) / static_cast<double>(enough_work_samples_)
        : 0.0;

    summary.claim_target = max_worker_target_;
    summary.claim_attempts = latest_telemetry_.claim_attempt_count;
    summary.claimed_jobs = latest_telemetry_.claimed_job_count;
    summary.clean_zero_claims = latest_telemetry_.clean_zero_claim_count;
    summary.claim_errors = latest_telemetry_.claim_error_count;
    summary.partial_claims = latest_telemetry_.partial_claim_count;
    summary.no_jobs_available = latest_telemetry_.no_jobs_available;

    summary.progress_batches = latest_telemetry_.progress_batch_count;
    summary.max_progress_batch_size = latest_telemetry_.max_progress_batch_size;
    summary.results_received = latest_telemetry_.results_received_count;
    summary.stale_claims = latest_telemetry_.stale_claim_count;
    summary.materialization_failures = latest_telemetry_.materialization_failure_count;
    summary.payload_materialization_failures = latest_telemetry_.payload_materialization_failure_count;

    summary.worker_dispatch_min = worker_totals_.empty() ? 0 : std::numeric_limits<std::int64_t>::max();
    std::int64_t dispatch_total = 0;
    for (const auto& [worker_id, totals] : worker_totals_) {
        summary.workers.push_back(savor::db::perf::WorkerCoordinatorWorkerMetric{
            .worker_id = worker_id,
            .dispatch_success_count = totals.dispatch_success_count,
            .program_kind_switch_count = totals.program_kind_switch_count,
        });
        summary.worker_dispatch_min = std::min(summary.worker_dispatch_min, totals.dispatch_success_count);
        summary.worker_dispatch_max = std::max(summary.worker_dispatch_max, totals.dispatch_success_count);
        summary.max_program_kind_switches = std::max(summary.max_program_kind_switches, totals.program_kind_switch_count);
        dispatch_total += totals.dispatch_success_count;
        if (totals.program_kind_switch_count > 3) {
            ++summary.workers_over_program_kind_switch_limit;
            summary.workers_over_program_kind_switch_limit_ids.push_back(worker_id);
        }
    }
    std::sort(summary.workers.begin(), summary.workers.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.worker_id < rhs.worker_id;
    });
    std::sort(
        summary.workers_over_program_kind_switch_limit_ids.begin(),
        summary.workers_over_program_kind_switch_limit_ids.end());
    summary.worker_dispatch_avg = worker_totals_.empty()
        ? 0.0
        : static_cast<double>(dispatch_total) / static_cast<double>(worker_totals_.size());
    return summary;
}

void RecordWorkerCoordinatorPerfSample(
    const CliOptions& options,
    const savor::runner::parallel::savordb::WorkflowCoordinatorTelemetry& telemetry,
    const std::vector<WorkerSnapshot>& workers) {
    if (options.worker_coordinator_perf == nullptr) {
        return;
    }
    options.worker_coordinator_perf->RecordSample(telemetry, workers);
}

} // namespace savor::e2e
