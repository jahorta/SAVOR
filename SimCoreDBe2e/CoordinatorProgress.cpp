#include "CoordinatorProgress.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <sstream>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace simcore::e2e {
namespace {

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

std::string FormatProgressDetails(
    std::int64_t job_set_id,
    const simcore::db::ExecutionJobSetProgressDetails& row) {
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

std::string FormatCoordinatorTelemetryLine(
    const simcore::runner::parallel::simcoredb::WorkflowCoordinatorTelemetry& telemetry,
    std::size_t active_workers) {
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

    const auto completed = counts[static_cast<std::size_t>(simcore::db::execution::workflow::WorkflowStepState::Completed)];
    std::ostringstream oss;
    oss << "workflow=" << ToString(graph.instance.state) << " steps=" << completed << "/" << graph.steps.size()
        << " [WAITING=" << counts[static_cast<std::size_t>(simcore::db::execution::workflow::WorkflowStepState::Waiting)]
        << " READY=" << counts[static_cast<std::size_t>(simcore::db::execution::workflow::WorkflowStepState::Ready)]
        << " MATERIALIZED=" << counts[static_cast<std::size_t>(simcore::db::execution::workflow::WorkflowStepState::Materialized)]
        << " RUNNING=" << counts[static_cast<std::size_t>(simcore::db::execution::workflow::WorkflowStepState::Running)]
        << " COMPLETED=" << completed
        << " FAILED=" << counts[static_cast<std::size_t>(simcore::db::execution::workflow::WorkflowStepState::Failed)]
        << " SKIPPED=" << counts[static_cast<std::size_t>(simcore::db::execution::workflow::WorkflowStepState::Skipped)]
        << "]";
    return oss.str();
}

std::vector<std::string> FormatActiveJobSetLines(
    simcore::db::IExecutionDb* execution_db,
    const simcore::db::execution::workflow::WorkflowGraphSnapshot& graph) {
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
        const simcore::db::ExecutionChildJobSetProgressDetails* selected_child = nullptr;
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

std::string NormalizeProgressText(std::string text) {
    for (auto& ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        }
    }
    return text;
}

std::vector<std::string> BuildWorkerProgressLines(
    const std::vector<WorkerSnapshot>& workers,
    const WorkerProgressById& last_progress_by_worker) {
    std::vector<WorkerSnapshot> sorted_workers = workers;
    std::sort(sorted_workers.begin(), sorted_workers.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.worker_id < rhs.worker_id;
    });

    std::vector<std::string> lines;
    lines.reserve(sorted_workers.size());
    for (const auto& worker : sorted_workers) {
        std::ostringstream oss;
        oss << "worker_progress worker=" << worker.worker_id;
        if (worker.job_id.has_value()) {
            oss << " job=" << *worker.job_id;
        } else {
            oss << " job=none";
        }
        const auto progress_it = last_progress_by_worker.find(static_cast<std::size_t>(worker.worker_id));
        if (progress_it == last_progress_by_worker.end()) {
            oss << " progress_job=none progress=none";
        } else {
            oss << " progress_job=" << progress_it->second.job_id
                << " progress=" << NormalizeProgressText(progress_it->second.text);
        }
        lines.push_back(oss.str());
    }
    return lines;
}

} // namespace

bool IsInteractiveStdout() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return ::isatty(fileno(stdout)) != 0;
#endif
}

std::vector<std::string> BuildCoordinatorProgressLines(
    simcore::db::IExecutionDb* execution_db,
    const simcore::runner::parallel::simcoredb::WorkflowCoordinatorTelemetry& telemetry,
    const std::vector<WorkerSnapshot>& worker_snapshot,
    const std::optional<simcore::db::execution::workflow::WorkflowGraphSnapshot>& graph,
    const WorkerProgressById* last_progress_by_worker) {
    std::vector<std::string> lines;
    lines.push_back("");
    lines.push_back("Coordinator Telemetry");
    lines.push_back("");
    lines.push_back(FormatCoordinatorTelemetryLine(telemetry, worker_snapshot.size()));
    if (CountActiveWorkers(worker_snapshot) > 1) {
        lines.push_back(FormatWorkerRollupLine(worker_snapshot));
    }
    if (!graph.has_value()) {
        lines.push_back("workflow=unavailable");
        lines.push_back("job_set=unavailable");
    } else {
        lines.push_back(FormatWorkflowStateLine(*graph));
        const auto job_lines = FormatActiveJobSetLines(execution_db, *graph);
        lines.insert(lines.end(), job_lines.begin(), job_lines.end());
    }
    if (last_progress_by_worker != nullptr) {
        const auto worker_progress_lines = BuildWorkerProgressLines(worker_snapshot, *last_progress_by_worker);
        lines.insert(lines.end(), worker_progress_lines.begin(), worker_progress_lines.end());
    }
    return lines;
}

} // namespace simcore::e2e
