#include "TasMovieRealWorkerScenario.h"

#include <chrono>
#include <deque>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbePhaseRegistration.h"
#include "Execution/ProgramDB/TasMovie/TasMoviePhaseRegistration.h"
#include "Phases/Programs/PlayTasMovie/TasMoviePayload.h"
#include "Runner/Parallel/SimCoreDB/DBWorkflowCoordinatorFactory.h"
#include "Runner/Parallel/SimCoreDB/DBWorkflowWorkerCoordinator.h"
#include "Tas/DtmFile.h"
#include "UIRead/IUiReadDb.h"

#include "CoordinatorProgress.h"
#include "DbSetup.h"
#include "DurableLogFile.h"
#include "MultiLineProgressRenderer.h"

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

std::string FormatWorkflowStateLine(const simcore::db::execution::workflow::WorkflowGraphSnapshot& graph) {
    std::size_t completed = 0;
    for (const auto& step : graph.steps) {
        if (step.state == simcore::db::execution::workflow::WorkflowStepState::Completed) {
            ++completed;
        }
    }
    std::ostringstream oss;
    oss << "workflow=" << ToString(graph.instance.state)
        << " steps=" << completed << "/" << graph.steps.size();
    return oss.str();
}

std::int64_t ComputeTasMovieRunMs(
    const std::filesystem::path& dtm_file,
    std::uint8_t headroom_x10,
    std::int64_t fallback_ms) {
    simcore::tas::DtmFile dtm;
    if (!dtm.load(dtm_file.string())) {
        return fallback_ms;
    }
    const auto info = dtm.info();
    const auto run_ms = simcore::tasmovie::compute_run_ms_from_counts(
        info.vi_count,
        info.input_count,
        static_cast<double>(headroom_x10) / 10.0);
    return run_ms > 0 ? static_cast<std::int64_t>(run_ms) : fallback_ms;
}

std::int64_t ComputeTasMovieScenarioTimeoutMs(
    const CliOptions& options,
    bool chain_seedprobe,
    std::uint8_t headroom_x10) {
    const auto tas_movie_run_ms = ComputeTasMovieRunMs(
        options.dtm_file,
        headroom_x10,
        options.timeout_ms * 2);
    const auto tas_budget_ms = tas_movie_run_ms + options.timeout_ms;
    if (!chain_seedprobe) {
        return tas_budget_ms;
    }
    constexpr std::int64_t kSeedProbeAverageUniqueCountEstimate = 25;
    const std::int64_t grid_probe_count =
        static_cast<std::int64_t>(kSeedProbeSamplesPerAxis)
        * static_cast<std::int64_t>(kSeedProbeSamplesPerAxis)
        * 3;
    const auto seedprobe_budget_ms =
        options.timeout_ms * (1 + grid_probe_count + kSeedProbeAverageUniqueCountEstimate);
    return tas_budget_ms + seedprobe_budget_ms;
}

bool RefreshSeedProbeUiReadProjection(
    simcore::db::IAnalysisDb* analysis_db,
    simcore::db::IUiReadDb* ui_read_db,
    std::int64_t probe_run_id,
    std::string* error_out) {
    if (analysis_db == nullptr || ui_read_db == nullptr || probe_run_id <= 0) {
        if (error_out) *error_out = "analysis/ui read db unavailable";
        return false;
    }
    const auto run = analysis_db->GetSeedProbeRun(probe_run_id);
    if (!run.has_value()) {
        if (error_out) *error_out = "seed probe run not found";
        return false;
    }
    const auto neutral = analysis_db->LookupSeedProbeNeutralSeed(probe_run_id);
    const auto grid_rows = analysis_db->ListSeedProbeGridSeeds(probe_run_id);
    const auto unique_rows = analysis_db->ListSeedProbeUniqueSeeds(probe_run_id);

    simcore::db::UiSeedProbeRunSummary summary{};
    summary.probe_run_id = run->probe_run_id;
    summary.probe_set_id = run->probe_set_id;
    summary.entry_savestate_id = run->entry_savestate_id;
    summary.seed_probe_spec_id = run->seed_probe_spec_id;
    summary.codec_version = run->codec_version;
    summary.status = run->status;
    summary.neutral_seed_value = neutral;
    summary.grid_count = static_cast<int>(grid_rows.size());
    summary.unique_count = static_cast<int>(unique_rows.size());
    summary.requested_at_utc = run->requested_at_utc.time_since_epoch().count();
    if (run->completed_at_utc.has_value()) {
        summary.completed_at_utc = run->completed_at_utc->time_since_epoch().count();
    }
    if (!ui_read_db->UpsertSeedProbeRunSummary(summary, error_out)) {
        return false;
    }

    std::vector<simcore::db::UiSeedProbeDeltaPoint> points;
    points.reserve(grid_rows.size());
    for (const auto& row : grid_rows) {
        points.push_back(
            {
                .delta_point_id = row.grid_seed_id,
                .probe_run_id = probe_run_id,
                .source_family = row.source_family,
                .axis_x = row.axis_x,
                .axis_y = row.axis_y,
                .seed_value = row.seed_value,
                .seed_delta = row.seed_delta,
            });
    }
    if (!ui_read_db->ReplaceSeedProbeDeltaPoints(probe_run_id, points, error_out)) {
        return false;
    }

    std::vector<simcore::db::UiSeedProbeUniqueValue> values;
    values.reserve(unique_rows.size());
    for (const auto& row : unique_rows) {
        values.push_back(
            {
                .unique_value_id = row.unique_seed_id,
                .probe_run_id = probe_run_id,
                .seed_value = row.seed_value,
                .seed_delta = row.seed_delta,
                .main_x = row.main_x,
                .main_y = row.main_y,
                .cstick_x = row.cstick_x,
                .cstick_y = row.cstick_y,
                .trigger_x = row.trigger_x,
                .trigger_y = row.trigger_y,
            });
    }
    return ui_read_db->ReplaceSeedProbeUniqueValues(probe_run_id, values, error_out);
}

std::int64_t ResolveSeedProbeRunIdFromGraph(
    const std::optional<simcore::db::execution::workflow::WorkflowGraphSnapshot>& graph,
    std::int64_t current_probe_run_id) {
    if (current_probe_run_id > 0 || !graph.has_value()) {
        return current_probe_run_id;
    }

    for (const auto& step : graph->steps) {
        if ((step.step_key == "probe_1" || step.step_kind == "seed_probe_chain")
            && step.input_ref_kind.has_value()
            && *step.input_ref_kind == "sp_probe_run"
            && step.input_ref_id.has_value()
            && *step.input_ref_id > 0) {
            return *step.input_ref_id;
        }
    }
    return 0;
}

void PrintSeedProbeUiReadLine(simcore::db::IUiReadDb* ui_read_db, std::int64_t probe_run_id) {
    if (ui_read_db == nullptr || probe_run_id <= 0) {
        return;
    }
    const auto summary = ui_read_db->GetSeedProbeRunSummary(probe_run_id);
    if (!summary.has_value()) {
        return;
    }
    const auto deltas = ui_read_db->ListSeedProbeDeltaPoints(probe_run_id);
    const auto uniques = ui_read_db->ListSeedProbeUniqueValues(probe_run_id);
    std::cout << "[tasmovie-seedprobe-uiread] probe_run=" << probe_run_id
              << " entry_savestate_id=" << summary->entry_savestate_id
              << " neutral=" << (summary->neutral_seed_value.has_value() ? std::to_string(*summary->neutral_seed_value) : "null")
              << " grid=" << summary->grid_count
              << " unique=" << summary->unique_count
              << " delta_rows=" << deltas.size()
              << " unique_rows=" << uniques.size()
              << '\n';
}

bool RunTasMovieScenario(
    const CliOptions& options,
    const char* argv0,
    simcore::db::core::DBService* db_service,
    bool chain_seedprobe,
    std::string* error_out) {
    if (db_service == nullptr || !db_service->IsRunning()) {
        if (error_out) *error_out = "db service must be running";
        return false;
    }

    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::exists(worker_exe)) {
        if (error_out) *error_out = "SimCoreWorker.exe was not found next to SimCoreDBe2e: " + worker_exe.string();
        return false;
    }

    std::string err;
    std::int64_t dtm_artifact_id = 0;
    if (!SeedStateDtmArtifact(db_service->StateDb(), options.dtm_file, &dtm_artifact_id, &err)) {
        if (error_out) *error_out = "failed seeding StateDB DTM artifact: " + err;
        return false;
    }

    std::int64_t workflow_instance_id = 0;
    std::int64_t probe_run_id = 0;
    if (chain_seedprobe) {
        std::int64_t seed_probe_spec_id = 0;
        if (!SeedAuthoringSpec(db_service->AuthoringDb(), &seed_probe_spec_id, &err)) {
            if (error_out) *error_out = "failed seeding AuthoringDB seedprobe spec: " + err;
            return false;
        }
        if (!SeedTasMovieSeedProbeWorkflow(
                db_service->AuthoringDb(),
                db_service->ExecutionDb(),
                dtm_artifact_id,
                seed_probe_spec_id,
                &workflow_instance_id,
                &err)) {
            if (error_out) *error_out = "failed seeding chained workflow rows: " + err;
            return false;
        }
    } else if (!SeedTasMovieWorkflow(db_service->AuthoringDb(), db_service->ExecutionDb(), dtm_artifact_id, &workflow_instance_id, &err)) {
        if (error_out) *error_out = "failed seeding TasMovie workflow rows: " + err;
        return false;
    }

    simcore::db::execution::programdb::ProgramKindRegistry registry;
    simcore::db::execution::programdb::tasmovie::TasMoviePhaseRegistrationConfig tas_config{};
    tas_config.authoring_db = db_service->AuthoringDb();
    tas_config.blueprint.base_dtm_artifact_id = dtm_artifact_id;
    tas_config.blueprint.rtc_low = 0;
    tas_config.blueprint.rtc_high = 0;
    tas_config.blueprint.run_ms = 0;
    tas_config.blueprint.vi_stall_ms = 2000;
    tas_config.blueprint.progress_enable = false;
    tas_config.blueprint.headroom_x10 = 35;
    const auto scenario_timeout_ms = ComputeTasMovieScenarioTimeoutMs(
        options,
        chain_seedprobe,
        tas_config.blueprint.headroom_x10);
    tas_config.working_dir_root = options.workspace_root.value_or(std::filesystem::temp_directory_path() / "simcoredbe2e-default") / "tasmovie";
    tas_config.next_step_key = chain_seedprobe ? "Neutral" : "Done";
    simcore::db::execution::programdb::tasmovie::RegisterTasMoviePhaseDescriptor(
        &registry,
        db_service->ExecutionDb(),
        db_service->StateDb(),
        db_service->AnalysisDb(),
        std::move(tas_config));
    if (chain_seedprobe) {
        simcore::db::execution::programdb::seedprobe::SeedProbePhaseRegistrationConfig seed_config{};
        seed_config.authoring_db = db_service->AuthoringDb();
        simcore::db::execution::programdb::seedprobe::RegisterSeedProbePhaseDescriptors(
            &registry,
            db_service->ExecutionDb(),
            db_service->AnalysisDb(),
            std::move(seed_config));
    }

    auto coordinator = simcore::runner::parallel::simcoredb::BuildDbBackedWorkflowCoordinator(
        db_service->ExecutionDb(),
        db_service->StateDb(),
        simcore::runner::parallel::simcoredb::DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = static_cast<std::size_t>(options.worker_count),
            .controller_sleep_ms = static_cast<std::uint32_t>(options.poll_ms),
            .worker_exe_path = worker_exe.string(),
            .iso_path = options.iso_path.string(),
            .dolphin_base_dir = options.dolphin_base_dir.string(),
            .worker_dir_root = options.worker_dir_root.value_or(
                std::filesystem::temp_directory_path() / "simcoredbe2e-workers").string(),
            .visual_workers = options.visual_worker,
            .auto_resume_visual_workers = false,
            .visual_screenshot_dir = options.visual_screenshot_dir.value_or(
                options.workspace_root.value_or(std::filesystem::temp_directory_path() / "simcoredbe2e-default")
                    / "visual-screenshots").string(),
        },
        simcore::runner::parallel::simcoredb::CoordinatorIntegrationConfig{},
        &registry);

    DurableLogFile durable_log;
    if (!durable_log.Open(options, options.scenario, error_out)) {
        return false;
    }
    std::cout << "[durable-log] path=" << durable_log.path().string() << '\n';

    std::mutex lines_mtx;
    std::deque<std::string> pending_lines;
    const auto push_line = [&](std::string line) {
        durable_log.AppendLine(line);
        std::lock_guard<std::mutex> lock(lines_mtx);
        pending_lines.push_back(std::move(line));
    };
    const auto drain_lines = [&]() {
        std::deque<std::string> out;
        std::lock_guard<std::mutex> lock(lines_mtx);
        std::swap(out, pending_lines);
        return out;
    };
    std::mutex progress_mtx;
    WorkerProgressById last_progress_by_worker;

    coordinator.SetResultCallback([&](const simcore::PRResult& result) {
        uint32_t dw_err = 0;
        uint32_t hit_pc = 0;
        uint32_t hit_bp_key = 0;
        uint32_t vi_delta = 0;
        uint32_t vi_last = 0;
        std::string save_path;
        std::string last_savestate_path;
        result.ps.ctx.get(simcore::keys::core::DW_RUN_OUTCOME_CODE, dw_err);
        result.ps.ctx.get(simcore::keys::core::RUN_HIT_PC, hit_pc);
        result.ps.ctx.get(simcore::keys::core::RUN_HIT_BP_KEY, hit_bp_key);
        result.ps.ctx.get(simcore::keys::core::VI_DELTA, vi_delta);
        result.ps.ctx.get(simcore::keys::core::VI_LAST, vi_last);
        result.ps.ctx.get(simcore::keys::tas::SAVE_PATH, save_path);
        result.ps.ctx.get(simcore::keys::core::LAST_SAVESTATE_PATH, last_savestate_path);
        std::ostringstream line;
        line << "[tasmovie-worker-result] job=" << result.job_id
             << " worker=" << result.worker_id
             << " ok=" << (result.ps.ok ? "true" : "false")
             << " w_err=" << simcore::WErrToString(result.ps.w_err) << "(" << static_cast<int>(result.ps.w_err) << ")"
             << " dw_err=" << dw_err
             << " hit_pc=0x" << std::hex << std::uppercase << hit_pc << std::dec
             << " hit_bp_key=" << hit_bp_key
             << " vi_delta=" << vi_delta
             << " vi_last=" << vi_last
             << " save_path=\"" << save_path << "\""
             << " last_savestate_path=\"" << last_savestate_path << "\"";
        if (options.visual_worker) {
            const auto screenshot_path = options.visual_screenshot_dir.value_or(
                options.workspace_root.value_or(std::filesystem::temp_directory_path() / "simcoredbe2e-default")
                    / "visual-screenshots")
                / ("worker-" + std::to_string(result.worker_id)
                    + "-job-" + std::to_string(result.job_id)
                    + "-epoch-" + std::to_string(result.epoch)
                    + ".png");
            line << " visual_screenshot=\"" << screenshot_path.string() << "\"";
        }
        push_line(line.str());
    });
    coordinator.SetResultMapEventCallback([&](const std::string& line) {
        push_line(line);
    });
    coordinator.SetProgressCallback([&](const simcore::PRProgress& progress) {
        std::lock_guard<std::mutex> lock(progress_mtx);
        last_progress_by_worker[progress.worker_id] = progress;
    });

    coordinator.Start();
    const auto started = std::chrono::steady_clock::now();
    const bool interactive_stdout = IsInteractiveStdout();
    MultiLineProgressRenderer progress_renderer;
    bool completed = false;
    bool failed = false;
    std::string latest_state = "workflow=unavailable";
    std::vector<std::string> latest_lines;
    std::size_t poll_count = 0;
    std::size_t ticks_since_snapshot = 0;
    while (std::chrono::steady_clock::now() - started < std::chrono::milliseconds(scenario_timeout_ms)) {
        ++poll_count;
        ++ticks_since_snapshot;
        const auto event_lines = drain_lines();
        const auto graph = db_service->ExecutionDb()->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
        WorkerProgressById progress_snapshot;
        {
            std::lock_guard<std::mutex> lock(progress_mtx);
            progress_snapshot = last_progress_by_worker;
        }
        latest_lines = BuildCoordinatorProgressLines(
            db_service->ExecutionDb(),
            coordinator.SnapshotTelemetry(),
            coordinator.SnapshotWorkers(),
            graph,
            &progress_snapshot);
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
            if (ticks_since_snapshot >= 10 || poll_count == 1) {
                ticks_since_snapshot = 0;
                std::cout << "[tasmovie] ";
                for (std::size_t i = 0; i < latest_lines.size(); ++i) {
                    if (i > 0) {
                        std::cout << " | ";
                    }
                    std::cout << latest_lines[i];
                }
                std::cout << '\n';
            }
        }
        if (graph.has_value()) {
            probe_run_id = ResolveSeedProbeRunIdFromGraph(graph, probe_run_id);
            latest_state = FormatWorkflowStateLine(*graph);
            using simcore::db::execution::workflow::WorkflowInstanceState;
            if (graph->instance.state == WorkflowInstanceState::Completed) {
                completed = true;
                break;
            }
            if (graph->instance.state == WorkflowInstanceState::Failed || graph->instance.state == WorkflowInstanceState::Canceled) {
                failed = true;
                break;
            }
            if (AreWorkflowStepsTerminal(*graph)) {
                if (HasFailedWorkflowStep(*graph)) {
                    failed = true;
                } else {
                    completed = true;
                }
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
    }
    coordinator.Stop();
    for (const auto& line : drain_lines()) {
        if (interactive_stdout) {
            progress_renderer.WriteEventLine(std::cout, line);
        } else {
            std::cout << line << '\n';
        }
    }
    const auto final_graph = db_service->ExecutionDb()->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    WorkerProgressById final_progress_snapshot;
    {
        std::lock_guard<std::mutex> lock(progress_mtx);
        final_progress_snapshot = last_progress_by_worker;
    }
    latest_lines = BuildCoordinatorProgressLines(
        db_service->ExecutionDb(),
        coordinator.SnapshotTelemetry(),
        coordinator.SnapshotWorkers(),
        final_graph,
        &final_progress_snapshot);
    if (final_graph.has_value()) {
        probe_run_id = ResolveSeedProbeRunIdFromGraph(final_graph, probe_run_id);
        latest_state = FormatWorkflowStateLine(*final_graph);
    }
    if (interactive_stdout) {
        progress_renderer.SetLines(latest_lines);
        progress_renderer.Render(std::cout);
    }

    if (chain_seedprobe && db_service->UiReadDb() != nullptr) {
        if (RefreshSeedProbeUiReadProjection(db_service->AnalysisDb(), db_service->UiReadDb(), probe_run_id, &err)) {
            PrintSeedProbeUiReadLine(db_service->UiReadDb(), probe_run_id);
        }
    }

    std::cout << "[tasmovie-final] status=" << (completed ? "success" : failed ? "failure" : "timeout") << '\n';
    std::cout << "  timeout_ms=" << scenario_timeout_ms << '\n';
    std::cout << "  " << latest_state << '\n';

    if (!completed) {
        if (error_out) *error_out = failed
            ? "workflow did not complete successfully"
            : "workflow did not reach COMPLETED state before timeout";
        return false;
    }
    return true;
}

} // namespace

bool RunTasMovieRealWorkerSmoke(
    const CliOptions& options,
    const char* argv0,
    simcore::db::core::DBService* db_service,
    std::string* error_out) {
    return RunTasMovieScenario(options, argv0, db_service, false, error_out);
}

bool RunTasMovieSeedProbeRealWorkerSmoke(
    const CliOptions& options,
    const char* argv0,
    simcore::db::core::DBService* db_service,
    std::string* error_out) {
    return RunTasMovieScenario(options, argv0, db_service, true, error_out);
}

} // namespace simcore::e2e
