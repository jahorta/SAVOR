#include "SeedProbeRealWorkerScenario.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "Execution/Workflow/WorkflowModeProvider.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "DB/Scheduling/JobSetsRepo.h"
#include "Runner/Parallel/SimCoreDB/DBWorkflowWorkerCoordinator.h"

#include "Cli.h"
#include "DbSetup.h"
#include "MultiLineProgressRenderer.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace simcore::e2e {

using simcore::db::execution::workflow::WorkflowExecutionMode;
using simcore::db::execution::workflow::StaticWorkflowModeProvider;
using simcore::runner::parallel::simcoredb::CoordinatorIntegrationConfig;
using simcore::runner::parallel::simcoredb::DBWorkflowWorkerCoordinator;
using simcore::runner::parallel::simcoredb::DBWorkflowWorkerCoordinatorConfig;
using simcore::runner::parallel::simcoredb::ScheduledJobSet;
using simcore::runner::parallel::simcoredb::WorkflowReadyStep;
using ::WorkerSnapshot;
using ::WorkerStateKind;

namespace {

constexpr std::int64_t kSeedProbeAverageUniqueCountEstimate = 25;

std::int64_t ComputeSeedProbeTimeoutMs(std::int64_t baseline_timeout_ms) {
    const std::int64_t grid_probe_count = static_cast<std::int64_t>(kSeedProbeSamplesPerAxis) * 2 * 3;
    const std::int64_t multiplier = 1 + grid_probe_count + kSeedProbeAverageUniqueCountEstimate;
    return baseline_timeout_ms * multiplier;
}

const char* ToString(simcore::db::execution::workflow::WorkflowInstanceState state) {
    using simcore::db::execution::workflow::WorkflowInstanceState;
    switch (state) {
    case WorkflowInstanceState::Pending: return "PENDING";
    case WorkflowInstanceState::Running: return "RUNNING";
    case WorkflowInstanceState::Completed: return "COMPLETED";
    case WorkflowInstanceState::Failed: return "FAILED";
    case WorkflowInstanceState::Canceled: return "CANCELED";
    }
    return "UNKNOWN";
}

const char* ToString(simcore::db::execution::workflow::WorkflowStepState state) {
    using simcore::db::execution::workflow::WorkflowStepState;
    switch (state) {
    case WorkflowStepState::Waiting: return "WAITING";
    case WorkflowStepState::Ready: return "READY";
    case WorkflowStepState::Materialized: return "MATERIALIZED";
    case WorkflowStepState::Running: return "RUNNING";
    case WorkflowStepState::Completed: return "COMPLETED";
    case WorkflowStepState::Failed: return "FAILED";
    case WorkflowStepState::Skipped: return "SKIPPED";
    }
    return "UNKNOWN";
}

bool IsInteractiveStdout() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return ::isatty(fileno(stdout)) != 0;
#endif
}

std::string FormatCoordinatorTelemetryLine(const WorkflowCoordinatorTelemetry& telemetry, size_t active_workers) {
    std::ostringstream oss;
    oss << "workers=" << active_workers
        << " scans=" << telemetry.ready_scan_count
        << " enqueued=" << telemetry.ready_steps_enqueued
        << " materialized=" << telemetry.materialization_count
        << " dispatch=" << telemetry.dispatch_attempt_count
        << " miss=" << telemetry.dispatch_miss_count
        << " progress_batches=" << telemetry.progress_batch_count;
    return oss.str();
}

std::string FormatWorkerRollupLine(const std::vector<WorkerSnapshot>& workers) {
    std::size_t running = 0;
    std::size_t idle = 0;
    std::size_t dead = 0;
    std::vector<std::string> assigned_job_ids;
    std::optional<std::string> last_error;

    for (const auto& worker : workers) {
        if (worker.job_id.has_value()) {
            ++running;
            assigned_job_ids.push_back(std::to_string(*worker.job_id));
        } else if (worker.state == WorkerStateKind::Idle || worker.state == WorkerStateKind::Paused) {
            ++idle;
        } else if (worker.state == WorkerStateKind::Dead || worker.state == WorkerStateKind::Stopping) {
            ++dead;
        } else {
            ++idle;
        }

        if (!worker.last_error.empty()) {
            std::ostringstream err;
            err << "w" << worker.worker_id << " pid=" << worker.pid << " err=" << worker.last_error;
            last_error = err.str();
        }
    }

    std::ostringstream oss;
    oss << "worker_rollup running=" << running << " idle=" << idle << " dead=" << dead;
    if (!assigned_job_ids.empty()) {
        oss << " jobs=";
        for (std::size_t i = 0; i < assigned_job_ids.size(); ++i) {
            if (i > 0) {
                oss << ",";
            }
            oss << assigned_job_ids[i];
        }
    } else {
        oss << " jobs=none";
    }
    if (last_error.has_value()) {
        oss << " last_error=" << *last_error;
    }
    return oss.str();
}

std::string FormatWorkflowStateLine(const simcore::db::execution::workflow::WorkflowGraphSnapshot& graph) {
    std::array<std::size_t, 7> counts{};
    for (const auto& step : graph.steps) {
        const auto idx = static_cast<std::size_t>(step.state);
        if (idx < counts.size()) {
            ++counts[idx];
        }
    }

    const std::size_t completed = counts[static_cast<std::size_t>(simcore::db::execution::workflow::WorkflowStepState::Completed)];
    std::ostringstream oss;
    oss << "workflow=" << ToString(graph.instance.state) << " steps=" << completed << "/" << graph.steps.size()
        << " [WAITING=" << counts[static_cast<std::size_t>(simcore::db::execution::workflow::WorkflowStepState::Waiting)]
        << " READY=" << counts[static_cast<std::size_t>(simcore::db::execution::workflow::WorkflowStepState::Ready)]
        << " MATERIALIZED=" << counts[static_cast<std::size_t>(simcore::db::execution::workflow::WorkflowStepState::Materialized)]
        << " RUNNING=" << counts[static_cast<std::size_t>(simcore::db::execution::workflow::WorkflowStepState::Running)]
        << " COMPLETED=" << counts[static_cast<std::size_t>(simcore::db::execution::workflow::WorkflowStepState::Completed)]
        << " FAILED=" << counts[static_cast<std::size_t>(simcore::db::execution::workflow::WorkflowStepState::Failed)]
        << " SKIPPED=" << counts[static_cast<std::size_t>(simcore::db::execution::workflow::WorkflowStepState::Skipped)]
        << "]";
    return oss.str();
}

std::vector<std::string> FormatActiveJobSetLines(
    simcore::db::execution::workflow::SqliteExecutionDb* execution_db,
    const simcore::db::execution::workflow::WorkflowGraphSnapshot& graph) {
    (void)execution_db;
    using simcore::db::execution::workflow::WorkflowStepState;
    const auto is_terminal = [](WorkflowStepState state) {
        return state == WorkflowStepState::Completed
            || state == WorkflowStepState::Failed
            || state == WorkflowStepState::Skipped;
    };
    const auto select_step = [&](WorkflowStepState target) -> const simcore::db::execution::workflow::WorkflowStepSnapshot* {
        const auto it = std::find_if(graph.steps.begin(), graph.steps.end(), [&](const auto& step) {
            return step.state == target;
        });
        return it == graph.steps.end() ? nullptr : &(*it);
    };

    const auto* selected_step = select_step(WorkflowStepState::Running);
    if (selected_step == nullptr) selected_step = select_step(WorkflowStepState::Materialized);
    if (selected_step == nullptr) selected_step = select_step(WorkflowStepState::Ready);
    if (selected_step == nullptr) {
        const auto it = std::find_if(graph.steps.begin(), graph.steps.end(), [&](const auto& step) {
            return !is_terminal(step.state);
        });
        selected_step = (it == graph.steps.end()) ? nullptr : &(*it);
    }
    if (selected_step == nullptr) {
        return { "current_step=none (all steps terminal)" };
    }

    std::ostringstream step_label;
    step_label << "current_step key=" << selected_step->step_key
               << " kind=" << selected_step->step_kind
               << " workflow_step_id=" << selected_step->workflow_step_id
               << " state=" << ToString(selected_step->state);
    if (!selected_step->job_set_id.has_value()) {
        return { step_label.str(), "current step not materialized yet" };
    }

    const auto job_set_id = *selected_step->job_set_id;
    const auto lite = simcore::db::JobSetsRepo::GetLite(job_set_id);
    if (!lite.ok) {
        return { step_label.str(), "job_set progress unavailable" };
    }

    const auto& row = lite.value;
    const std::int64_t total = row.total_jobs;
    const std::int64_t done = row.completed_jobs;
    const std::int64_t ok = row.succeeded_jobs;
    const std::int64_t fail = row.failed_jobs;
    const std::int64_t can = row.canceled_jobs;
    const std::int64_t remaining = std::max<std::int64_t>(0, total - ok - fail - can);

    std::ostringstream progress;
    progress << "job_set=" << job_set_id
             << " progress done=" << done << "/" << total
             << " ok=" << ok
             << " fail=" << fail
             << " can=" << can
             << " remaining=" << remaining;
    return { step_label.str(), progress.str() };
}

std::size_t CountActiveWorkers(const std::vector<WorkerSnapshot>& workers) {
    return std::count_if(workers.begin(), workers.end(), [](const WorkerSnapshot& worker) {
        return worker.state != WorkerStateKind::Dead && worker.state != WorkerStateKind::Stopping;
    });
}

std::vector<std::string> BuildProgressLines(
    simcore::db::execution::workflow::SqliteExecutionDb* execution_db,
    const WorkflowCoordinatorTelemetry& telemetry,
    const std::vector<WorkerSnapshot>& worker_snapshot,
    const std::optional<simcore::db::execution::workflow::WorkflowGraphSnapshot>& graph) {
    std::vector<std::string> lines;
    lines.push_back(FormatCoordinatorTelemetryLine(telemetry, worker_snapshot.size()));
    if (CountActiveWorkers(worker_snapshot) > 1) {
        lines.push_back(FormatWorkerRollupLine(worker_snapshot));
    }
    if (!graph.has_value()) {
        lines.push_back("workflow=unavailable");
        lines.push_back("job_set=unavailable");
        return lines;
    }

    lines.push_back(FormatWorkflowStateLine(*graph));
    const auto job_lines = FormatActiveJobSetLines(execution_db, *graph);
    lines.insert(lines.end(), job_lines.begin(), job_lines.end());
    return lines;
}

} // namespace

bool RunSeedProbeRealWorkerSmoke(
    const CliOptions& options,
    const char* argv0,
    simcore::db::core::DBService* db_service,
    std::string* error_out) {
    if (db_service == nullptr) {
        if (error_out) *error_out = "db service is required";
        return false;
    }
    if (!db_service->IsRunning()) {
        if (error_out) *error_out = "db service must be started in main before running scenarios";
        return false;
    }

    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::exists(worker_exe)) {
        if (error_out) *error_out = "SimCoreWorker.exe was not found next to SimCoreDBe2e: " + worker_exe.string();
        return false;
    }
    std::string err;

    std::int64_t savestate_id = 0;
    if (!SeedStateSavestate(db_service->StateDb(), options.savestate_file, &savestate_id, &err)) {
        if (error_out) *error_out = "failed seeding StateDB savestate: " + err;
        return false;
    }

    std::int64_t seed_probe_spec_id = 0;
    if (!SeedAuthoringSpec(db_service->AuthoringDb(), &seed_probe_spec_id, &err)) {
        if (error_out) *error_out = "failed seeding AuthoringDB seedprobe spec: " + err;
        return false;
    }

    auto* execution_db = dynamic_cast<simcore::db::execution::workflow::SqliteExecutionDb*>(db_service->ExecutionDb());
    if (execution_db == nullptr) {
        if (error_out) *error_out = "DBService execution db is not sqlite-backed";
        return false;
    }

    std::int64_t workflow_instance_id = 0;
    if (!SeedExecutionWorkflow(
            db_service->AnalysisDb(),
            execution_db,
            savestate_id,
            seed_probe_spec_id,
            &workflow_instance_id,
            &err)) {
        if (error_out) *error_out = "failed seeding execution workflow rows: " + err;
        return false;
    }

    StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::Workflow, .source = "SimCoreDBe2e" });

    std::int64_t next_job_set_id = 30000;
    auto schedule = [&](const WorkflowReadyStep& step) {
        ++next_job_set_id;
        return ScheduledJobSet{ .job_set_id = next_job_set_id, .workflow_step_id = step.workflow_step_id };
    };

    DBWorkflowWorkerCoordinator coordinator(
        execution_db,
        &mode_provider,
        DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 1,
            .controller_sleep_ms = static_cast<uint32_t>(options.poll_ms),
            .worker_exe_path = worker_exe.string(),
            .iso_path = options.iso_path.string(),
            .dolphin_base_dir = options.dolphin_base_dir.string(),
            .worker_dir_root = options.worker_dir_root.value_or(
                std::filesystem::temp_directory_path() / "simcoredbe2e-workers").string(),
        },
        CoordinatorIntegrationConfig{},
        schedule);

    coordinator.Start();
    auto* ui_read_db = db_service->UiReadDb();
    if (ui_read_db == nullptr) {
        coordinator.Stop();
        if (error_out) *error_out = "DBService ui read db unavailable";
        return false;
    }

    const auto timeout_ms = ComputeSeedProbeTimeoutMs(options.timeout_ms);
    const bool interactive_stdout = IsInteractiveStdout();
    MultiLineProgressRenderer progress_renderer;
    const auto started = std::chrono::steady_clock::now();
    std::size_t poll_count = 0;
    std::size_t ticks_since_snapshot = 0;
    bool reached_completed = false;
    bool saw_terminal_failure = false;
    bool timed_out = false;
    std::vector<std::string> latest_lines;
    while (std::chrono::steady_clock::now() - started < std::chrono::milliseconds(timeout_ms)) {
        (void)ui_read_db->ListProjectionSubscriptions("Execution", "exec_outbox_message");
        ++poll_count;
        ++ticks_since_snapshot;

        const auto telemetry = coordinator.SnapshotTelemetry();
        const auto graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
        latest_lines = BuildProgressLines(execution_db, telemetry, coordinator.SnapshotWorkers(), graph);
        if (interactive_stdout) {
            progress_renderer.SetLines(latest_lines);
            progress_renderer.Render(std::cout);
        } else if (ticks_since_snapshot >= 10 || poll_count == 1) {
            ticks_since_snapshot = 0;
            std::cout << "[seedprobe] ";
            for (std::size_t i = 0; i < latest_lines.size(); ++i) {
                if (i > 0) {
                    std::cout << " | ";
                }
                std::cout << latest_lines[i];
            }
            std::cout << '\n';
        }

        if (graph.has_value()) {
            using simcore::db::execution::workflow::WorkflowInstanceState;
            if (graph->instance.state == WorkflowInstanceState::Completed) {
                reached_completed = true;
                break;
            }
            if (graph->instance.state == WorkflowInstanceState::Failed
                || graph->instance.state == WorkflowInstanceState::Canceled) {
                saw_terminal_failure = true;
                break;
            }
        }
        if (telemetry.ready_scan_count == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
    }
    if (!reached_completed && !saw_terminal_failure) {
        timed_out = true;
    }

    coordinator.Stop();

    if (poll_count == 0) {
        if (error_out) *error_out = "UiReadDB polling loop did not execute";
        return false;
    }

    const auto final_graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    const auto final_telemetry = coordinator.SnapshotTelemetry();
    if (final_graph.has_value()) {
        latest_lines = BuildProgressLines(execution_db, final_telemetry, coordinator.SnapshotWorkers(), final_graph);
    }
    if (latest_lines.empty()) {
        latest_lines.push_back("workflow=unavailable");
    }

    std::string final_status = "success";
    if (saw_terminal_failure) {
        final_status = "failure";
    } else if (!final_graph.has_value()
        || final_graph->instance.state != simcore::db::execution::workflow::WorkflowInstanceState::Completed
        || timed_out) {
        final_status = "timeout";
    }

    std::cout << "[seedprobe-final] status=" << final_status << '\n';
    for (const auto& line : latest_lines) {
        std::cout << "  " << line << '\n';
    }

    if (!final_graph.has_value()
        || final_graph->instance.state != simcore::db::execution::workflow::WorkflowInstanceState::Completed) {
        if (error_out) *error_out = saw_terminal_failure
            ? "workflow did not complete successfully"
            : "workflow did not reach COMPLETED state before timeout";
        return false;
    }

    return true;
}

} // namespace simcore::e2e
