#include "BattleJobBatchWorkerRun.h"

#include "BattleJobBatchRunManifest.h"
#include "BattleJobClone.h"
#include "CaptureArtifact.h"
#include "CaptureProfileJson.h"
#include "CheckpointTrace.h"
#include "CliResourceInputCompatibility.h"
#include "LiveCaptureProfile.h"
#include "ProbeCpuCoreEnvironment.h"

#include "Common/DbConfigPaths.h"
#include "Common/DbService.h"
#include "Common/Migrations/MigrationRunner.h"
#include "DbRootCopy.h"
#include "Execution/DBWorkflowCoordinatorFactory.h"
#include "Execution/ProgramDB/BattleSingleTurn/BattleSingleTurnPhaseRegistration.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/Workflow/SqliteExecutionDb.h"
#include "Utils/Hash.h"
#include "Worker/WorkerCapabilityPreflight.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <thread>
#include <utility>

#ifdef GetJob
#undef GetJob
#endif

namespace savor::predict {
namespace {

bool is_terminal_state(const std::string& state) {
    return state == "SUCCEEDED"
        || state == "SUCCEEDED_WINNER"
        || state == "SUCCEEDED_DUPLICATE"
        || state == "FAILED"
        || state == "CANCELED"
        || state == "SUPERSEDED";
}

savor::db::DbConfigPaths db_paths_for_root(const std::filesystem::path& root) {
    savor::db::DbConfigPaths paths{};
    paths.execution_db_path = root / "execution.db";
    paths.state_db_path = root / "state.db";
    paths.analysis_db_path = root / "analysis.db";
    paths.authoring_db_path = root / "authoring.db";
    paths.ui_read_db_path = root / "ui_read.db";
    paths.archive_db_path = root / "archive.db";
    paths.object_store_root = root / "object_store";
    paths.archive_store_root = root / "archive_store";
    return paths;
}

bool write_or_copy_capture_profile(
    const BattleJobBatchRunOptions& options,
    const std::filesystem::path& output_path,
    std::ostream& err) {
    std::error_code ec;
    std::filesystem::create_directories(output_path.parent_path(), ec);
    if (ec) {
        err << "Failed creating capture profile directory: " << ec.message() << "\n";
        return false;
    }
    std::string text;
    if (options.probe_mode == ProbeMode::ControlOnly) {
        text = R"({"schema":"savor.capture.profile/1","name":"savor_control_only","revision":1,"battle_progress_enabled":false,"probes":[]})";
    } else if (!options.capture_profile_path.empty()) {
        std::ifstream source(options.capture_profile_path, std::ios::binary);
        if (!source) {
            err << "Failed opening capture profile " << options.capture_profile_path.string() << "\n";
            return false;
        }
        text.assign(std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>());
    } else if (options.probe_mode == ProbeMode::Capture) {
        text = build_first_battle_capture_profile_ini();
    } else {
        err << "Internal error: progress-only mode must not materialize a profile.\n";
        return false;
    }
    try {
        text = pin_capture_profile_module_hash(
            text,
            hash::sha256_of_file(options.worker_exe_path.string()));
    } catch (const std::exception& ex) {
        err << "Failed pinning capture profile to worker module: " << ex.what() << "\n";
        return false;
    }

    std::ofstream profile(output_path, std::ios::binary | std::ios::trunc);
    if (!profile.is_open()) {
        err << "Failed opening capture profile output: " << output_path.string() << "\n";
        return false;
    }
    profile.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!profile.good()) {
        err << "Failed writing capture profile: " << output_path.string() << "\n";
        return false;
    }
    return true;
}

bool check_runtime_paths(const BattleJobBatchRunOptions& options, std::ostream& err) {
    std::error_code ec;
    if (!std::filesystem::exists(options.iso_path, ec)) {
        err << "ISO path does not exist: " << options.iso_path.string() << "\n";
        return false;
    }
    if (!std::filesystem::is_directory(options.dolphin_base_dir, ec)) {
        err << "Dolphin base directory does not exist: " << options.dolphin_base_dir.string() << "\n";
        return false;
    }
    if (!std::filesystem::exists(options.worker_exe_path, ec)) {
        err << "SavorWorker executable does not exist: " << options.worker_exe_path.string() << "\n";
        return false;
    }
    if (!options.capture_profile_path.empty() && !std::filesystem::exists(options.capture_profile_path, ec)) {
        err << "Capture profile does not exist: " << options.capture_profile_path.string() << "\n";
        return false;
    }
    return true;
}

void append_error(BattleJobBatchRunSummary& summary, const std::string& message) {
    summary.errors.push_back(message);
}

void append_error(BattleJobBatchRunJobSummary& summary, const std::string& message) {
    summary.errors.push_back(message);
}

void write_manifest_outputs(BattleJobBatchRunSummary& summary, std::ostream& err) {
    const auto manifest_path = summary.sandbox.run_root / "manifest.json";
    const auto summary_path = summary.sandbox.run_root / "summary.txt";
    const auto csv_path = summary.sandbox.run_root / "summary.csv";
    (void)write_battle_job_batch_run_manifest(summary, manifest_path, err);
    (void)write_battle_job_batch_run_text_summary(summary, summary_path, err);
    (void)write_battle_job_batch_run_csv_summary(summary, csv_path, err);
}

int prepare_battle_job_batch_sandbox(
    const BattleJobBatchRunOptions& options,
    BattleJobSandboxResult* result_out,
    std::ostream& out,
    std::ostream& err) {
    if (result_out == nullptr) {
        err << "Internal error: sandbox result output is null.\n";
        return 1;
    }
    const auto run_root = options.run_root;
    if (run_root.empty()) {
        err << "run-battle-jobs requires a run root.\n";
        return 2;
    }

    std::error_code ec;
    std::filesystem::create_directories(run_root, ec);
    if (ec) {
        err << "Failed to create run root " << run_root.string() << ": " << ec.message() << "\n";
        return 1;
    }

    const auto sandbox_db_root = run_root / "db";
    if (std::filesystem::exists(sandbox_db_root, ec)) {
        err << "Sandbox DB already exists: " << sandbox_db_root.string() << "\n";
        err << "Choose a fresh --run-root so the analysis DB is never mutated in place.\n";
        return 1;
    }

    result_out->run_root = run_root;
    result_out->db_root = sandbox_db_root;
    result_out->sandbox_mode = options.sandbox_mode;

    if (options.sandbox_mode == savor::dbutils::SandboxMode::FullCopy) {
        return savor::dbutils::CopyDbRootFull(
            {
                .source_root = options.db_root,
                .dest_root = sandbox_db_root,
                .overwrite = false,
            },
            out,
            err);
    }

    const auto source_exec_job_ids = unique_battle_job_batch_source_exec_job_ids(options);
    std::vector<savor::dbutils::BattleJobSelector> selectors;
    selectors.reserve(source_exec_job_ids.size());
    for (const auto exec_job_id : source_exec_job_ids) {
        selectors.push_back({
            .turn_job_id = std::nullopt,
            .exec_job_id = exec_job_id,
        });
    }

    savor::dbutils::BattleSingleTurnJobSubsetsResult subsets;
    const int rc = savor::dbutils::HydrateBattleSingleTurnJobSubsets(
        {
            .source_root = options.db_root,
            .target_root = sandbox_db_root,
            .artifact_root = run_root / "source-artifacts",
            .selectors = selectors,
            .overwrite_target = false,
        },
        &subsets,
        out,
        err);
    result_out->table_counts = std::move(subsets.table_counts);
    result_out->copied_artifacts = std::move(subsets.copied_artifacts);
    result_out->validation_errors = std::move(subsets.validation_errors);
    return rc;
}

BattleJobBatchRunJobSummary make_job_summary(
    const BattleJobCloneResult& clone,
    const std::filesystem::path& working_root,
    const std::filesystem::path& run_root) {
    BattleJobBatchRunJobSummary summary;
    summary.clone = clone;
    summary.expected_capture_path = working_root
        / ("job-" + std::to_string(clone.cloned_exec_job_id))
        / "battle_capture.scap";
    summary.stable_capture_path = run_root / "captures"
        / ("job-" + std::to_string(clone.cloned_exec_job_id) + ".scap");
    summary.capture_export_path = run_root / "exports"
        / ("job-" + std::to_string(clone.cloned_exec_job_id) + ".jsonl");
    summary.trace_report_path = run_root / "traces"
        / ("job-" + std::to_string(clone.cloned_exec_job_id) + ".txt");
    return summary;
}

bool all_jobs_terminal(
    savor::db::IExecutionDb* execution_db,
    std::vector<BattleJobBatchRunJobSummary>* jobs) {
    bool all_terminal = true;
    for (auto& job_summary : *jobs) {
        const auto job = execution_db->GetJob(job_summary.clone.cloned_exec_job_id);
        if (job.has_value()) {
            job_summary.terminal_state = job->state;
            if (!is_terminal_state(job->state)) {
                all_terminal = false;
            }
        } else {
            all_terminal = false;
        }
    }
    return all_terminal;
}

void collect_capture_and_trace_artifacts(
    BattleJobBatchRunSummary* summary,
    savor::db::core::DBService* db_service,
    std::ostream& err) {
    std::error_code ec;
    if (summary->options.probe_mode != ProbeMode::Capture) {
        for (auto& job : summary->jobs) {
            if (std::filesystem::exists(job.expected_capture_path, ec)) {
                append_error(job, "non-capture probe mode unexpectedly produced a .scap");
            }
            job.expected_capture_path.clear();
            job.stable_capture_path.clear();
            job.capture_export_path.clear();
            job.trace_report_path.clear();
        }
        return;
    }
    std::filesystem::create_directories(summary->sandbox.run_root / "captures", ec);
    if (ec) {
        append_error(*summary, "failed creating captures directory: " + ec.message());
    }
    ec.clear();
    std::filesystem::create_directories(summary->sandbox.run_root / "traces", ec);
    if (ec) {
        append_error(*summary, "failed creating traces directory: " + ec.message());
    }

    for (auto& job : summary->jobs) {
        ec.clear();
        job.capture_found = std::filesystem::exists(job.expected_capture_path, ec);
        if (!job.capture_found) {
            append_error(job, "capture .scap was not produced at expected path");
            continue;
        }

        std::vector<std::filesystem::path> copied_segments;
        std::string copy_error;
        if (!copy_capture_segments(
                job.expected_capture_path,
                job.stable_capture_path,
                &copied_segments,
                &copy_error)) {
            append_error(job, copy_error);
            continue;
        }
        PreparedCaptureArtifact capture;
        std::string capture_error;
        if (!prepare_capture_artifact(
                job.stable_capture_path,
                job.capture_export_path,
                &capture,
                &capture_error)) {
            append_error(job, capture_error);
            continue;
        }
        job.capture_artifact = capture;
        if (!capture.complete) {
            append_error(job, "capture session is incomplete: " + capture.incomplete_reason);
        }
        job.captured_original_seed = capture.seed_override.original_seed;
        job.captured_override_seed = capture.seed_override.requested_seed;
        job.captured_applied_seed = capture.seed_override.applied_seed;
        job.captured_seed_readback_matches = capture.seed_override.readback_matches;

        std::ofstream trace(job.trace_report_path, std::ios::binary | std::ios::trunc);
        if (!trace.is_open()) {
            append_error(job, "failed opening trace report");
            continue;
        }

        TraceCheckpointsOptions trace_options;
        trace_options.db_root = summary->sandbox.db_root;
        trace_options.checkpoint_file = job.capture_export_path;
        trace_options.exec_job_id = job.clone.cloned_exec_job_id;
        trace_options.action_view_std_json_dir =
            summary->options.action_view_std_json_dir;
        trace_options.disc_dump_root = summary->options.disc_dump_root;
        trace_options.spice_file_parsing_exe = summary->options.spice_file_parsing_exe;
        trace_options.resource_inputs = summary->options.resource_inputs;
        job.trace_exit_code = run_trace_checkpoints(trace_options, trace, err);
        if (job.trace_exit_code != 0) {
            append_error(job, "trace-checkpoints failed with exit code " + std::to_string(job.trace_exit_code));
        }
    }

    (void)db_service;
}

bool has_job_errors(const BattleJobBatchRunSummary& summary) {
    if (!summary.errors.empty()) {
        return true;
    }
    for (const auto& job : summary.jobs) {
        if (!job.errors.empty()) {
            return true;
        }
    }
    return false;
}

} // namespace

int run_battle_jobs(
    const BattleJobBatchRunOptions& supplied,
    std::ostream& out,
    std::ostream& err) {
    auto options = supplied;
    const auto validation_errors = validate_battle_job_batch_run_options(options);
    if (!validation_errors.empty()) {
        for (const auto& error : validation_errors) {
            err << error << "\n";
        }
        return 2;
    }
    if (!check_runtime_paths(options, err)) {
        return 1;
    }
    if (!cli_detail::ensure_first_battle_resource_inputs(
            options.disc_dump_root,
            options.action_view_std_json_dir,
            options.resource_inputs,
            err)) {
        return 1;
    }

    BattleJobBatchRunSummary summary{};
    summary.options = options;
    summary.resource_inputs = options.resource_inputs;
    const auto run_requests = resolved_battle_job_batch_requests(options);

    const auto worker_preflight = savor::RunWorkerCapabilityPreflight(
        savor::WorkerCapabilityPreflightRequest{
            .worker_exe_path = options.worker_exe_path.string(),
            .timeout_ms = 10000,
            .required_capabilities = savor::runtime::CapabilityMask(
                savor::runtime::WorkerCapability::WorksetDispatch),
            .require_complete_exact_catalog = true,
        });
    if (!worker_preflight) {
        err << worker_preflight.message << "\n";
        return 1;
    }

    if (const int rc = prepare_battle_job_batch_sandbox(options, &summary.sandbox, out, err); rc != 0) {
        append_error(summary, "failed preparing battle job sandbox");
        if (!summary.sandbox.run_root.empty()) {
            write_manifest_outputs(summary, err);
        }
        return rc;
    }

    if (options.probe_mode != ProbeMode::ProgressOnly) {
        summary.capture_profile_path = summary.sandbox.run_root / "capture_profile.json";
    }
    if (options.probe_mode != ProbeMode::ProgressOnly
        && !write_or_copy_capture_profile(options, summary.capture_profile_path, err)) {
        append_error(summary, "failed to prepare capture profile");
        write_manifest_outputs(summary, err);
        return 1;
    }

    auto db_service = std::make_unique<savor::db::core::DBService>(
        db_paths_for_root(summary.sandbox.db_root),
        savor::db::migrations::MigrationSourceOptions{ .source_kind = savor::db::migrations::MigrationSourceKind::Embedded });
    std::string db_error;
    if (!db_service->Start(&db_error)) {
        err << "Failed starting sandbox DB service: " << db_error << "\n";
        append_error(summary, "failed starting sandbox DB service: " + db_error);
        write_manifest_outputs(summary, err);
        return 1;
    }

    std::vector<BattleJobCloneRequest> clone_requests;
    clone_requests.reserve(run_requests.size());
    for (const auto& request : run_requests) {
        clone_requests.push_back({
            .source_exec_job_id = request.exec_job_id,
            .override_start_rng_seed = request.override_start_rng_seed,
            .override_fake_attacks_this_turn = request.override_fake_attacks_this_turn,
        });
    }
    if (!clone_battle_jobs_for_capture(
            *db_service,
            clone_requests,
            summary.capture_profile_path,
            &summary.clone,
            err)) {
        append_error(summary, "failed cloning battle jobs for capture");
        write_manifest_outputs(summary, err);
        db_service->Stop();
        return 1;
    }

    const auto working_root = summary.sandbox.run_root / "workflow-runtime" / "battle-single-turn";
    for (const auto& clone : summary.clone.clones) {
        summary.jobs.push_back(make_job_summary(clone, working_root, summary.sandbox.run_root));
    }

    savor::db::execution::programdb::ProgramKindRegistry registry;
    savor::db::execution::programdb::battle::RegisterBattleSingleTurnPhaseDescriptor(
        &registry,
        db_service->ExecutionDb(),
        db_service->StateDb(),
        db_service->AnalysisDb(),
        savor::db::execution::programdb::battle::BattleSingleTurnPhaseRegistrationConfig{
            .authoring_db = db_service->AuthoringDb(),
            .working_dir_root = working_root,
        });

    summary.worker_count = static_cast<int>(std::min<std::size_t>(
        static_cast<std::size_t>(std::max(1, options.max_workers)),
        std::max<std::size_t>(1, summary.clone.clones.size())));

    auto coordinator = savor::runner::parallel::savordb::BuildDbBackedWorkflowCoordinator(
        db_service->ExecutionDb(),
        db_service->StateDb(),
        savor::runner::parallel::savordb::DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = static_cast<std::uint32_t>(summary.worker_count),
            .controller_sleep_ms = static_cast<std::uint32_t>(options.poll_ms),
            .max_concurrent_worker_starts = 1u,
            .worker_exe_path = options.worker_exe_path.string(),
            .iso_path = options.iso_path.string(),
            .dolphin_base_dir = options.dolphin_base_dir.string(),
            .worker_dir_root = (summary.sandbox.run_root / ".worker-runtime").string(),
            .worker_binary_runtime_root = (summary.sandbox.run_root / ".worker-binary-runtime").string(),
            .visual_workers = false,
            .auto_resume_visual_workers = false,
        },
        savor::runner::parallel::savordb::CoordinatorIntegrationConfig{
            .workflow_enabled = false,
        },
        &registry);

    std::mutex events_mutex;
    coordinator.SetProgressCallback([&](const savor::PRProgress& progress) {
        std::lock_guard<std::mutex> lock(events_mutex);
        summary.events.push_back("progress worker=" + std::to_string(progress.worker_id)
            + " job=" + std::to_string(progress.job_id)
            + " text=" + progress.text);
    });
    coordinator.SetResultMapEventCallback([&](const std::string& line) {
        std::lock_guard<std::mutex> lock(events_mutex);
        summary.events.push_back(line);
    });

    ScopedProbeCpuCoreEnvironment cpu_core_environment(options.probe_cpu_core);
    if (!cpu_core_environment.ok()) {
        append_error(summary, "failed configuring probe CPU core environment");
        write_manifest_outputs(summary, err);
        db_service->Stop();
        return 1;
    }

    out << "Running " << summary.clone.clones.size()
        << " cloned battle jobs with " << summary.worker_count
        << " workers in sandbox " << summary.sandbox.db_root.string() << "\n";
    const bool wait_for_initial_worker_pool = options.wait_for_workers_ready
        && summary.worker_count > 1;
    coordinator.SetPaused(wait_for_initial_worker_pool);
    coordinator.Start();

    if (wait_for_initial_worker_pool) {
        const auto ready_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(60);
        std::size_t ready_workers = 0;
        while (std::chrono::steady_clock::now() < ready_deadline) {
            const auto workers = coordinator.SnapshotWorkers();
            ready_workers = static_cast<std::size_t>(std::count_if(
                workers.begin(), workers.end(), [](const WorkerSnapshot& worker) {
                    return worker.pid > 0 && worker.state == WorkerStateKind::Idle;
                }));
            if (ready_workers >= static_cast<std::size_t>(summary.worker_count))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
        }
        {
            std::lock_guard<std::mutex> lock(events_mutex);
            summary.events.push_back(
                "[batch-worker-start-barrier] expected=" + std::to_string(summary.worker_count)
                + " ready=" + std::to_string(ready_workers)
                + " complete=" + (ready_workers >= static_cast<std::size_t>(summary.worker_count)
                    ? "true" : "false"));
        }
        coordinator.SetPaused(false);
    }

    for (;;) {
        if (all_jobs_terminal(db_service->ExecutionDb(), &summary.jobs)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
    }

    coordinator.Stop();
    (void)all_jobs_terminal(db_service->ExecutionDb(), &summary.jobs);

    collect_capture_and_trace_artifacts(&summary, db_service.get(), err);
    write_manifest_outputs(summary, err);
    db_service->Stop();

    out << "Run root: " << summary.sandbox.run_root.string() << "\n";
    out << "Worker count: " << summary.worker_count << "\n";
    for (const auto& job : summary.jobs) {
        out << "Job original_exec=" << job.clone.original_exec_job_id
            << " cloned_exec=" << job.clone.cloned_exec_job_id
            << " state=" << (job.terminal_state.empty() ? "unknown" : job.terminal_state)
            << " capture=" << (options.probe_mode == ProbeMode::Capture
                ? (job.capture_found ? job.stable_capture_path.string() : "missing")
                : "disabled")
            << "\n";
    }

    const bool all_succeeded = std::all_of(summary.jobs.begin(), summary.jobs.end(), [](const BattleJobBatchRunJobSummary& job) {
        return job.terminal_state == "SUCCEEDED";
    });
    if (!all_succeeded || has_job_errors(summary)) {
        return 1;
    }
    return 0;
}

} // namespace savor::predict
