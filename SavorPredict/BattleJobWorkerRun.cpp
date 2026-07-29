#include "BattleJobWorkerRun.h"

#include "BattleJobClone.h"
#include "BattleJobSandbox.h"
#include "CaptureArtifact.h"
#include "CaptureProfileJson.h"
#include "CheckpointTrace.h"
#include "CliResourceInputCompatibility.h"
#include "LiveCaptureProfile.h"
#include "ProbeCpuCoreEnvironment.h"

#include "Common/DbConfigPaths.h"
#include "Common/DbService.h"
#include "Common/Migrations/MigrationRunner.h"
#include "Execution/DBWorkflowCoordinatorFactory.h"
#include "Execution/ProgramDB/BattleSingleTurn/BattleSingleTurnPhaseRegistration.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/Workflow/SqliteExecutionDb.h"
#include "Utils/Hash.h"
#include "Worker/WorkerCapabilityPreflight.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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
    const BattleJobRunOptions& options,
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

bool check_runtime_paths(const BattleJobRunOptions& options, std::ostream& err) {
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

void append_error(BattleJobRunSummary& summary, const std::string& message) {
    summary.errors.push_back(message);
}

void write_manifest_outputs(BattleJobRunSummary& summary, std::ostream& err) {
    std::string ignored;
    const auto manifest_path = summary.sandbox.run_root / "manifest.json";
    const auto summary_path = summary.sandbox.run_root / "summary.txt";
    if (!write_battle_job_run_manifest(summary, manifest_path, err)) {
        ignored = "failed writing manifest";
    }
    if (!write_battle_job_run_text_summary(summary, summary_path, err)) {
        ignored = "failed writing text summary";
    }
    (void)ignored;
}

} // namespace

int run_battle_job(
    const BattleJobRunOptions& supplied,
    std::ostream& out,
    std::ostream& err) {
    auto options = supplied;
    const auto validation_errors = validate_battle_job_run_options(options);
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

    BattleJobRunSummary summary{};
    summary.options = options;
    summary.resource_inputs = options.resource_inputs;

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

    if (const int rc = prepare_battle_job_sandbox(options, &summary.sandbox, out, err); rc != 0) {
        append_error(summary, "failed preparing battle job sandbox");
        if (!summary.sandbox.run_root.empty()) {
            write_manifest_outputs(summary, err);
        }
        return rc;
    }

    if (options.probe_mode != ProbeMode::ProgressOnly) {
        summary.capture_profile_path = summary.sandbox.run_root / "capture_profile.json";
    }
    if (options.probe_mode == ProbeMode::Capture) {
        summary.stable_capture_path = summary.sandbox.run_root / "capture.scap";
        summary.capture_export_path = summary.sandbox.run_root / "capture.export.jsonl";
        summary.trace_report_path = summary.sandbox.run_root / "trace_checkpoints.txt";
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

    if (!clone_battle_job_for_capture(*db_service, options, summary.capture_profile_path, &summary.clone, err)) {
        append_error(summary, "failed cloning battle job for capture");
        write_manifest_outputs(summary, err);
        db_service->Stop();
        return 1;
    }

    const auto working_root = summary.sandbox.run_root / "workflow-runtime" / "battle-single-turn";
    if (options.probe_mode == ProbeMode::Capture) {
        summary.expected_capture_path = working_root
            / ("job-" + std::to_string(summary.clone.cloned_exec_job_id))
            / "battle_capture.scap";
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

    auto coordinator = savor::runner::parallel::savordb::BuildDbBackedWorkflowCoordinator(
        db_service->ExecutionDb(),
        db_service->StateDb(),
        savor::runner::parallel::savordb::DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 1u,
            .controller_sleep_ms = static_cast<std::uint32_t>(options.poll_ms),
            .worker_silence_in_flight_timeout_ms =
                static_cast<std::uint32_t>(options.timeout_ms),
            .max_concurrent_worker_starts = 1u,
            .worker_exe_path = options.worker_exe_path.string(),
            .iso_path = options.iso_path.string(),
            .dolphin_base_dir = options.dolphin_base_dir.string(),
            .worker_dir_root = (summary.sandbox.run_root / ".worker-runtime").string(),
            .visual_workers = false,
            .auto_resume_visual_workers = false,
        },
        savor::runner::parallel::savordb::CoordinatorIntegrationConfig{
            .workflow_enabled = false,
        },
        &registry);

    coordinator.SetProgressCallback([&](const savor::PRProgress& progress) {
        summary.events.push_back("progress worker=" + std::to_string(progress.worker_id)
            + " job=" + std::to_string(progress.job_id)
            + " text=" + progress.text);
    });
    coordinator.SetResultMapEventCallback([&](const std::string& line) {
        summary.events.push_back(line);
    });

    ScopedProbeCpuCoreEnvironment cpu_core_environment(options.probe_cpu_core);
    if (!cpu_core_environment.ok()) {
        append_error(summary, "failed configuring probe CPU core environment");
        write_manifest_outputs(summary, err);
        db_service->Stop();
        return 1;
    }

    out << "Running cloned battle job " << summary.clone.cloned_exec_job_id
        << " in sandbox " << summary.sandbox.db_root.string() << "\n";
    coordinator.Start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto job = db_service->ExecutionDb()->GetJob(summary.clone.cloned_exec_job_id);
        if (job.has_value()) {
            summary.terminal_state = job->state;
            if (is_terminal_state(job->state)) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
    }
    if (!is_terminal_state(summary.terminal_state)) {
        summary.timed_out = true;
        append_error(summary, "timed out waiting for cloned execution job to finish");
        std::string cancel_error;
        (void)db_service->ExecutionDb()->CancelQueuedOrClaimedJob(summary.clone.cloned_exec_job_id, &cancel_error);
    }

    coordinator.Stop();

    std::error_code ec;
    summary.capture_found = options.probe_mode == ProbeMode::Capture
        && std::filesystem::exists(summary.expected_capture_path, ec);
    if (options.probe_mode == ProbeMode::Capture && summary.capture_found) {
        std::vector<std::filesystem::path> copied_segments;
        std::string copy_error;
        if (!copy_capture_segments(
                summary.expected_capture_path,
                summary.stable_capture_path,
                &copied_segments,
                &copy_error)) {
            append_error(summary, copy_error);
        } else {
            PreparedCaptureArtifact capture;
            std::string capture_error;
            if (!prepare_capture_artifact(
                    summary.stable_capture_path,
                    summary.capture_export_path,
                    &capture,
                    &capture_error)) {
                append_error(summary, capture_error);
            }
            summary.capture_artifact = capture;
            if (!capture.complete) {
                append_error(summary, "capture session is incomplete: " + capture.incomplete_reason);
            }
            summary.captured_original_seed = capture.seed_override.original_seed;
            summary.captured_override_seed = capture.seed_override.requested_seed;
            summary.captured_applied_seed = capture.seed_override.applied_seed;
            summary.captured_seed_readback_matches = capture.seed_override.readback_matches;
            if (capture.verified) {
            std::ofstream trace(summary.trace_report_path, std::ios::binary | std::ios::trunc);
            if (!trace.is_open()) {
                append_error(summary, "failed opening trace report");
            } else {
                TraceCheckpointsOptions trace_options;
                trace_options.db_root = summary.sandbox.db_root;
                trace_options.checkpoint_file = summary.capture_export_path;
                trace_options.exec_job_id = summary.clone.cloned_exec_job_id;
                trace_options.action_view_std_json_dir =
                    summary.options.action_view_std_json_dir;
                trace_options.disc_dump_root = summary.options.disc_dump_root;
                trace_options.spice_file_parsing_exe = summary.options.spice_file_parsing_exe;
                trace_options.resource_inputs = summary.options.resource_inputs;
                summary.trace_exit_code = run_trace_checkpoints(trace_options, trace, err);
                if (summary.trace_exit_code != 0) {
                    append_error(summary, "trace-checkpoints failed with exit code "
                        + std::to_string(summary.trace_exit_code));
                }
            }
            }
        }
    } else if (options.probe_mode == ProbeMode::Capture) {
        append_error(summary, "capture .scap was not produced at expected path");
    } else {
        const auto unexpected_capture = working_root
            / ("job-" + std::to_string(summary.clone.cloned_exec_job_id))
            / "battle_capture.scap";
        if (std::filesystem::exists(unexpected_capture, ec)) {
            append_error(summary, "non-capture probe mode unexpectedly produced a .scap");
        }
    }

    write_manifest_outputs(summary, err);
    db_service->Stop();

    out << "Run root: " << summary.sandbox.run_root.string() << "\n";
    out << "Terminal state: " << (summary.terminal_state.empty() ? "unknown" : summary.terminal_state) << "\n";
    if (summary.capture_found) {
        out << "Capture: " << summary.stable_capture_path.string() << "\n";
        out << "Trace report: " << summary.trace_report_path.string() << "\n";
    } else {
        out << "Probe mode: " << probe_mode_name(options.probe_mode) << " (no .scap)\n";
    }

    if (summary.timed_out || summary.terminal_state != "SUCCEEDED" || !summary.errors.empty()) {
        return 1;
    }
    return 0;
}

} // namespace savor::predict
