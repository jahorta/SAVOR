#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

#include "../DbService.h"

namespace savor::db::perf {

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
