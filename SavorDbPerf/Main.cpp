#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Common/DbService.h"
#include "Common/QueuedDb.h"
#include "Execution/IExecutionDb.h"
#include "Execution/Workflow/WorkflowOrchestration.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string scenario = "queued-db-micro";
    int duration_sec = 5;
    int producers = 2;
    int workers = 2;
    int jobs = 1000;
    int batch_size = 16;
    int read_ratio = 1;
    int write_ratio = 1;
    int snapshot_interval_ms = 1000;
    std::filesystem::path report_dir;
};

struct PerfCounters {
    std::atomic<std::int64_t> submitted{ 0 };
    std::atomic<std::int64_t> completed{ 0 };
    std::atomic<std::int64_t> failed{ 0 };
};

struct RunResult {
    std::string scenario;
    std::string measured_workload;
    std::string first_error;
    std::vector<savor::db::core::NamedQueuedDbTelemetrySnapshot> databases;
    savor::db::uiread::projectors::AttachedUiReadProjectionTelemetrySnapshot projection;
    std::int64_t submitted = 0;
    std::int64_t completed = 0;
    std::int64_t failed = 0;
    std::uint64_t elapsed_ms = 0;
};

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

void PrintUsage() {
    std::cout
        << "SavorDbPerf --scenario queued-db-micro|execution-queue|projection-lag|workflow-materialization|e2e-replay\n"
        << "  --duration-sec N --producers N --workers N --jobs N --batch-size N\n"
        << "  --read-ratio N --write-ratio N --snapshot-interval-ms N --report-dir PATH\n";
}

bool ParseOptions(int argc, char** argv, Options* options) {
    if (options == nullptr) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };
        try {
            if (arg == "--help" || arg == "-h") {
                PrintUsage();
                return false;
            } else if (arg == "--scenario") {
                options->scenario = need_value("--scenario");
            } else if (arg == "--duration-sec") {
                options->duration_sec = std::stoi(need_value("--duration-sec"));
            } else if (arg == "--producers") {
                options->producers = std::stoi(need_value("--producers"));
            } else if (arg == "--workers") {
                options->workers = std::stoi(need_value("--workers"));
            } else if (arg == "--jobs") {
                options->jobs = std::stoi(need_value("--jobs"));
            } else if (arg == "--batch-size") {
                options->batch_size = std::stoi(need_value("--batch-size"));
            } else if (arg == "--read-ratio") {
                options->read_ratio = std::stoi(need_value("--read-ratio"));
            } else if (arg == "--write-ratio") {
                options->write_ratio = std::stoi(need_value("--write-ratio"));
            } else if (arg == "--snapshot-interval-ms") {
                options->snapshot_interval_ms = std::stoi(need_value("--snapshot-interval-ms"));
            } else if (arg == "--report-dir") {
                options->report_dir = need_value("--report-dir");
            } else {
                std::cerr << "Unknown argument: " << arg << "\n";
                return false;
            }
        } catch (const std::exception& ex) {
            std::cerr << ex.what() << "\n";
            return false;
        }
    }
    options->duration_sec = std::max(1, options->duration_sec);
    options->producers = std::max(1, options->producers);
    options->workers = std::max(1, options->workers);
    options->jobs = std::max(1, options->jobs);
    options->batch_size = std::max(1, options->batch_size);
    options->snapshot_interval_ms = std::max(100, options->snapshot_interval_ms);
    if (options->report_dir.empty()) {
        options->report_dir = std::filesystem::path("perf-runs") / (TimestampForPath() + "-" + options->scenario);
    }
    return true;
}

std::filesystem::path MakeDbRoot(const Options& options) {
    return options.report_dir / "db";
}

savor::db::DbConfigPaths MakeDbPaths(const Options& options) {
    const auto root = MakeDbRoot(options);
    return savor::db::DbConfigPaths{
        .execution_db_path = root / "execution.db",
        .state_db_path = root / "state.db",
        .analysis_db_path = root / "analysis.db",
        .authoring_db_path = root / "authoring.db",
        .ui_read_db_path = root / "ui_read.db",
        .archive_db_path = root / "archive.db",
        .object_store_root = root / "objects",
        .archive_store_root = root / "archive_objects",
    };
}

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

void WriteDatabasesJson(std::ostream& out, const std::vector<savor::db::core::NamedQueuedDbTelemetrySnapshot>& databases) {
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

void WriteSnapshotLine(
    std::ofstream& out,
    const std::string& scenario,
    std::uint64_t elapsed_ms,
    const std::vector<savor::db::core::NamedQueuedDbTelemetrySnapshot>& databases) {
    out << "{\"scenario\":\"" << JsonEscape(scenario) << "\",\"elapsed_ms\":" << elapsed_ms << ",\"databases\":";
    WriteDatabasesJson(out, databases);
    out << "}\n";
}

std::vector<savor::db::core::NamedQueuedDbTelemetrySnapshot> MicroSnapshots(
    const savor::db::core::QueuedDbLane& write_lane,
    const savor::db::core::QueuedDbLane& read_lane) {
    return {
        {
            .db_context = "QueuedMicro",
            .queue = savor::db::core::BuildQueuedDbTelemetrySnapshot(&write_lane, &read_lane),
        },
    };
}

RunResult RunQueuedDbMicro(const Options& options, std::ofstream& snapshots) {
    savor::db::core::QueuedDbLane write_lane("perf-write", 4096);
    savor::db::core::QueuedDbLane read_lane("perf-read", 4096);
    std::string error;
    if (!write_lane.Start(&error) || !read_lane.Start(&error)) {
        throw std::runtime_error("failed to start perf lanes: " + error);
    }

    PerfCounters counters;
    std::atomic<int> next_job{ 0 };
    std::atomic<bool> stop_sampling{ false };
    const auto started_at = Clock::now();
    std::thread sampler([&]() {
        while (!stop_sampling.load()) {
            const auto elapsed_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count());
            WriteSnapshotLine(snapshots, options.scenario, elapsed_ms, MicroSnapshots(write_lane, read_lane));
            std::this_thread::sleep_for(std::chrono::milliseconds{ options.snapshot_interval_ms });
        }
    });

    std::vector<std::thread> producers;
    for (int p = 0; p < options.producers; ++p) {
        producers.emplace_back([&, p]() {
            for (;;) {
                const int index = next_job.fetch_add(1);
                if (index >= options.jobs) {
                    break;
                }
                auto work = [&]() {
                    counters.completed.fetch_add(1);
                };
                const int ratio_total = std::max(1, options.read_ratio + options.write_ratio);
                const bool read = (index % ratio_total) < options.read_ratio;
                const bool accepted = read
                    ? read_lane.Enqueue("Perf.ReadNoop", work)
                    : write_lane.Enqueue("Perf.WriteNoop", work);
                counters.submitted.fetch_add(1);
                if (!accepted) {
                    counters.failed.fetch_add(1);
                }
                (void)p;
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }

    const auto deadline = Clock::now() + std::chrono::seconds{ options.duration_sec };
    while (counters.completed.load() + counters.failed.load() < options.jobs && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{ 10 });
    }
    stop_sampling = true;
    sampler.join();
    write_lane.Stop();
    read_lane.Stop();

    const auto elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count());
    RunResult result{};
    result.scenario = options.scenario;
    result.measured_workload = "Synthetic in-process queued read/write no-op operations.";
    result.databases = MicroSnapshots(write_lane, read_lane);
    result.submitted = counters.submitted.load();
    result.completed = counters.completed.load();
    result.failed = counters.failed.load();
    result.elapsed_ms = elapsed_ms;
    return result;
}

RunResult RunExecutionQueue(const Options& options, std::ofstream& snapshots) {
    std::filesystem::create_directories(MakeDbRoot(options));
    savor::db::core::DBService service(MakeDbPaths(options));
    std::string error;
    if (!service.Start(&error)) {
        throw std::runtime_error("failed to start DBService: " + error);
    }
    auto* execution = service.ExecutionDb();
    if (execution == nullptr) {
        throw std::runtime_error("execution DB unavailable");
    }

    std::int64_t job_set_id = 0;
    if (!execution->CreateJobSet(
        {
            .program_kind = 1,
            .purpose = "SavorDbPerf execution queue",
            .created_by = std::string("SavorDbPerf"),
            .expected_total = options.jobs,
        },
        &job_set_id,
        &error)) {
        throw std::runtime_error("failed to create job set: " + error);
    }

    PerfCounters counters;
    std::mutex first_error_mtx;
    std::string first_error;
    auto capture_error = [&](const std::string& value) {
        if (value.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(first_error_mtx);
        if (first_error.empty()) {
            first_error = value;
        }
    };
    std::atomic<int> next_job{ 0 };
    std::atomic<bool> stop_sampling{ false };
    std::mutex job_ids_mtx;
    std::vector<std::int64_t> job_ids;
    job_ids.reserve(static_cast<std::size_t>(options.jobs));
    const auto started_at = Clock::now();
    std::thread sampler([&]() {
        while (!stop_sampling.load()) {
            const auto elapsed_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count());
            WriteSnapshotLine(snapshots, options.scenario, elapsed_ms, service.SnapshotPerformance().databases);
            std::this_thread::sleep_for(std::chrono::milliseconds{ options.snapshot_interval_ms });
        }
    });

    std::vector<std::thread> producers;
    for (int p = 0; p < options.producers; ++p) {
        producers.emplace_back([&, p]() {
            for (;;) {
                const int index = next_job.fetch_add(1);
                if (index >= options.jobs) {
                    break;
                }
                std::int64_t job_id = 0;
                std::string local_error;
                const bool ok = execution->EnqueueJob(
                    {
                        .job_set_id = job_set_id,
                        .program_kind = 1,
                        .program_version = 1,
                        .program_ref_kind = "perf.synthetic",
                        .program_ref_id = index + 1,
                        .fingerprint = "perf-job-" + std::to_string(index),
                        .priority = index % 8,
                        .max_attempts = 1,
                        .input_ini = "[perf]\nindex=" + std::to_string(index) + "\n",
                    },
                    &job_id,
                    &local_error);
                counters.submitted.fetch_add(1);
                if (ok && job_id > 0) {
                    {
                        std::lock_guard<std::mutex> lock(job_ids_mtx);
                        job_ids.push_back(job_id);
                    }
                    counters.completed.fetch_add(1);
                } else {
                    capture_error(local_error);
                    counters.failed.fetch_add(1);
                }
                (void)p;
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    std::atomic<std::size_t> next_read{ 0 };
    std::vector<std::thread> workers;
    for (int w = 0; w < options.workers; ++w) {
        workers.emplace_back([&]() {
            for (;;) {
                const auto index = next_read.fetch_add(1);
                std::int64_t job_id = 0;
                {
                    std::lock_guard<std::mutex> lock(job_ids_mtx);
                    if (index >= job_ids.size()) {
                        break;
                    }
                    job_id = job_ids[index];
                }
                if (!execution->GetJob(job_id).has_value()) {
                    counters.failed.fetch_add(1);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    stop_sampling = true;
    sampler.join();
    const auto service_snapshot = service.SnapshotPerformance();
    service.Stop();

    const auto elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count());
    RunResult result{};
    result.scenario = options.scenario;
    result.measured_workload = "Execution DB job set creation, concurrent enqueue, and concurrent job readback.";
    result.first_error = first_error;
    result.databases = service_snapshot.databases;
    result.projection = service_snapshot.ui_read_projection;
    result.submitted = counters.submitted.load();
    result.completed = counters.completed.load();
    result.failed = counters.failed.load();
    result.elapsed_ms = elapsed_ms;
    return result;
}

RunResult RunWorkflowMaterialization(const Options& options, std::ofstream& snapshots) {
    std::filesystem::create_directories(MakeDbRoot(options));
    savor::db::core::DBService service(MakeDbPaths(options));
    std::string error;
    if (!service.Start(&error)) {
        throw std::runtime_error("failed to start DBService: " + error);
    }
    auto* commands = service.ExecutionDb() != nullptr ? service.ExecutionDb()->WorkflowCommandService() : nullptr;
    if (commands == nullptr) {
        throw std::runtime_error("workflow command service unavailable");
    }

    PerfCounters counters;
    std::mutex first_error_mtx;
    std::string first_error;
    auto capture_error = [&](const std::string& value) {
        if (value.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(first_error_mtx);
        if (first_error.empty()) {
            first_error = value;
        }
    };
    std::atomic<bool> stop_sampling{ false };
    const auto started_at = Clock::now();
    std::thread sampler([&]() {
        while (!stop_sampling.load()) {
            const auto elapsed_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count());
            WriteSnapshotLine(snapshots, options.scenario, elapsed_ms, service.SnapshotPerformance().databases);
            std::this_thread::sleep_for(std::chrono::milliseconds{ options.snapshot_interval_ms });
        }
    });

    std::atomic<int> next_workflow{ 0 };
    std::vector<std::thread> producers;
    for (int p = 0; p < options.producers; ++p) {
        producers.emplace_back([&]() {
            for (;;) {
                const int index = next_workflow.fetch_add(1);
                if (index >= options.jobs) {
                    break;
                }
                savor::db::execution::workflow::WorkflowCreateUnitStepSpec step{};
                step.step_key = "perf_step";
                step.step_key_suffix = "perf_step";
                step.step_kind = "perf.synthetic";
                step.max_attempts = 1;

                savor::db::execution::workflow::WorkflowCreateUnitActivationSpec activation{};
                activation.activation_key = "perf_activation";
                activation.graph_node_key = "perf_activation";
                activation.unit_kind = "perf.synthetic";
                activation.display_name = "Perf Synthetic";
                activation.activation_params_json = "{}";
                activation.steps.push_back(step);

                savor::db::execution::workflow::WorkflowCreateInstanceCommand command{};
                command.workflow_kind = "workflow_graph";
                command.workflow_graph_revision_id = 1;
                command.root_scope_kind = "manual";
                command.root_scope_id = index + 1;
                command.created_by = "SavorDbPerf";
                command.unit_activations.push_back(activation);

                std::int64_t workflow_instance_id = 0;
                std::string local_error;
                counters.submitted.fetch_add(1);
                if (commands->CreateWorkflowInstance(command, &workflow_instance_id, &local_error) && workflow_instance_id > 0) {
                    counters.completed.fetch_add(1);
                } else {
                    capture_error(local_error);
                    counters.failed.fetch_add(1);
                }
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }

    stop_sampling = true;
    sampler.join();
    service.RunUiReadProjectionOnce(&error);
    const auto service_snapshot = service.SnapshotPerformance();
    service.Stop();

    const auto elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count());
    RunResult result{};
    result.scenario = options.scenario;
    result.measured_workload = "Workflow instance graph creation under concurrent producers.";
    result.first_error = first_error;
    result.databases = service_snapshot.databases;
    result.projection = service_snapshot.ui_read_projection;
    result.submitted = counters.submitted.load();
    result.completed = counters.completed.load();
    result.failed = counters.failed.load();
    result.elapsed_ms = elapsed_ms;
    return result;
}

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
    std::uint64_t workload_failed = 0;
};

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

DecisionMetrics BuildDecisionMetrics(const RunResult& result) {
    DecisionMetrics metrics{};
    for (const auto& db : result.databases) {
        AccumulateLane(&metrics, db.queue.write_lane);
        AccumulateLane(&metrics, db.queue.read_lane);
    }
    metrics.projection_failed_runs = result.projection.failed_run_once_count;
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
        || metrics.workload_failed > 0) {
        return "Investigate tuning current architecture";
    }
    return "Keep current queued DB architecture";
}

void WriteSummaryJson(const Options& options, const RunResult& result, const DecisionMetrics& metrics) {
    std::ofstream out(options.report_dir / "perf-summary.json", std::ios::binary);
    out << "{\n"
        << "  \"scenario\": \"" << JsonEscape(result.scenario) << "\",\n"
        << "  \"measured_workload\": \"" << JsonEscape(result.measured_workload) << "\",\n"
        << "  \"first_error\": \"" << JsonEscape(result.first_error) << "\",\n"
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
        << "    \"workload_failed\": " << metrics.workload_failed << "\n"
        << "  },\n"
        << "  \"databases\": ";
    WriteDatabasesJson(out, result.databases);
    out << "\n}\n";
}

const char* PassFail(bool pass) {
    return pass ? "PASS" : "FAIL";
}

void WriteMarkdownReport(const Options& options, const RunResult& result, const DecisionMetrics& metrics) {
    std::ofstream out(options.report_dir / "perf-report.md", std::ios::binary);
    out << "# Savor DB Performance Report\n\n"
        << "Scenario: `" << result.scenario << "`\n\n"
        << "Measured workload: " << result.measured_workload << "\n\n"
        << "Elapsed: " << result.elapsed_ms << " ms\n\n"
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

RunResult RunScenario(const Options& options, std::ofstream& snapshots) {
    if (options.scenario == "queued-db-micro") {
        return RunQueuedDbMicro(options, snapshots);
    }
    if (options.scenario == "execution-queue" || options.scenario == "projection-lag") {
        auto result = RunExecutionQueue(options, snapshots);
        if (options.scenario == "projection-lag") {
            result.scenario = options.scenario;
            result.measured_workload = "Execution outbox burst with UI projection service running during enqueue/claim load.";
        }
        return result;
    }
    if (options.scenario == "workflow-materialization") {
        return RunWorkflowMaterialization(options, snapshots);
    }
    if (options.scenario == "e2e-replay") {
        throw std::runtime_error("e2e-replay should be captured by running SavorE2E with DBService::SnapshotPerformance polling; direct artifact orchestration is not embedded in SavorDbPerf.");
    }
    throw std::runtime_error("unknown scenario: " + options.scenario);
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        return 2;
    }

    try {
        std::filesystem::create_directories(options.report_dir);
        std::ofstream snapshots(options.report_dir / "perf-snapshots.jsonl", std::ios::binary);
        auto result = RunScenario(options, snapshots);
        snapshots.flush();
        const auto metrics = BuildDecisionMetrics(result);
        WriteSummaryJson(options, result, metrics);
        WriteMarkdownReport(options, result, metrics);
        std::cout << "SavorDbPerf complete\n"
                  << "Report: " << (options.report_dir / "perf-report.md").string() << "\n"
                  << "Summary: " << (options.report_dir / "perf-summary.json").string() << "\n"
                  << "Recommendation: " << Recommendation(metrics) << "\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "SavorDbPerf failed: " << ex.what() << "\n";
        return 1;
    }
}
