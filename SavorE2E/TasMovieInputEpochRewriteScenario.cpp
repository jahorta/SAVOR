#include "TasMovieInputEpochRewriteScenario.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Analysis/IAnalysisDb.h"
#include "DbSetup.h"
#include "Execution/CoordinatorRuntime.h"
#include "Execution/ProgramDB/ProductionProgramKindRegistry.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Phases/Programs/TasMovieInputEpoch/TasMovieInputEpochModule.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "State/IStateDb.h"
#include "Utils/Hash.h"
#include "WorkerStartupBarrier.h"

namespace savor::e2e {
namespace {

constexpr std::uint32_t kWorkerStartupOperationTimeoutMs = 60'000;

struct BreakpointDiagnosticLogEvidence {
    std::size_t completed_padreads = 0;
    std::uint64_t last_cursor = 0;
    bool unrouted_pause = false;
    std::vector<std::string> correlated_records;
};

std::optional<std::uint64_t> ParseUnsignedField(
    std::string_view line,
    std::string_view field) {
    const auto field_position = line.find(field);
    if (field_position == std::string_view::npos) return std::nullopt;
    const auto begin = field_position + field.size();
    auto end = begin;
    while (end < line.size() && line[end] >= '0' && line[end] <= '9') ++end;
    if (end == begin) return std::nullopt;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(
        line.data() + begin, line.data() + end, value);
    return parsed.ec == std::errc{} && parsed.ptr == line.data() + end
        ? std::optional<std::uint64_t>(value)
        : std::nullopt;
}

BreakpointDiagnosticLogEvidence ReadBreakpointDiagnosticLog(
    const std::filesystem::path& path) {
    BreakpointDiagnosticLogEvidence evidence{};
    std::ifstream stream(path);
    std::string line;
    while (std::getline(stream, line)) {
        const bool routed_padread =
            line.find("[tag=execution.operation,execution.terminal]")
                != std::string::npos
            && line.find("selector=next-pad-read/await") != std::string::npos
            && line.find("status=requested_completion") != std::string::npos
            && line.find("hit_pc=0x801D6E7C") != std::string::npos;
        if (routed_padread) {
            ++evidence.completed_padreads;
            if (const auto cursor = ParseUnsignedField(
                    line, "recording_input_count=")) {
                evidence.last_cursor = *cursor;
            }
        }
        const bool correlated =
            line.find("[tag=stop.router,stop.router.hit]") != std::string::npos
            || line.find("[tag=execution.unrouted_pause") != std::string::npos
            || line.find("[tag=breakpoint.diagnostics") != std::string::npos;
        if (!correlated) continue;
        evidence.unrouted_pause = evidence.unrouted_pause
            || line.find("[tag=execution.unrouted_pause") != std::string::npos;
        evidence.correlated_records.push_back(line);
        if (evidence.correlated_records.size() > 8)
            evidence.correlated_records.erase(evidence.correlated_records.begin());
    }
    return evidence;
}

bool ReportBreakpointDiagnosticRun(
    savor::db::IExecutionDb* execution_db,
    const savor::runner::parallel::savordb::CoordinatorRuntime& coordinator,
    std::int64_t workflow_id,
    int run) {
    const auto workers = coordinator.SnapshotWorkers();
    BreakpointDiagnosticLogEvidence log_evidence{};
    for (const auto& worker : workers) {
        const auto evidence = ReadBreakpointDiagnosticLog(worker.log_path);
        log_evidence.completed_padreads += evidence.completed_padreads;
        if (evidence.last_cursor != 0)
            log_evidence.last_cursor = evidence.last_cursor;
        log_evidence.unrouted_pause = log_evidence.unrouted_pause
            || evidence.unrouted_pause;
        log_evidence.correlated_records.insert(
            log_evidence.correlated_records.end(),
            evidence.correlated_records.begin(), evidence.correlated_records.end());
        std::cout << "[breakpoint-diagnostic-worker] run=" << run
                  << " worker=" << worker.worker_id
                  << " generation=" << worker.process_generation
                  << " log_path=" << worker.log_path << '\n';
    }
    if (log_evidence.correlated_records.size() > 8) {
        log_evidence.correlated_records.erase(
            log_evidence.correlated_records.begin(),
            log_evidence.correlated_records.end() - 8);
    }

    const auto graph = execution_db->WorkflowQueryService()
        ->GetWorkflowGraph(workflow_id);
    if (graph) {
        for (const auto& step : graph->steps) {
            if (!step.job_set_id) continue;
            for (const auto& member :
                 execution_db->ListJobsInJobSet(*step.job_set_id)) {
                const auto job = execution_db->GetExecutionJob(member.job_id);
                if (!job) continue;
                std::cout << "[breakpoint-diagnostic-job] run=" << run
                          << " workflow=" << workflow_id
                          << " step=" << step.workflow_step_id
                          << " job_set=" << *step.job_set_id
                          << " job=" << job->job_id
                          << " workset="
                          << (job->workset_id
                              ? std::to_string(*job->workset_id)
                              : std::string("none"))
                          << " state=" << job->state
                          << " terminal="
                          << job->worker_terminal_status.value_or("none")
                          << " completed_padreads="
                          << log_evidence.completed_padreads
                          << " last_cursor=" << log_evidence.last_cursor
                          << '\n';
                for (const auto& event :
                     execution_db->ListJobEvents(job->job_id)) {
                    if (event.message.find(
                            "Dolphin paused without a routed completion")
                        != std::string::npos) {
                        std::cout << "[breakpoint-diagnostic-terminal] job="
                                  << job->job_id << " event="
                                  << event.event_kind << " message="
                                  << event.message << '\n';
                    }
                }
            }
        }
    }
    for (const auto& record : log_evidence.correlated_records)
        std::cout << "[breakpoint-diagnostic-record] " << record << '\n';
    return log_evidence.unrouted_pause;
}

std::optional<std::vector<std::uint8_t>> ReadFile(
    const std::filesystem::path& path,
    std::string* error_out) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        if (error_out) *error_out = "failed opening " + path.string();
        return std::nullopt;
    }
    const auto end = stream.tellg();
    if (end < 0) {
        if (error_out) *error_out = "failed sizing " + path.string();
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty()
        && !stream.read(reinterpret_cast<char*>(bytes.data()), end)) {
        if (error_out) *error_out = "failed reading " + path.string();
        return std::nullopt;
    }
    return bytes;
}

bool IsExactButtonOnly(const GCInputFrame& input, std::uint16_t button) {
    return input.buttons == button
        && input.main_x == 128 && input.main_y == 128
        && input.c_x == 128 && input.c_y == 128
        && input.trig_l == 0 && input.trig_r == 0;
}

std::optional<std::int64_t> FindOutput(
    savor::db::IExecutionDb* execution_db,
    std::int64_t workflow_id,
    std::string_view node_key,
    std::string_view output_key,
    std::string_view data_kind,
    std::string_view ref_kind) {
    const auto outputs = execution_db->WorkflowQueryService()
        ->ListStepOutputs(workflow_id);
    std::optional<std::int64_t> result;
    for (const auto& output : outputs) {
        if (output.graph_node_key == node_key
            && output.output_key == output_key
            && output.data_kind == data_kind
            && output.ref_kind == ref_kind
            && output.ref_id > 0) {
            if (result) return std::nullopt;
            result = output.ref_id;
        }
    }
    return result;
}

bool WaitForWorkflow(
    savor::db::IExecutionDb* execution_db,
    std::int64_t workflow_id,
    std::string_view phase,
    std::chrono::milliseconds poll,
    std::string* error_out) {
    using savor::db::execution::workflow::WorkflowInstanceState;
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::minutes(30);
    std::size_t polls = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto graph = execution_db->WorkflowQueryService()
            ->GetWorkflowGraph(workflow_id);
        if (graph) {
            if (++polls == 1 || polls % 20 == 0) {
                std::cout << "[tasmovie-input-epoch] phase=" << phase
                          << " workflow=" << workflow_id
                          << " state=" << static_cast<int>(graph->instance.state)
                          << '\n';
            }
            if (graph->instance.state == WorkflowInstanceState::Completed)
                return true;
            if (graph->instance.state == WorkflowInstanceState::Failed
                || graph->instance.state == WorkflowInstanceState::Interrupted
                || graph->instance.state == WorkflowInstanceState::Canceled) {
                if (error_out) {
                    *error_out = std::string(phase)
                        + " workflow entered non-success state "
                        + std::to_string(static_cast<int>(graph->instance.state));
                }
                return false;
            }
        }
        std::this_thread::sleep_for(poll);
    }
    if (error_out) *error_out = std::string(phase) + " workflow timed out";
    return false;
}

bool LoadSchedule(
    savor::db::IStateDb* state_db,
    std::int64_t artifact_id,
    const std::filesystem::path& destination,
    savor::runtime::tasmovie::inputepoch::TasMovieInputEpochScheduleV1* out,
    std::string* error_out) {
    if (!state_db->MaterializeArtifactToPath(
            artifact_id, destination.string(), error_out)) {
        return false;
    }
    const auto bytes = ReadFile(destination, error_out);
    return bytes && savor::runtime::tasmovie::inputepoch::
        DecodeInputEpochScheduleArtifactV1(*bytes, *out, error_out);
}

} // namespace

bool RunTasMovieInputEpochRewriteRealWorkerScenario(
    const CliOptions& options,
    const ResolvedE2eScenarioEntry&,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out) {
    namespace inputepoch = savor::runtime::tasmovie::inputepoch;
    const bool breakpoint_diagnostic = options.scenario ==
        "tasmovie_input_epoch_breakpoint_diagnostics";
    if (db_service == nullptr || options.dtm_file.empty()
        || options.iso_path.empty() || options.dolphin_base_dir.empty()) {
        if (error_out) *error_out = "input-epoch rewrite scenario inputs are incomplete";
        return false;
    }
    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::is_regular_file(worker_exe)) {
        if (error_out) *error_out = "SavorWorker executable is missing: " + worker_exe.string();
        return false;
    }
    const auto workspace = options.workspace_root.value_or(
        std::filesystem::temp_directory_path() / "savor-e2e-input-epoch");
    std::error_code directory_error;
    std::filesystem::create_directories(workspace / "verification", directory_error);
    if (directory_error) {
        if (error_out) *error_out = "failed creating verification directory: " + directory_error.message();
        return false;
    }

    const auto source_bytes = ReadFile(options.dtm_file, error_out);
    if (!source_bytes) return false;
    const auto source_sha256 = hash::sha256_of_file(options.dtm_file.string());
    std::int64_t source_dtm_artifact_id = 0;
    if (!SeedStateDtmArtifact(
            db_service->StateDb(), options.dtm_file,
            &source_dtm_artifact_id, error_out)) {
        return false;
    }

    std::int64_t source_annotation_workflow = 0;
    if (!SeedTasMovieInputEpochAnnotationWorkflow(
            db_service->AuthoringDb(), db_service->ExecutionDb(),
            source_dtm_artifact_id, "source", &source_annotation_workflow,
            error_out, breakpoint_diagnostic)) {
        return false;
    }
    std::int64_t source_establishment_workflow = 0;
    if (!SeedTasMovieWorkflow(
            db_service->AuthoringDb(), db_service->ExecutionDb(),
            source_dtm_artifact_id, &source_establishment_workflow, error_out)) {
        return false;
    }

    auto registry_config = savor::db::execution::programdb::
        MakeProductionProgramKindRegistryConfig(
            workspace / "workflow-runtime", worker_exe);
    savor::db::execution::programdb::ProgramKindRegistry registry;
    std::string error;
    if (!savor::db::execution::programdb::BuildProductionProgramKindRegistry(
            {
                .execution_db = db_service->ExecutionDb(),
                .state_db = db_service->StateDb(),
                .analysis_db = db_service->AnalysisDb(),
                .authoring_db = db_service->AuthoringDb(),
            },
            std::move(registry_config), &registry, &error)) {
        if (error_out) *error_out = "failed building program registry: " + error;
        return false;
    }

    std::string iso_sha256;
    try {
        iso_sha256 = hash::sha256_of_file(options.iso_path.string());
    } catch (const std::exception& exception) {
        if (error_out) *error_out = "failed hashing ISO: " + std::string(exception.what());
        return false;
    }
    savor::runner::parallel::savordb::WorkerCoordinatorConfig worker_config{
        .desired_workers = 1,
        .controller_sleep_ms = static_cast<std::uint32_t>(std::max<std::int64_t>(1, options.poll_ms)),
        .worker_start_timeout_ms = kWorkerStartupOperationTimeoutMs,
        .breakpoint_diagnostics = options.breakpoint_diagnostics
            || breakpoint_diagnostic,
        .worker_exe_path = worker_exe.string(),
        .iso_path = options.iso_path.string(),
        .dolphin_base_dir = options.dolphin_base_dir.string(),
        .worker_dir_root = options.worker_dir_root.value_or(workspace / "workers").string(),
        .worker_binary_runtime_root = (workspace / "worker-runtime").string(),
        .worker_mode = options.visual_worker
            ? savor::runtime::WorkerMode::Visual
            : savor::runtime::WorkerMode::Headless,
        .runtime_artifact_root = (workspace / "runtime-artifacts").string(),
    };
    savor::runtime::ArtifactCompatibilityToken compatibility{
        .game_id = std::string(savor::runtime::program::capabilities::kSupportedGameId),
        .iso_sha256 = std::move(iso_sha256),
        .emulator_build = "dolphin-2506a",
        .runtime_revision = "worker-runtime-slice4",
    };
    savor::runner::parallel::savordb::CoordinatorRuntime coordinator;
    ArmInitialWorkerPoolBarrier(
        options.wait_for_workers_ready,
        [&](bool paused) { coordinator.SetExecutionPaused(paused); },
        [&](const std::string& line) { std::cout << line << '\n'; });
    if (!coordinator.Start(
            db_service->ExecutionDb(), db_service->AuthoringDb(), &registry,
            {
                .worker = std::move(worker_config),
                .poll_interval = std::chrono::milliseconds(std::max<std::int64_t>(1, options.poll_ms)),
                .state_compatibility = std::move(compatibility),
                .initially_paused = options.wait_for_workers_ready,
                .object_store_root = workspace / "object_store",
                .event_line_callback = [&](const std::string& line) { std::cout << line << '\n'; },
            },
            &error)) {
        if (error_out) *error_out = "coordinator startup failed: " + error;
        return false;
    }
    bool coordinator_started = true;
    const auto stop_coordinator = [&]() {
        if (!coordinator_started) return std::string{};
        std::string stop_error;
        (void)coordinator.Stop(&stop_error);
        coordinator_started = false;
        return stop_error;
    };

    const auto barrier = WaitForInitialWorkerPool(
        options.wait_for_workers_ready,
        std::chrono::milliseconds(std::max<std::int64_t>(1, options.poll_ms)),
        [&]() { return coordinator.SnapshotFleetStartup(); },
        [&](bool paused) { coordinator.SetExecutionPaused(paused); },
        [&](const std::string& line) { std::cout << line << '\n'; });
    if (!barrier.satisfied) {
        const auto stop_error = stop_coordinator();
        if (error_out) {
            *error_out = barrier.diagnostic;
            if (!stop_error.empty()) *error_out += "; shutdown: " + stop_error;
        }
        return false;
    }

    const auto poll = std::chrono::milliseconds(std::max<std::int64_t>(1, options.poll_ms));
    auto fail_after_start = [&](std::string message) {
        const auto stop_error = stop_coordinator();
        if (!stop_error.empty()) message += "; shutdown: " + stop_error;
        if (error_out) *error_out = std::move(message);
        return false;
    };
    if (breakpoint_diagnostic) {
        for (int run = 1; run <= options.diagnostic_max_runs; ++run) {
            error.clear();
            if (!WaitForWorkflow(db_service->ExecutionDb(), source_annotation_workflow,
                    "breakpoint-diagnostic-" + std::to_string(run), poll, &error)) {
                const bool reproduced = ReportBreakpointDiagnosticRun(
                    db_service->ExecutionDb(), coordinator,
                    source_annotation_workflow, run);
                if (reproduced) {
                    std::cout << "[RESULT] REPRODUCED run=" << run
                              << " workflow=" << source_annotation_workflow
                              << " classification=UNROUTED_PAUSE\n";
                    const auto stop_error = stop_coordinator();
                    if (!stop_error.empty() && error_out) *error_out = stop_error;
                    return stop_error.empty();
                }
                return fail_after_start(error);
            }
            std::cout << "[RESULT] diagnostic_run=" << run
                      << " workflow=" << source_annotation_workflow
                      << " terminal=COMPLETED\n";
            (void)ReportBreakpointDiagnosticRun(db_service->ExecutionDb(),
                coordinator, source_annotation_workflow, run);
            if (run < options.diagnostic_max_runs
                && !SeedTasMovieInputEpochAnnotationWorkflow(
                    db_service->AuthoringDb(), db_service->ExecutionDb(),
                    source_dtm_artifact_id, "diagnostic-" + std::to_string(run + 1),
                    &source_annotation_workflow, &error, true)) {
                return fail_after_start(error);
            }
        }
        const auto stop_error = stop_coordinator();
        if (!stop_error.empty()) {
            if (error_out) *error_out = stop_error;
            return false;
        }
        std::cout << "[RESULT] NOT_REPRODUCED runs="
                  << options.diagnostic_max_runs << '\n';
        return true;
    }
    if (!WaitForWorkflow(db_service->ExecutionDb(), source_annotation_workflow,
            "source-annotation", poll, &error))
        return fail_after_start(error);
    if (!WaitForWorkflow(db_service->ExecutionDb(), source_establishment_workflow,
            "source-establishment", poll, &error))
        return fail_after_start(error);

    const auto source_attempt_id = FindOutput(
        db_service->ExecutionDb(), source_annotation_workflow, "annotate_1",
        "annotation_attempt", "analysis.tas_movie_input_epoch_annotation_attempt_id",
        "tmv_input_epoch_annotation_attempt");
    if (!source_attempt_id) return fail_after_start("source annotation output is missing or ambiguous");
    const auto source_attempt = db_service->AnalysisDb()
        ->GetTasMovieInputEpochAnnotationAttempt(*source_attempt_id);
    if (!source_attempt || !source_attempt->succeeded
        || !source_attempt->schedule_artifact_id) {
        return fail_after_start("source annotation attempt is not a successful persisted schedule");
    }
    const auto source_root_id = FindOutput(
        db_service->ExecutionDb(), source_establishment_workflow, "tas_1",
        "root_establishment", "analysis.tas_movie_root_establishment_attempt_id",
        "tmv_root_establishment_attempt");
    if (!source_root_id) return fail_after_start("source root establishment output is missing or ambiguous");
    inputepoch::TasMovieInputEpochScheduleV1 source_schedule;
    if (!LoadSchedule(db_service->StateDb(), *source_attempt->schedule_artifact_id,
            workspace / "verification" / "source.tes", &source_schedule, &error)) {
        return fail_after_start(error);
    }

    std::optional<std::size_t> insertion_epoch;
    for (std::size_t i = 0; i + 1 < source_schedule.epochs.size(); ++i) {
        if (IsExactButtonOnly(source_schedule.epochs[i].input, GC_B)
            && IsExactButtonOnly(source_schedule.epochs[i + 1].input, GC_A)) {
            insertion_epoch = i;
        }
    }
    if (!insertion_epoch) return fail_after_start("source schedule has no exact B-only to A-only adjacent epoch pair");

    std::string quiescence;
    if (!CheckWorkflowQuiescence(db_service->ExecutionDb(), &quiescence))
        return fail_after_start("source annotation did not quiesce: " + quiescence);
    std::int64_t rewrite_workflow = 0;
    if (!SeedTasMovieInputEpochRewriteWorkflow(
            db_service->AuthoringDb(), db_service->ExecutionDb(),
            *source_attempt_id, *source_root_id,
            static_cast<std::int64_t>(*insertion_epoch), 1,
            "neutral-before-final-b-a", &rewrite_workflow, &error)) {
        return fail_after_start(error);
    }
    if (!WaitForWorkflow(db_service->ExecutionDb(), rewrite_workflow,
            "rewrite", poll, &error)) {
        return fail_after_start(error);
    }

    const auto rewrite_attempt_id = FindOutput(
        db_service->ExecutionDb(), rewrite_workflow, "rewrite_1",
        "rewrite_attempt", "analysis.tas_movie_input_epoch_rewrite_attempt_id",
        "tmv_input_epoch_rewrite_attempt");
    if (!rewrite_attempt_id) return fail_after_start("rewrite attempt output is missing or ambiguous");
    const auto rewrite_attempt = db_service->AnalysisDb()
        ->GetTasMovieInputEpochRewriteAttempt(*rewrite_attempt_id);
    if (!rewrite_attempt || !rewrite_attempt->succeeded
        || !rewrite_attempt->rewritten_dtm_artifact_id
        || !rewrite_attempt->endpoint_savestate_id
        || !rewrite_attempt->rewritten_dtm_sha256) {
        return fail_after_start("rewrite attempt is incomplete");
    }
    const auto child_artifact = db_service->StateDb()->GetArtifact(
        *rewrite_attempt->rewritten_dtm_artifact_id);
    const auto endpoint = db_service->StateDb()->GetSavestate(
        *rewrite_attempt->endpoint_savestate_id);
    if (!child_artifact || !endpoint || !endpoint->is_complete
        || endpoint->playback_state != savor::db::SavestatePlaybackState::MoviePaired
        || endpoint->savestate_type != "TAS_MOVIE_INPUT_EPOCH_REWRITE_ENDPOINT"
        || endpoint->dtm_artifact_id != rewrite_attempt->rewritten_dtm_artifact_id
        || child_artifact->sha256 != *rewrite_attempt->rewritten_dtm_sha256
        || child_artifact->sha256 == source_sha256) {
        return fail_after_start("rewritten DTM or paired endpoint invariant failed");
    }

    const auto child_attempt_id = FindOutput(
        db_service->ExecutionDb(), rewrite_workflow, "rewrite_1",
        "annotation_attempt", "analysis.tas_movie_input_epoch_annotation_attempt_id",
        "tmv_input_epoch_annotation_attempt");
    if (!child_attempt_id) return fail_after_start("child annotation output is missing or ambiguous");
    const auto child_attempt = db_service->AnalysisDb()
        ->GetTasMovieInputEpochAnnotationAttempt(*child_attempt_id);
    if (!child_attempt || !child_attempt->succeeded || !child_attempt->schedule_artifact_id)
        return fail_after_start("child annotation attempt is incomplete");
    const auto child_root_id = FindOutput(
        db_service->ExecutionDb(), rewrite_workflow, "rewrite_1",
        "root_establishment", "analysis.tas_movie_root_establishment_attempt_id",
        "tmv_root_establishment_attempt");
    if (!child_root_id) return fail_after_start("child root establishment output is missing or ambiguous");
    inputepoch::TasMovieInputEpochScheduleV1 child_schedule;
    if (!LoadSchedule(db_service->StateDb(), *child_attempt->schedule_artifact_id,
            workspace / "verification" / "child.tes", &child_schedule, &error)) {
        return fail_after_start(error);
    }

    std::vector<GCInputFrame> expected;
    expected.reserve(source_schedule.epochs.size() + 1);
    for (std::size_t i = 0; i < *insertion_epoch; ++i)
        expected.push_back(source_schedule.epochs[i].input);
    expected.push_back(GCInputFrame{});
    for (std::size_t i = *insertion_epoch; i < source_schedule.epochs.size(); ++i)
        expected.push_back(source_schedule.epochs[i].input);
    if (child_schedule.epochs.size() != expected.size())
        return fail_after_start("child schedule epoch count does not equal source plus one");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (!(child_schedule.epochs[i].input == expected[i]))
            return fail_after_start("child schedule diverged at epoch " + std::to_string(i));
    }

    std::int64_t chained_rewrite_workflow = 0;
    if (!SeedTasMovieInputEpochRewriteWorkflow(
            db_service->AuthoringDb(), db_service->ExecutionDb(),
            *child_attempt_id, *child_root_id,
            static_cast<std::int64_t>(*insertion_epoch + 1), 2,
            "chain-two-neutral-before-final-b-a", &chained_rewrite_workflow,
            &error)) {
        return fail_after_start(error);
    }
    if (!WaitForWorkflow(db_service->ExecutionDb(), chained_rewrite_workflow,
            "chained-rewrite", poll, &error)) {
        return fail_after_start(error);
    }
    const auto chained_attempt_id = FindOutput(
        db_service->ExecutionDb(), chained_rewrite_workflow, "rewrite_1",
        "annotation_attempt", "analysis.tas_movie_input_epoch_annotation_attempt_id",
        "tmv_input_epoch_annotation_attempt");
    const auto chained_root_id = FindOutput(
        db_service->ExecutionDb(), chained_rewrite_workflow, "rewrite_1",
        "root_establishment", "analysis.tas_movie_root_establishment_attempt_id",
        "tmv_root_establishment_attempt");
    if (!chained_attempt_id || !chained_root_id)
        return fail_after_start("chained revise authorities are missing or ambiguous");
    const auto chained_attempt = db_service->AnalysisDb()
        ->GetTasMovieInputEpochAnnotationAttempt(*chained_attempt_id);
    if (!chained_attempt || !chained_attempt->succeeded
        || !chained_attempt->schedule_artifact_id) {
        return fail_after_start("chained annotation attempt is incomplete");
    }
    inputepoch::TasMovieInputEpochScheduleV1 chained_schedule;
    if (!LoadSchedule(db_service->StateDb(), *chained_attempt->schedule_artifact_id,
            workspace / "verification" / "chained.tes", &chained_schedule, &error)) {
        return fail_after_start(error);
    }
    std::vector<GCInputFrame> chained_expected;
    chained_expected.reserve(source_schedule.epochs.size() + 3);
    for (std::size_t i = 0; i < *insertion_epoch; ++i)
        chained_expected.push_back(source_schedule.epochs[i].input);
    chained_expected.insert(chained_expected.end(), 3, GCInputFrame{});
    for (std::size_t i = *insertion_epoch; i < source_schedule.epochs.size(); ++i)
        chained_expected.push_back(source_schedule.epochs[i].input);
    if (chained_schedule.epochs.size() != chained_expected.size())
        return fail_after_start("chained schedule epoch count does not equal source plus three");
    for (std::size_t i = 0; i < chained_expected.size(); ++i) {
        if (!(chained_schedule.epochs[i].input == chained_expected[i]))
            return fail_after_start("chained schedule diverged at epoch " + std::to_string(i));
    }
    const auto current_source_bytes = ReadFile(options.dtm_file, &error);
    if (!current_source_bytes || *current_source_bytes != *source_bytes)
        return fail_after_start(error.empty() ? "source DTM bytes changed" : error);

    const auto stop_error = stop_coordinator();
    if (!stop_error.empty()) {
        if (error_out) *error_out = "coordinator shutdown failed: " + stop_error;
        return false;
    }
    std::cout
        << "[tasmovie-input-epoch-result] insertion_epoch=" << *insertion_epoch
        << " source_cursor=" << source_schedule.epochs[*insertion_epoch].movie_input_cursor
        << " source_epochs=" << source_schedule.epochs.size()
        << " child_epochs=" << child_schedule.epochs.size()
        << " source_dtm_artifact=" << source_dtm_artifact_id
        << " source_dtm_sha256=" << source_sha256
        << " source_schedule_artifact=" << *source_attempt->schedule_artifact_id
        << " source_schedule_sha256=" << source_attempt->schedule_sha256.value_or("")
        << " child_dtm_artifact=" << *rewrite_attempt->rewritten_dtm_artifact_id
        << " child_dtm_sha256=" << *rewrite_attempt->rewritten_dtm_sha256
        << " child_schedule_artifact=" << *child_attempt->schedule_artifact_id
        << " child_schedule_sha256=" << child_attempt->schedule_sha256.value_or("")
        << " endpoint_savestate=" << *rewrite_attempt->endpoint_savestate_id
        << " chained_schedule_artifact=" << *chained_attempt->schedule_artifact_id
        << " chained_epochs=" << chained_schedule.epochs.size()
        << " workflows=" << source_establishment_workflow << ','
        << source_annotation_workflow << ',' << rewrite_workflow << ','
        << chained_rewrite_workflow << '\n';
    std::cout
        << "[tasmovie-input-epoch-trajectory] annotate_source=COMPLETED"
        << " rewrite_count_1=COMPLETED rewrite_count_2=COMPLETED replay=movie_end"
        << " schedule=source_prefix+neutral_x3+source_suffix checkpoint=MOVIE_PAIRED\n";
    return true;
}

} // namespace savor::e2e
