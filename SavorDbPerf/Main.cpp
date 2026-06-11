#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
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
#include "Common/Performance/DbPerfReport.h"
#include "Common/QueuedDb.h"
#include "Execution/IExecutionDb.h"
#include "Execution/Workflow/WorkflowOrchestration.h"

namespace {

using Clock = std::chrono::steady_clock;
using savor::db::perf::JsonEscape;

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
    int repeat = 1;
    int timeout_ms = 0;
    int poll_ms = 0;
    std::string load_level = "low";
    std::vector<std::string> e2e_scenarios;
    std::filesystem::path e2e_exe;
    std::filesystem::path savestate_file;
    std::filesystem::path dtm_file;
    std::filesystem::path iso_path;
    std::filesystem::path dolphin_base_dir;
    std::filesystem::path migration_root;
    std::filesystem::path workspace_root;
    std::filesystem::path worker_dir_root;
    std::string durable_lines;
    std::string tasmovie_headroom;
    std::string tasmovie_rtc;
    std::string tasmovie_rtc_min;
    std::string tasmovie_rtc_max;
    std::string seedprobe_combo_attempts_per_target;
    std::filesystem::path report_dir;
};

struct PerfCounters {
    std::atomic<std::int64_t> submitted{ 0 };
    std::atomic<std::int64_t> completed{ 0 };
    std::atomic<std::int64_t> failed{ 0 };
};

using RunResult = savor::db::perf::PerfRunReport;

void PrintUsage() {
    std::cout
        << "SavorDbPerf --scenario queued-db-micro|execution-queue|projection-lag|workflow-materialization|e2e-replay\n"
        << "  --duration-sec N --producers N --workers N --jobs N --batch-size N\n"
        << "  --read-ratio N --write-ratio N --snapshot-interval-ms N --report-dir PATH\n"
        << "  e2e-replay: --e2e-scenario NAME --load-level low|mid|high|very_high --repeat N\n"
        << "              --iso PATH --dolphin-base-dir PATH [--savestate-file PATH] [--dtm-file PATH]\n";
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
            } else if (arg == "--timeout-ms") {
                options->timeout_ms = std::stoi(need_value("--timeout-ms"));
            } else if (arg == "--poll-ms") {
                options->poll_ms = std::stoi(need_value("--poll-ms"));
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
            } else if (arg == "--repeat") {
                options->repeat = std::stoi(need_value("--repeat"));
            } else if (arg == "--load-level") {
                options->load_level = need_value("--load-level");
            } else if (arg == "--e2e-scenario") {
                options->e2e_scenarios.push_back(need_value("--e2e-scenario"));
            } else if (arg == "--e2e-exe") {
                options->e2e_exe = need_value("--e2e-exe");
            } else if (arg == "--savestate-file") {
                options->savestate_file = need_value("--savestate-file");
            } else if (arg == "--dtm-file") {
                options->dtm_file = need_value("--dtm-file");
            } else if (arg == "--iso") {
                options->iso_path = need_value("--iso");
            } else if (arg == "--dolphin-base-dir") {
                options->dolphin_base_dir = need_value("--dolphin-base-dir");
            } else if (arg == "--migration-root") {
                options->migration_root = need_value("--migration-root");
            } else if (arg == "--workspace-root") {
                options->workspace_root = need_value("--workspace-root");
            } else if (arg == "--worker-dir-root") {
                options->worker_dir_root = need_value("--worker-dir-root");
            } else if (arg == "--durable-lines") {
                options->durable_lines = need_value("--durable-lines");
            } else if (arg == "--tasmovie-headroom" || arg == "--tasmovie-headroom-x10" || arg == "--headroom") {
                options->tasmovie_headroom = need_value(arg.c_str());
            } else if (arg == "--tasmovie-rtc" || arg == "--rtc") {
                options->tasmovie_rtc = need_value(arg.c_str());
            } else if (arg == "--tasmovie-rtc-min" || arg == "--rtc-min") {
                options->tasmovie_rtc_min = need_value(arg.c_str());
            } else if (arg == "--tasmovie-rtc-max" || arg == "--rtc-max") {
                options->tasmovie_rtc_max = need_value(arg.c_str());
            } else if (arg == "--seedprobe-combo-attempts-per-target" || arg == "--combo-attempts-per-target") {
                options->seedprobe_combo_attempts_per_target = need_value(arg.c_str());
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
    options->repeat = std::max(1, options->repeat);
    if (options->report_dir.empty()) {
        options->report_dir = std::filesystem::path("perf-runs") / (savor::db::perf::TimestampForPath() + "-" + options->scenario);
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
            savor::db::perf::WriteSnapshotLine(snapshots, options.scenario, elapsed_ms, MicroSnapshots(write_lane, read_lane));
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
            savor::db::perf::WriteSnapshotLine(snapshots, options.scenario, elapsed_ms, service.SnapshotPerformance());
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

    savor::db::perf::DrainUiReadProjection(service, std::chrono::seconds{ 5 }, &error);
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
            savor::db::perf::WriteSnapshotLine(snapshots, options.scenario, elapsed_ms, service.SnapshotPerformance());
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

    savor::db::perf::DrainUiReadProjection(service, std::chrono::seconds{ 5 }, &error);
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
    std::int64_t projection_lag_count = 0;
    std::int64_t projection_max_lag_age_ms = 0;
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

std::string QuoteArg(const std::string& value) {
    std::string out = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            out += "\\\"";
        } else {
            out += ch;
        }
    }
    out += "\"";
    return out;
}

bool PathLooksReleaseBinary(const std::filesystem::path& path) {
    std::string value = path.string();
    std::replace(value.begin(), value.end(), '/', '\\');
    return value.find("\\Release\\") != std::string::npos
        || value.find("\\release\\") != std::string::npos;
}

std::filesystem::path ResolveE2EExe(const Options& options, const char* argv0) {
    if (!options.e2e_exe.empty()) {
        return std::filesystem::absolute(options.e2e_exe);
    }
    const auto own_exe = std::filesystem::absolute(std::filesystem::path(argv0));
    return own_exe.parent_path() / "SavorE2E.exe";
}

void AppendPathArg(std::ostringstream& cmd, const char* flag, const std::filesystem::path& value) {
    if (!value.empty()) {
        cmd << ' ' << flag << ' ' << QuoteArg(value.string());
    }
}

void AppendStringArg(std::ostringstream& cmd, const char* flag, const std::string& value) {
    if (!value.empty()) {
        cmd << ' ' << flag << ' ' << QuoteArg(value);
    }
}

int RunE2EReplay(const Options& options, const char* argv0) {
#ifndef NDEBUG
    std::cerr << "e2e-replay perf runs must use a Release SavorDbPerf build.\n";
    return 2;
#else
    const auto e2e_exe = ResolveE2EExe(options, argv0);
    if (!std::filesystem::exists(e2e_exe)) {
        std::cerr << "SavorE2E.exe not found: " << e2e_exe.string() << "\n";
        return 2;
    }
    if (!PathLooksReleaseBinary(e2e_exe)) {
        std::cerr << "e2e-replay requires a Release SavorE2E.exe: " << e2e_exe.string() << "\n";
        return 2;
    }

    std::filesystem::create_directories(options.report_dir);
    std::ostringstream cmd;
    cmd << QuoteArg(e2e_exe.string())
        << " --perf-report-dir " << QuoteArg(options.report_dir.string())
        << " --perf-snapshot-interval-ms " << options.snapshot_interval_ms
        << " --repeat " << options.repeat
        << " --load-level " << QuoteArg(options.load_level)
        << " --worker-count 15";
    const auto scenarios = options.e2e_scenarios.empty()
        ? std::vector<std::string>{ "all" }
        : options.e2e_scenarios;
    for (const auto& scenario : scenarios) {
        cmd << " --scenario " << QuoteArg(scenario);
    }
    if (options.timeout_ms > 0) {
        cmd << " --timeout-ms " << options.timeout_ms;
    }
    if (options.poll_ms > 0) {
        cmd << " --poll-ms " << options.poll_ms;
    }
    AppendPathArg(cmd, "--iso", options.iso_path);
    AppendPathArg(cmd, "--dolphin-base-dir", options.dolphin_base_dir);
    AppendPathArg(cmd, "--savestate-file", options.savestate_file);
    AppendPathArg(cmd, "--dtm-file", options.dtm_file);
    AppendPathArg(cmd, "--migration-root", options.migration_root);
    AppendPathArg(cmd, "--workspace-root", options.workspace_root);
    AppendPathArg(cmd, "--worker-dir-root", options.worker_dir_root);
    AppendStringArg(cmd, "--durable-lines", options.durable_lines);
    AppendStringArg(cmd, "--tasmovie-headroom", options.tasmovie_headroom);
    AppendStringArg(cmd, "--tasmovie-rtc", options.tasmovie_rtc);
    AppendStringArg(cmd, "--tasmovie-rtc-min", options.tasmovie_rtc_min);
    AppendStringArg(cmd, "--tasmovie-rtc-max", options.tasmovie_rtc_max);
    AppendStringArg(cmd, "--seedprobe-combo-attempts-per-target", options.seedprobe_combo_attempts_per_target);

    std::cout << "Running E2E replay via " << e2e_exe.string() << "\n";
    const std::string shell_command = "cmd.exe /S /C \"" + cmd.str() + "\"";
    const int rc = std::system(shell_command.c_str());
    std::cout << "Report: " << (options.report_dir / "perf-report.md").string() << "\n"
              << "Summary: " << (options.report_dir / "perf-summary.json").string() << "\n";
    return rc;
#endif
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
        << "    \"projection_lag_count\": " << metrics.projection_lag_count << ",\n"
        << "    \"projection_max_lag_age_ms\": " << metrics.projection_max_lag_age_ms << ",\n"
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
        if (options.scenario == "e2e-replay") {
            return RunE2EReplay(options, argv[0]);
        }
        std::filesystem::create_directories(options.report_dir);
        std::ofstream snapshots(options.report_dir / "perf-snapshots.jsonl", std::ios::binary);
        auto result = RunScenario(options, snapshots);
        snapshots.flush();
        const auto metrics = savor::db::perf::BuildDecisionMetrics(result);
        savor::db::perf::WriteSummaryJson(options.report_dir, result);
        savor::db::perf::WriteMarkdownReport(options.report_dir, result);
        std::cout << "SavorDbPerf complete\n"
                  << "Report: " << (options.report_dir / "perf-report.md").string() << "\n"
                  << "Summary: " << (options.report_dir / "perf-summary.json").string() << "\n"
                  << "Recommendation: " << savor::db::perf::Recommendation(metrics) << "\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "SavorDbPerf failed: " << ex.what() << "\n";
        return 1;
    }
}
