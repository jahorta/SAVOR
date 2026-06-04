#include "SeedProbeRealWorkerScenario.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeContracts.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbePhaseRegistration.h"
#include "Runner/Parallel/SimCoreDB/DBWorkflowCoordinatorFactory.h"
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

using simcore::runner::parallel::simcoredb::CoordinatorIntegrationConfig;
using simcore::runner::parallel::simcoredb::BuildDbBackedWorkflowCoordinator;
using simcore::runner::parallel::simcoredb::DBWorkflowWorkerCoordinator;
using simcore::runner::parallel::simcoredb::DBWorkflowWorkerCoordinatorConfig;
using simcore::runner::parallel::simcoredb::WorkflowCoordinatorTelemetry;
using ::WorkerSnapshot;
using ::WorkerStateKind;

namespace {

constexpr std::int64_t kSeedProbeAverageUniqueCountEstimate = 25;

std::int64_t ComputeSeedProbeTimeoutMs(std::int64_t baseline_timeout_ms) {
    const std::int64_t grid_probe_count = static_cast<std::int64_t>(kSeedProbeSamplesPerAxis) * static_cast<std::int64_t>(kSeedProbeSamplesPerAxis) * 3;
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

simcore::db::execution::programdb::seedprobe::ResultsIni BuildSeedProbeResultsIni(const simcore::PRResult& result) {
    simcore::db::execution::programdb::seedprobe::ResultsIni parsed{};
    parsed.w_err = result.ps.w_err;
    if (parsed.w_err == 0) {
        result.ps.ctx.get(simcore::keys::core::DW_RUN_OUTCOME_CODE, parsed.dw_err);
    }
    if (result.ps.ok) {
        result.ps.ctx.get(simcore::keys::seed::RNG_SEED, parsed.rng_seed);
        result.ps.ctx.get(simcore::keys::core::VI_FIRST, parsed.vi_start);
        result.ps.ctx.get(simcore::keys::core::VI_LAST, parsed.vi_end);
    }
    return parsed;
}

std::string FormatSeedProbeResultEventLine(const simcore::PRResult& result) {
    const auto parsed = BuildSeedProbeResultsIni(result);
    const bool failed = parsed.w_err != 0 || parsed.dw_err != 0;

    std::ostringstream oss;
    if (!failed) {
        oss << "[seedprobe-result] job=" << result.job_id
            << " worker=" << result.worker_id
            << " rng_seed=" << parsed.rng_seed
            << " vi=" << parsed.vi_start << "-" << parsed.vi_end;
        return oss.str();
    }

    oss << "[seedprobe-error] job=" << result.job_id
        << " worker=" << result.worker_id
        << " failed cause=";
    if (parsed.w_err != 0) {
        oss << "w_err=" << simcore::WErrToString(parsed.w_err) << "(" << parsed.w_err << ")";
    } else {
        oss << "dw_err=" << simcore::RunToBpOutcomeToString(parsed.dw_err) << "(" << parsed.dw_err << ")";
    }
    oss << " w_err=" << simcore::WErrToString(parsed.w_err) << "(" << parsed.w_err << ")"
        << " dw_err=" << simcore::RunToBpOutcomeToString(parsed.dw_err) << "(" << parsed.dw_err << ")";
    return oss.str();
}

std::string FormatProgressDetails(
    std::int64_t job_set_id,
    const simcore::db::execution::workflow::ExecutionJobSetProgressDetails& row) {
    const std::int64_t total = row.total_jobs;
    const std::int64_t done = row.completed_jobs;
    const std::int64_t ok = row.succeeded_jobs;
    const std::int64_t fail = row.failed_jobs;
    const std::int64_t can = row.canceled_jobs;
    const std::int64_t remaining = std::max<std::int64_t>(0, total - ok - fail - can);

    std::ostringstream progress;
    progress << "job_set=" << job_set_id
             << " progress done=" << done << "/" << total;
    if (row.expected_total.has_value()) {
        progress << " expected_total=" << *row.expected_total;
    }
    progress << " ok=" << ok
             << " fail=" << fail
             << " can=" << can
             << " remaining=" << remaining;
    return progress.str();
}

std::vector<std::string> BuildNewFailedStepEventLines(
    const simcore::db::execution::workflow::WorkflowGraphSnapshot& graph,
    std::unordered_set<std::int64_t>* emitted_failed_step_ids) {
    std::vector<std::string> lines;
    if (emitted_failed_step_ids == nullptr) {
        return lines;
    }

    for (const auto& step : graph.steps) {
        if (step.state != simcore::db::execution::workflow::WorkflowStepState::Failed) {
            continue;
        }
        if (!emitted_failed_step_ids->insert(step.workflow_step_id).second) {
            continue;
        }

        std::ostringstream oss;
        oss << "[seedprobe-step-failed] key=" << step.step_key
            << " kind=" << step.step_kind
            << " workflow_step_id=" << step.workflow_step_id;
        if (step.job_set_id.has_value()) {
            oss << " job_set=" << *step.job_set_id;
        }
        if (step.blocked_reason.has_value() && !step.blocked_reason->empty()) {
            oss << " reason=" << *step.blocked_reason;
        }
        lines.push_back(oss.str());
    }
    return lines;
}

std::vector<std::string> BuildNewMaterializedStepEventLines(
    simcore::db::execution::workflow::SqliteExecutionDb* execution_db,
    const simcore::db::execution::workflow::WorkflowGraphSnapshot& graph,
    std::unordered_set<std::int64_t>* emitted_materialized_step_ids) {
    std::vector<std::string> lines;
    if (execution_db == nullptr || emitted_materialized_step_ids == nullptr) {
        return lines;
    }

    for (const auto& step : graph.steps) {
        if (!step.job_set_id.has_value()) {
            continue;
        }
        if (step.state != simcore::db::execution::workflow::WorkflowStepState::Materialized
            && step.state != simcore::db::execution::workflow::WorkflowStepState::Running
            && step.state != simcore::db::execution::workflow::WorkflowStepState::Completed) {
            continue;
        }
        if (!emitted_materialized_step_ids->insert(step.workflow_step_id).second) {
            continue;
        }

        const auto job_set_id = *step.job_set_id;
        const auto details = execution_db->GetJobSetProgress(job_set_id);
        std::ostringstream oss;
        oss << "[seedprobe-step-materialized] key=" << step.step_key
            << " kind=" << step.step_kind
            << " workflow_step_id=" << step.workflow_step_id
            << " state=" << ToString(step.state)
            << " job_set=" << job_set_id;
        if (details.has_value()) {
            oss << " total=" << details->total_jobs
                << " done=" << details->completed_jobs
                << " ok=" << details->succeeded_jobs
                << " fail=" << details->failed_jobs
                << " can=" << details->canceled_jobs;
            if (details->expected_total.has_value()) {
                oss << " expected_total=" << *details->expected_total;
            }
        } else {
            oss << " progress=unavailable";
        }

        const auto child_rows = execution_db->GetChildJobSetProgress(job_set_id);
        if (!child_rows.empty()) {
            std::int64_t child_total = 0;
            std::int64_t child_expected = 0;
            for (const auto& child : child_rows) {
                child_total += child.total_jobs;
                child_expected += child.expected_total.value_or(0);
            }
            oss << " child_job_sets=" << child_rows.size()
                << " child_total=" << child_total
                << " child_expected_total=" << child_expected;
        }
        lines.push_back(oss.str());
    }
    return lines;
}

std::string FormatCoordinatorTelemetryLine(const WorkflowCoordinatorTelemetry& telemetry, size_t active_workers) {
    std::ostringstream oss;
    oss << "workers=" << active_workers
        << " dispatch=" << telemetry.dispatch_success_count
        << " dispatch_miss=" << telemetry.dispatch_miss_count
        << " progress_batches=" << telemetry.progress_batch_count
        << " results_received=" << telemetry.results_received_count;
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
    const auto select_step = [&](WorkflowStepState target) -> const simcore::db::execution::workflow::WorkflowStepRecord* {
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
    const auto details = execution_db != nullptr ? execution_db->GetJobSetProgress(job_set_id) : std::nullopt;
    if (!details.has_value()) {
        return { step_label.str(), "job_set progress unavailable" };
    }

    std::vector<std::string> lines{ step_label.str(), FormatProgressDetails(job_set_id, *details) };
    const auto child_rows = execution_db->GetChildJobSetProgress(job_set_id);
    if (!child_rows.empty()) {
        std::size_t active_children = 0;
        const simcore::db::execution::workflow::ExecutionChildJobSetProgressDetails* selected_child = nullptr;
        for (const auto& child : child_rows) {
            if (child.completed_jobs < child.total_jobs) {
                ++active_children;
                if (selected_child == nullptr) {
                    selected_child = &child;
                }
            }
        }

        std::ostringstream child_rollup;
        child_rollup << "child_job_sets=" << child_rows.size()
                     << " active=" << active_children;
        lines.push_back(child_rollup.str());

        if (selected_child != nullptr) {
            const auto child_remaining = std::max<std::int64_t>(0, selected_child->total_jobs - selected_child->completed_jobs);
            std::ostringstream child;
            child << "active_child_job_set=" << selected_child->job_set_id;
            if (selected_child->expected_delta.has_value()) {
                child << " expected_delta=" << *selected_child->expected_delta;
            }
            child << " done=" << selected_child->completed_jobs << "/" << selected_child->total_jobs
                  << " ok=" << selected_child->succeeded_jobs
                  << " fail=" << selected_child->failed_jobs
                  << " can=" << selected_child->canceled_jobs
                  << " remaining=" << child_remaining;
            lines.push_back(child.str());
        }
    }
    return lines;
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

    simcore::db::execution::programdb::ProgramKindRegistry program_kind_registry;
    simcore::db::execution::programdb::seedprobe::SeedProbePhaseRegistrationConfig phase_config{};
    phase_config.authoring_db = db_service->AuthoringDb();
    simcore::db::execution::programdb::seedprobe::RegisterSeedProbePhaseDescriptors(
        &program_kind_registry,
        execution_db,
        db_service->AnalysisDb(),
        std::move(phase_config));

    DBWorkflowWorkerCoordinator coordinator = BuildDbBackedWorkflowCoordinator(
        execution_db,
        db_service->StateDb(),
        DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 5,
            .controller_sleep_ms = static_cast<uint32_t>(options.poll_ms),
            .worker_exe_path = worker_exe.string(),
            .iso_path = options.iso_path.string(),
            .dolphin_base_dir = options.dolphin_base_dir.string(),
            .worker_dir_root = options.worker_dir_root.value_or(
                std::filesystem::temp_directory_path() / "simcoredbe2e-workers").string(),
        },
        CoordinatorIntegrationConfig{},
        &program_kind_registry);

    std::mutex event_lines_mtx;
    std::deque<std::string> pending_event_lines;
    std::mutex seen_result_mtx;
    std::unordered_map<std::uint64_t, simcore::PRResult> seen_results_by_job_id;
    auto enqueue_event_line = [&](std::string line) {
        std::lock_guard<std::mutex> lock(event_lines_mtx);
        pending_event_lines.push_back(std::move(line));
    };
    auto drain_event_lines = [&]() {
        std::vector<std::string> lines;
        std::lock_guard<std::mutex> lock(event_lines_mtx);
        while (!pending_event_lines.empty()) {
            lines.push_back(std::move(pending_event_lines.front()));
            pending_event_lines.pop_front();
        }
        return lines;
    };
    coordinator.SetResultCallback([&](const simcore::PRResult& result) {
        {
            std::lock_guard<std::mutex> lock(seen_result_mtx);
            const auto [it, inserted] = seen_results_by_job_id.try_emplace(result.job_id, result);
            if (!inserted) {
                std::ostringstream duplicate;
                duplicate << "[seedprobe-result-duplicate] job=" << result.job_id
                          << " first_worker=" << it->second.worker_id
                          << " current_worker=" << result.worker_id;
                enqueue_event_line(duplicate.str());
            }
        }
        enqueue_event_line(FormatSeedProbeResultEventLine(result));
    });
    coordinator.SetResultMapEventCallback([&](const std::string& line) {
        enqueue_event_line(line);
    });

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
    std::unordered_set<std::int64_t> emitted_failed_step_ids;
    std::unordered_set<std::int64_t> emitted_materialized_step_ids;
    std::vector<std::string> latest_lines;
    while (std::chrono::steady_clock::now() - started < std::chrono::milliseconds(timeout_ms)) {
        (void)ui_read_db->ListProjectionSubscriptions("Execution", "exec_outbox_message");
        ++poll_count;
        ++ticks_since_snapshot;

        const auto telemetry = coordinator.SnapshotTelemetry();
        const auto graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
        latest_lines = BuildProgressLines(execution_db, telemetry, coordinator.SnapshotWorkers(), graph);
        std::vector<std::string> event_lines = drain_event_lines();
        if (graph.has_value()) {
            auto materialized_step_lines = BuildNewMaterializedStepEventLines(
                execution_db,
                *graph,
                &emitted_materialized_step_ids);
            event_lines.insert(
                event_lines.end(),
                std::make_move_iterator(materialized_step_lines.begin()),
                std::make_move_iterator(materialized_step_lines.end()));
            auto failed_step_lines = BuildNewFailedStepEventLines(*graph, &emitted_failed_step_ids);
            event_lines.insert(
                event_lines.end(),
                std::make_move_iterator(failed_step_lines.begin()),
                std::make_move_iterator(failed_step_lines.end()));
        }
        if (interactive_stdout) {
            progress_renderer.SetLines(latest_lines);
            for (const auto& line : event_lines) {
                progress_renderer.WriteEventLine(std::cout, line);
            }
            progress_renderer.Render(std::cout);
        } else {
            for (const auto& line : event_lines) {
                std::cout << line << '\n';
            }
        }
        if (!interactive_stdout && (ticks_since_snapshot >= 10 || poll_count == 1)) {
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
    const auto final_event_lines = drain_event_lines();
    for (const auto& line : final_event_lines) {
        if (interactive_stdout) {
            progress_renderer.WriteEventLine(std::cout, line);
        } else {
            std::cout << line << '\n';
        }
    }

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
    if (final_graph.has_value()) {
        auto final_failed_step_lines = BuildNewFailedStepEventLines(*final_graph, &emitted_failed_step_ids);
        if (interactive_stdout) {
            progress_renderer.SetLines(latest_lines);
            for (const auto& line : final_failed_step_lines) {
                progress_renderer.WriteEventLine(std::cout, line);
            }
        } else {
            for (const auto& line : final_failed_step_lines) {
                std::cout << line << '\n';
            }
        }
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
