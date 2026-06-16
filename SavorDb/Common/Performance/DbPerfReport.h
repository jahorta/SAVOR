#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

#include "../DbService.h"

namespace savor::db::perf {

struct WorkerCoordinatorWorkerMetric {
    std::int64_t worker_id = 0;
    std::int64_t dispatch_success_count = 0;
    std::int64_t program_kind_switch_count = 0;
};

struct WorkerCoordinatorPerfSummary {
    bool available = false;
    std::int64_t dispatch_attempts = 0;
    std::int64_t dispatch_successes = 0;
    std::int64_t dispatch_misses = 0;
    std::int64_t dispatch_miss_rate_basis_points = 0;
    std::int64_t worker_dispatch_min = 0;
    std::int64_t worker_dispatch_max = 0;
    double worker_dispatch_avg = 0.0;
    std::int64_t active_samples = 0;
    double avg_running_workers = 0.0;
    double avg_idle_workers = 0.0;
    std::int64_t enough_work_samples = 0;
    std::int64_t enough_work_full_utilization_samples = 0;
    double enough_work_full_utilization_pct = 0.0;
    std::int64_t claim_target = 0;
    std::int64_t claim_attempts = 0;
    std::int64_t claimed_jobs = 0;
    std::int64_t clean_zero_claims = 0;
    std::int64_t claim_errors = 0;
    std::int64_t partial_claims = 0;
    bool no_jobs_available = false;
    std::int64_t max_program_kind_switches = 0;
    std::int64_t workers_over_program_kind_switch_limit = 0;
    std::vector<std::int64_t> workers_over_program_kind_switch_limit_ids;
    std::int64_t progress_batches = 0;
    std::int64_t max_progress_batch_size = 0;
    std::int64_t results_received = 0;
    std::int64_t stale_claims = 0;
    std::int64_t materialization_failures = 0;
    std::int64_t payload_materialization_failures = 0;
    std::vector<WorkerCoordinatorWorkerMetric> workers;
};

struct PerfRunReport {
    std::string scenario;
    std::string measured_workload;
    std::string first_error;
    std::string load_level;
    std::string configuration;
    std::vector<savor::db::core::NamedQueuedDbTelemetrySnapshot> databases;
    savor::db::uiread::projectors::UiReadProjectionTelemetrySnapshot projection;
    std::int64_t submitted = 0;
    std::int64_t completed = 0;
    std::int64_t failed = 0;
    std::uint64_t elapsed_ms = 0;
    int worker_count = 0;
    int repeat_count = 1;
    WorkerCoordinatorPerfSummary worker_coordinator;
};

struct DecisionMetrics {
    std::uint64_t rejected = 0;
    std::uint64_t failed = 0;
    std::uint64_t sqlite_busy = 0;
    std::uint64_t sqlite_locked = 0;
    std::uint64_t max_end_to_end_p95_ms = 0;
    std::uint64_t max_end_to_end_p99_ms = 0;
    std::uint64_t max_queue_wait_p95_ms = 0;
    std::uint64_t max_queue_wait_p99_ms = 0;
    double max_high_water_ratio = 0.0;
    std::uint64_t projection_failed_runs = 0;
    std::int64_t projection_lag_count = 0;
    std::int64_t projection_max_lag_age_ms = 0;
    std::uint64_t workload_failed = 0;
    std::uint64_t worker_claim_errors = 0;
    std::uint64_t worker_materialization_failures = 0;
    std::uint64_t worker_payload_materialization_failures = 0;
    std::uint64_t worker_stale_claims = 0;
    std::uint64_t workers_over_program_kind_switch_limit = 0;
    double worker_enough_work_full_utilization_pct = 0.0;
};

std::string JsonEscape(const std::string& value);
std::string TimestampForPath();
std::int64_t TotalProjectionLag(
    const savor::db::uiread::projectors::UiReadProjectionTelemetrySnapshot& projection);
void DrainUiReadProjection(
    savor::db::core::DBService& service,
    std::chrono::milliseconds timeout,
    std::string* last_error_out = nullptr);

void WriteDatabasesJson(
    std::ostream& out,
    const std::vector<savor::db::core::NamedQueuedDbTelemetrySnapshot>& databases);
void WriteProjectionJson(
    std::ostream& out,
    const savor::db::uiread::projectors::UiReadProjectionTelemetrySnapshot& projection);
void WriteSnapshotLine(
    std::ostream& out,
    const std::string& scenario,
    std::uint64_t elapsed_ms,
    const savor::db::core::DBServicePerformanceSnapshot& snapshot);
void WriteSnapshotLine(
    std::ostream& out,
    const std::string& scenario,
    std::uint64_t elapsed_ms,
    const std::vector<savor::db::core::NamedQueuedDbTelemetrySnapshot>& databases,
    const savor::db::uiread::projectors::UiReadProjectionTelemetrySnapshot* projection = nullptr);

DecisionMetrics BuildDecisionMetrics(const PerfRunReport& result);
std::string Recommendation(const DecisionMetrics& metrics);
void WriteSummaryJson(const std::filesystem::path& report_dir, const PerfRunReport& result);
void WriteMarkdownReport(const std::filesystem::path& report_dir, const PerfRunReport& result);

} // namespace savor::db::perf
