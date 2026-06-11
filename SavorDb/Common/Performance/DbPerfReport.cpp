#include "DbPerfReport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <thread>

namespace savor::db::perf {
namespace {

using Clock = std::chrono::steady_clock;

void WriteLatencyJson(std::ostream& out, const savor::db::core::DbLatencySnapshot& latency) {
    out << "{\"count\":" << latency.count
        << ",\"total_ms\":" << latency.total_ms
        << ",\"p50_ms\":" << latency.p50_ms
        << ",\"p95_ms\":" << latency.p95_ms
        << ",\"p99_ms\":" << latency.p99_ms
        << ",\"max_ms\":" << latency.max_ms
        << "}";
}

void WriteLaneJson(std::ostream& out, const savor::db::core::QueuedDbLaneTelemetrySnapshot& lane) {
    out << "{\"name\":\"" << JsonEscape(lane.name) << "\""
        << ",\"depth\":" << lane.depth
        << ",\"capacity\":" << lane.capacity
        << ",\"high_water_depth\":" << lane.high_water_depth
        << ",\"oldest_queued_age_ms\":" << lane.oldest_queued_age_ms
        << ",\"enqueued\":" << lane.enqueued
        << ",\"rejected\":" << lane.rejected
        << ",\"completed\":" << lane.completed
        << ",\"failed\":" << lane.failed
        << ",\"near_saturation_events\":" << lane.near_saturation_events
        << ",\"at_capacity_rejections\":" << lane.at_capacity_rejections
        << ",\"sqlite_busy\":" << lane.sqlite_busy
        << ",\"sqlite_locked\":" << lane.sqlite_locked
        << ",\"operations\":[";
    for (std::size_t i = 0; i < lane.operations.size(); ++i) {
        const auto& op = lane.operations[i];
        if (i != 0) {
            out << ",";
        }
        out << "{\"name\":\"" << JsonEscape(op.operation_name) << "\""
            << ",\"enqueued\":" << op.enqueued
            << ",\"rejected\":" << op.rejected
            << ",\"completed\":" << op.completed
            << ",\"failed\":" << op.failed
            << ",\"sqlite_busy\":" << op.sqlite_busy
            << ",\"sqlite_locked\":" << op.sqlite_locked
            << ",\"queue_wait\":";
        WriteLatencyJson(out, op.queue_wait);
        out << ",\"execution\":";
        WriteLatencyJson(out, op.execution);
        out << ",\"end_to_end\":";
        WriteLatencyJson(out, op.end_to_end);
        out << "}";
    }
    out << "]}";
}

void AccumulateLane(DecisionMetrics* metrics, const savor::db::core::QueuedDbLaneTelemetrySnapshot& lane) {
    if (metrics == nullptr) {
        return;
    }
    metrics->rejected += lane.rejected;
    metrics->failed += lane.failed;
    metrics->sqlite_busy += lane.sqlite_busy;
    metrics->sqlite_locked += lane.sqlite_locked;
    if (lane.capacity > 0) {
        metrics->max_high_water_ratio = std::max(
            metrics->max_high_water_ratio,
            static_cast<double>(lane.high_water_depth) / static_cast<double>(lane.capacity));
    }
    for (const auto& op : lane.operations) {
        metrics->max_end_to_end_p95_ms = std::max(metrics->max_end_to_end_p95_ms, op.end_to_end.p95_ms);
        metrics->max_end_to_end_p99_ms = std::max(metrics->max_end_to_end_p99_ms, op.end_to_end.p99_ms);
        metrics->max_queue_wait_p95_ms = std::max(metrics->max_queue_wait_p95_ms, op.queue_wait.p95_ms);
        metrics->max_queue_wait_p99_ms = std::max(metrics->max_queue_wait_p99_ms, op.queue_wait.p99_ms);
    }
}

const char* PassFail(bool pass) {
    return pass ? "PASS" : "FAIL";
}

} // namespace

std::string JsonEscape(const std::string& value) {
    std::ostringstream out;
    for (const char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
            } else {
                out << ch;
            }
            break;
        }
    }
    return out.str();
}

std::string TimestampForPath() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &time);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y%m%d-%H%M%S");
    return out.str();
}

std::int64_t TotalProjectionLag(
    const savor::db::uiread::projectors::UiReadProjectionTelemetrySnapshot& projection) {
    std::int64_t lag = 0;
    for (const auto& stream : projection.streams) {
        lag += stream.lag_count;
    }
    return lag;
}

void DrainUiReadProjection(
    savor::db::core::DBService& service,
    std::chrono::milliseconds timeout,
    std::string* last_error_out) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        std::string projection_error;
        service.RunUiReadProjectionOnce(&projection_error);
        if (!projection_error.empty() && last_error_out != nullptr) {
            *last_error_out = projection_error;
        }
        const auto snapshot = service.SnapshotPerformance();
        if (TotalProjectionLag(snapshot.ui_read_projection) == 0) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{ 50 });
    }
}

void WriteDatabasesJson(
    std::ostream& out,
    const std::vector<savor::db::core::NamedQueuedDbTelemetrySnapshot>& databases) {
    out << "[";
    for (std::size_t i = 0; i < databases.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << "{\"context\":\"" << JsonEscape(databases[i].db_context) << "\",\"write\":";
        WriteLaneJson(out, databases[i].queue.write_lane);
        out << ",\"read\":";
        WriteLaneJson(out, databases[i].queue.read_lane);
        out << "}";
    }
    out << "]";
}

void WriteProjectionJson(
    std::ostream& out,
    const savor::db::uiread::projectors::UiReadProjectionTelemetrySnapshot& projection) {
    out << "{\"running\":" << (projection.running ? "true" : "false")
        << ",\"run_once_count\":" << projection.run_once_count
        << ",\"succeeded_run_once_count\":" << projection.succeeded_run_once_count
        << ",\"failed_run_once_count\":" << projection.failed_run_once_count
        << ",\"last_run_duration_ms\":" << projection.last_run_duration_ms
        << ",\"max_run_duration_ms\":" << projection.max_run_duration_ms
        << ",\"configured_max_batch_size\":" << projection.configured_max_batch_size
        << ",\"configured_max_attempts\":" << projection.configured_max_attempts
        << ",\"streams\":[";
    for (std::size_t i = 0; i < projection.streams.size(); ++i) {
        const auto& stream = projection.streams[i];
        if (i != 0) {
            out << ",";
        }
        out << "{\"stream_id\":\"" << JsonEscape(stream.stream_id) << "\""
            << ",\"source_context\":\"" << JsonEscape(stream.source_context) << "\""
            << ",\"source_outbox_table\":\"" << JsonEscape(stream.source_outbox_table) << "\""
            << ",\"running\":" << (stream.running ? "true" : "false")
            << ",\"run_once_count\":" << stream.run_once_count
            << ",\"succeeded_run_once_count\":" << stream.succeeded_run_once_count
            << ",\"failed_run_once_count\":" << stream.failed_run_once_count
            << ",\"processed_event_count\":" << stream.processed_event_count
            << ",\"dead_letter_count\":" << stream.dead_letter_count
            << ",\"last_run_duration_ms\":" << stream.last_run_duration_ms
            << ",\"max_run_duration_ms\":" << stream.max_run_duration_ms
            << ",\"last_outbox_id\":" << stream.last_outbox_id
            << ",\"source_high_water_outbox_id\":" << stream.source_high_water_outbox_id
            << ",\"lag_count\":" << stream.lag_count
            << ",\"lag_age_ms\":" << stream.lag_age_ms
            << ",\"last_error\":\"" << JsonEscape(stream.last_error) << "\""
            << "}";
    }
    out << "]}";
}

void WriteSnapshotLine(
    std::ostream& out,
    const std::string& scenario,
    std::uint64_t elapsed_ms,
    const savor::db::core::DBServicePerformanceSnapshot& snapshot) {
    WriteSnapshotLine(out, scenario, elapsed_ms, snapshot.databases, &snapshot.ui_read_projection);
}

void WriteSnapshotLine(
    std::ostream& out,
    const std::string& scenario,
    std::uint64_t elapsed_ms,
    const std::vector<savor::db::core::NamedQueuedDbTelemetrySnapshot>& databases,
    const savor::db::uiread::projectors::UiReadProjectionTelemetrySnapshot* projection) {
    out << "{\"scenario\":\"" << JsonEscape(scenario) << "\",\"elapsed_ms\":" << elapsed_ms << ",\"databases\":";
    WriteDatabasesJson(out, databases);
    if (projection != nullptr) {
        out << ",\"projection\":";
        WriteProjectionJson(out, *projection);
    }
    out << "}\n";
}

DecisionMetrics BuildDecisionMetrics(const PerfRunReport& result) {
    DecisionMetrics metrics{};
    for (const auto& db : result.databases) {
        AccumulateLane(&metrics, db.queue.write_lane);
        AccumulateLane(&metrics, db.queue.read_lane);
    }
    metrics.projection_failed_runs = result.projection.failed_run_once_count;
    for (const auto& stream : result.projection.streams) {
        metrics.projection_lag_count += stream.lag_count;
        metrics.projection_max_lag_age_ms = std::max(metrics.projection_max_lag_age_ms, stream.lag_age_ms);
    }
    metrics.workload_failed = static_cast<std::uint64_t>(std::max<std::int64_t>(0, result.failed));
    return metrics;
}

std::string Recommendation(const DecisionMetrics& metrics) {
    if (metrics.rejected > 0 || metrics.max_high_water_ratio > 0.80 || metrics.sqlite_busy > 0 || metrics.sqlite_locked > 0) {
        return "Prototype command bus/CQRS";
    }
    if (metrics.max_end_to_end_p95_ms > 25
        || metrics.max_end_to_end_p99_ms > 100
        || metrics.max_queue_wait_p95_ms > 10
        || metrics.max_queue_wait_p99_ms > 50
        || metrics.max_high_water_ratio > 0.50
        || metrics.projection_failed_runs > 0
        || metrics.projection_lag_count > 0
        || metrics.workload_failed > 0) {
        return "Investigate tuning current architecture";
    }
    return "Keep current queued DB architecture";
}

void WriteSummaryJson(const std::filesystem::path& report_dir, const PerfRunReport& result) {
    const auto metrics = BuildDecisionMetrics(result);
    std::ofstream out(report_dir / "perf-summary.json", std::ios::binary);
    out << "{\n"
        << "  \"scenario\": \"" << JsonEscape(result.scenario) << "\",\n"
        << "  \"measured_workload\": \"" << JsonEscape(result.measured_workload) << "\",\n"
        << "  \"first_error\": \"" << JsonEscape(result.first_error) << "\",\n"
        << "  \"load_level\": \"" << JsonEscape(result.load_level) << "\",\n"
        << "  \"configuration\": \"" << JsonEscape(result.configuration) << "\",\n"
        << "  \"worker_count\": " << result.worker_count << ",\n"
        << "  \"repeat_count\": " << result.repeat_count << ",\n"
        << "  \"elapsed_ms\": " << result.elapsed_ms << ",\n"
        << "  \"submitted\": " << result.submitted << ",\n"
        << "  \"completed\": " << result.completed << ",\n"
        << "  \"failed\": " << result.failed << ",\n"
        << "  \"recommendation\": \"" << Recommendation(metrics) << "\",\n"
        << "  \"decision_metrics\": {\n"
        << "    \"rejected\": " << metrics.rejected << ",\n"
        << "    \"queue_failed\": " << metrics.failed << ",\n"
        << "    \"sqlite_busy\": " << metrics.sqlite_busy << ",\n"
        << "    \"sqlite_locked\": " << metrics.sqlite_locked << ",\n"
        << "    \"max_end_to_end_p95_ms\": " << metrics.max_end_to_end_p95_ms << ",\n"
        << "    \"max_end_to_end_p99_ms\": " << metrics.max_end_to_end_p99_ms << ",\n"
        << "    \"max_queue_wait_p95_ms\": " << metrics.max_queue_wait_p95_ms << ",\n"
        << "    \"max_queue_wait_p99_ms\": " << metrics.max_queue_wait_p99_ms << ",\n"
        << "    \"max_high_water_ratio\": " << metrics.max_high_water_ratio << ",\n"
        << "    \"projection_failed_runs\": " << metrics.projection_failed_runs << ",\n"
        << "    \"projection_lag_count\": " << metrics.projection_lag_count << ",\n"
        << "    \"projection_max_lag_age_ms\": " << metrics.projection_max_lag_age_ms << ",\n"
        << "    \"workload_failed\": " << metrics.workload_failed << "\n"
        << "  },\n"
        << "  \"projection\": ";
    WriteProjectionJson(out, result.projection);
    out << ",\n  \"databases\": ";
    WriteDatabasesJson(out, result.databases);
    out << "\n}\n";
}

void WriteMarkdownReport(const std::filesystem::path& report_dir, const PerfRunReport& result) {
    const auto metrics = BuildDecisionMetrics(result);
    std::ofstream out(report_dir / "perf-report.md", std::ios::binary);
    out << "# Savor DB Performance Report\n\n"
        << "Scenario: `" << result.scenario << "`\n\n"
        << "Measured workload: " << result.measured_workload << "\n\n";
    if (!result.load_level.empty()) {
        out << "Load level: `" << result.load_level << "`\n\n";
    }
    if (!result.configuration.empty()) {
        out << "Configuration: " << result.configuration << "\n\n";
    }
    out << "Elapsed: " << result.elapsed_ms << " ms\n\n"
        << "Workers: " << result.worker_count << "  \n"
        << "Repeat: " << result.repeat_count << "  \n"
        << "Submitted: " << result.submitted << "  \n"
        << "Completed: " << result.completed << "  \n"
        << "Failed: " << result.failed << "\n\n";
    if (!result.first_error.empty()) {
        out << "First error: `" << result.first_error << "`\n\n";
    }

    out << "## Queue Summary\n\n"
        << "| DB | Lane | Completed | Failed | Rejected | High Water | Capacity | Wait p95 | Wait p99 | E2E p95 | E2E p99 |\n"
        << "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& db : result.databases) {
        const std::array<std::pair<const char*, const savor::db::core::QueuedDbLaneTelemetrySnapshot*>, 2> lanes{
            std::pair<const char*, const savor::db::core::QueuedDbLaneTelemetrySnapshot*>{ "write", &db.queue.write_lane },
            std::pair<const char*, const savor::db::core::QueuedDbLaneTelemetrySnapshot*>{ "read", &db.queue.read_lane },
        };
        for (const auto& [lane_name, lane] : lanes) {
            std::uint64_t wait_p95 = 0;
            std::uint64_t wait_p99 = 0;
            std::uint64_t e2e_p95 = 0;
            std::uint64_t e2e_p99 = 0;
            for (const auto& op : lane->operations) {
                wait_p95 = std::max(wait_p95, op.queue_wait.p95_ms);
                wait_p99 = std::max(wait_p99, op.queue_wait.p99_ms);
                e2e_p95 = std::max(e2e_p95, op.end_to_end.p95_ms);
                e2e_p99 = std::max(e2e_p99, op.end_to_end.p99_ms);
            }
            out << "| " << db.db_context
                << " | " << lane_name
                << " | " << lane->completed
                << " | " << lane->failed
                << " | " << lane->rejected
                << " | " << lane->high_water_depth
                << " | " << lane->capacity
                << " | " << wait_p95
                << " | " << wait_p99
                << " | " << e2e_p95
                << " | " << e2e_p99
                << " |\n";
        }
    }

    out << "\n## Decision Gates\n\n"
        << "| Gate | Result | Value |\n"
        << "|---|---|---:|\n"
        << "| Zero rejected DB queue requests | " << PassFail(metrics.rejected == 0) << " | " << metrics.rejected << " |\n"
        << "| DB API p95 end-to-end <= 25 ms | " << PassFail(metrics.max_end_to_end_p95_ms <= 25) << " | " << metrics.max_end_to_end_p95_ms << " |\n"
        << "| DB API p99 end-to-end <= 100 ms | " << PassFail(metrics.max_end_to_end_p99_ms <= 100) << " | " << metrics.max_end_to_end_p99_ms << " |\n"
        << "| Queue wait p95 <= 10 ms | " << PassFail(metrics.max_queue_wait_p95_ms <= 10) << " | " << metrics.max_queue_wait_p95_ms << " |\n"
        << "| Queue wait p99 <= 50 ms | " << PassFail(metrics.max_queue_wait_p99_ms <= 50) << " | " << metrics.max_queue_wait_p99_ms << " |\n"
        << "| Queue high-water <= 50% capacity | " << PassFail(metrics.max_high_water_ratio <= 0.50) << " | " << metrics.max_high_water_ratio << " |\n"
        << "| SQLite busy/locked count == 0 | " << PassFail(metrics.sqlite_busy == 0 && metrics.sqlite_locked == 0) << " | "
        << (metrics.sqlite_busy + metrics.sqlite_locked) << " |\n"
        << "| Projection failed run count == 0 | " << PassFail(metrics.projection_failed_runs == 0) << " | "
        << metrics.projection_failed_runs << " |\n"
        << "| Projection lag count == 0 | " << PassFail(metrics.projection_lag_count == 0) << " | "
        << metrics.projection_lag_count << " |\n"
        << "| Projection max lag age ms | PASS | " << metrics.projection_max_lag_age_ms << " |\n"
        << "| Workload failed operation count == 0 | " << PassFail(metrics.workload_failed == 0) << " | "
        << metrics.workload_failed << " |\n\n";

    out << "## Projection Telemetry\n\n"
        << "RunOnce count: " << result.projection.run_once_count << "  \n"
        << "Succeeded: " << result.projection.succeeded_run_once_count << "  \n"
        << "Failed: " << result.projection.failed_run_once_count << "  \n"
        << "Last duration ms: " << result.projection.last_run_duration_ms << "  \n"
        << "Max duration ms: " << result.projection.max_run_duration_ms << "\n\n";

    out << "## Recommendation\n\n"
        << Recommendation(metrics) << "\n";
}

} // namespace savor::db::perf
