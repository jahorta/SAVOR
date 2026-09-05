#include "TasMovieCutsceneScenario.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Analysis/IAnalysisDb.h"
#include "Authoring/AuthoringRecipeMaterializer.h"
#include "BattlePhasesRealWorkerScenario.h"
#include "Common/DbService.h"
#include "DbSetup.h"
#include "Execution/CoordinatorRuntime.h"
#include "Execution/ProgramDB/ProductionProgramKindRegistry.h"
#include "Execution/ProgramDB/TasMovieValidation/TasMovieInputEpochProgram.h"
#include "Execution/Workflow/BattleVictoryRecordingService.h"
#include "Execution/Workflow/WorkflowComposition.h"
#include "Execution/Workflow/WorkflowGraphLaunchService.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Utils/Hash.h"
#include "WorkerStartupBarrier.h"

namespace savor::e2e {
namespace {

using savor::db::execution::workflow::WorkflowInstanceState;
using savor::db::execution::workflow::WorkflowStepState;
constexpr auto kTimeout = std::chrono::minutes(20);
constexpr auto kFanoutTimeout = std::chrono::minutes(60);

bool Fail(std::string message, std::string* error_out) {
    if (error_out) *error_out = std::move(message);
    return false;
}

bool WaitForWorkflow(
    savor::runner::parallel::savordb::CoordinatorRuntime& runtime,
    savor::db::IExecutionDb* execution, std::int64_t workflow_id,
    std::chrono::milliseconds poll, std::string_view label,
    savor::db::execution::workflow::WorkflowGraphSnapshot* graph_out,
    std::string* error_out) {
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto graph = execution->WorkflowQueryService()->GetWorkflowGraph(
            workflow_id);
        if (graph && (graph->instance.state == WorkflowInstanceState::Completed
                || graph->instance.state == WorkflowInstanceState::Failed
                || graph->instance.state == WorkflowInstanceState::Canceled)) {
            if (graph_out) *graph_out = *graph;
            if (graph->instance.state == WorkflowInstanceState::Completed)
                return true;
            std::string message(label);
            message += " workflow did not complete";
            for (const auto& step : graph->steps) {
                if (step.state == WorkflowStepState::Failed) {
                    message += "; " + step.step_key + ":"
                        + step.blocked_reason.value_or("failed");
                }
            }
            return Fail(std::move(message), error_out);
        }
        const auto telemetry = runtime.SnapshotTelemetry();
        if (telemetry.execution.invariant_admission_paused) {
            return Fail(telemetry.execution.last_error.empty()
                ? std::string(label) + " coordinator entered an invariant pause"
                : telemetry.execution.last_error, error_out);
        }
        std::this_thread::sleep_for(poll);
    }
    return Fail(std::string(label) + " workflow timed out", error_out);
}

std::optional<std::int64_t> FindSingleOutput(
    savor::db::IExecutionDb* execution, std::int64_t workflow_id,
    std::string_view ref_kind) {
    std::optional<std::int64_t> found;
    for (const auto& output : execution->WorkflowQueryService()
             ->ListStepOutputs(workflow_id)) {
        if (output.ref_kind != ref_kind || output.ref_id <= 0) continue;
        if (found && *found != output.ref_id) return std::nullopt;
        found = output.ref_id;
    }
    return found;
}

std::optional<std::int64_t> FindNodeOutput(
    savor::db::IExecutionDb* execution,
    std::int64_t workflow_id,
    std::string_view node_key,
    std::string_view ref_kind)
{
    if (!execution || workflow_id <= 0)
        return std::nullopt;
    std::optional<std::int64_t> found;
    for (const auto& output : execution->WorkflowQueryService()
             ->ListStepOutputs(workflow_id)) {
        if (output.graph_node_key != node_key || output.ref_kind != ref_kind
            || output.ref_id <= 0)
            continue;
        if (found && *found != output.ref_id)
            return std::nullopt;
        found = output.ref_id;
    }
    return found;
}

std::optional<std::int64_t> FindRecommendedCompletedVictory(
    savor::db::IAnalysisDb* analysis, std::int64_t battle_set_id,
    std::string* error_out) {
    std::set<std::int64_t> recommended;
    std::set<int> turns;
    for (const auto& wave : analysis->ListBattleTurnWaves(battle_set_id))
        turns.insert(wave.turn_index);
    for (const int turn : turns) {
        for (const auto& pool :
             analysis->ListBattleAdvancementPoolsForBattleTurn(
                 battle_set_id, turn)) {
            for (const auto& decision :
                 analysis->ListBattleAdvancementDecisionsForPool(
                     pool.battle_advancement_pool_id)) {
                if (decision.decision_kind !=
                    savor::db::BattleAdvancementDecisionKind::Recommended)
                    continue;
                const auto completion =
                    analysis->GetBattleCompletionForSelectedTurnJob(
                        decision.turn_job_id);
                if (completion && completion->status == "COMPLETED")
                    recommended.insert(decision.turn_job_id);
            }
        }
    }
    if (recommended.size() != 1) {
        Fail("cutscene E2E requires exactly one recommended completed Victory; found "
            + std::to_string(recommended.size()), error_out);
        return std::nullopt;
    }
    return *recommended.begin();
}

bool LaunchCutscene(savor::db::IAuthoringDb* authoring,
    savor::db::IExecutionDb* execution, std::int64_t tree_id,
    std::int64_t rtc, std::string_view run_identity,
    std::int64_t* workflow_id_out,
    std::string* error_out) {
    const auto registry = savor::db::execution::workflow::
        BuildDefaultWorkflowUnitRegistry();
    const auto* unit = registry.Find("tas_movie_cutscene");
    if (!unit || unit->required_inputs.size() != 1)
        return Fail("tas_movie_cutscene workflow unit is unavailable", error_out);
    savor::db::SaveWorkflowGraphResult saved{};
    const savor::db::authoring::AuthoringRecipeMaterializer materializer(authoring);
    if (!materializer.SaveWorkflowGraph({
            .symbol = "e2e.cutscene.rtc." + std::to_string(rtc),
            .name = "SavorE2E cutscene RTC " + std::to_string(rtc) + " "
                + std::string(run_identity),
            .description = "Records and validates two consecutive post-battle cutscene segments.",
            .hidden = true,
            .nodes = {
                {.node_key = "cutscene_1", .unit = {unit->unit_kind},
                    .display_name = unit->display_name + " 1"},
                {.node_key = "cutscene_2", .unit = {unit->unit_kind},
                    .display_name = unit->display_name + " 2"},
            },
            .external_inputs = {{"cutscene_1", {"tas_movie_tree"}}},
            .edges = {{
                .from_node_key = "cutscene_1",
                .output = {"tas_movie_tree"},
                .to_node_key = "cutscene_2",
                .input = {"tas_movie_tree"},
                .guard_kind = std::string(savor::db::kWorkflowOutputPresentGuard),
            }},
        }, {}, &saved, error_out)) return false;
    savor::db::execution::workflow::WorkflowGraphLaunchService launcher(
        authoring, execution);
    return launcher.Start({
        .workflow_graph_revision_id = saved.workflow_graph_revision_id,
        .created_by = "SavorE2E",
        .launch_key = "tasmovie-cutscene:" + std::to_string(rtc) + ":"
            + std::to_string(tree_id),
        .input_bindings = {{
            .node_key = "cutscene_1", .input_key = "tas_movie_tree",
            .data_kind = "state.tas_movie_tree_id",
            .ref_kind = "state_tas_movie_tree", .ref_id = tree_id,
            .source_kind = "scenario"}},
    }, workflow_id_out, error_out);
}

enum class CutsceneBranchStage : std::uint8_t {
    Battle,
    Recording,
    Cutscene,
    Completed,
    Failed,
};

struct CutsceneBranch {
    std::int64_t rtc = 0;
    CutsceneBranchStage stage = CutsceneBranchStage::Battle;
    std::int64_t battle_workflow_id = 0;
    std::int64_t battle_set_id = 0;
    std::int64_t selected_turn_job_id = 0;
    std::int64_t recording_workflow_id = 0;
    std::int64_t recording_id = 0;
    std::int64_t recording_tree_id = 0;
    std::int64_t cutscene_workflow_id = 0;
    std::string diagnostic;
};

bool IsTerminal(const WorkflowInstanceState state) {
    return state == WorkflowInstanceState::Completed
        || state == WorkflowInstanceState::Failed
        || state == WorkflowInstanceState::Canceled;
}

std::vector<std::int64_t> ResolveRtcValues(const CliOptions& options) {
    if (options.tasmovie_rtc)
        return {*options.tasmovie_rtc};
    std::vector<std::int64_t> values;
    if (!options.tasmovie_rtc_min || !options.tasmovie_rtc_max)
        return values;
    values.reserve(static_cast<std::size_t>(
        *options.tasmovie_rtc_max - *options.tasmovie_rtc_min + 1));
    for (auto rtc = *options.tasmovie_rtc_min;
         rtc <= *options.tasmovie_rtc_max; ++rtc)
        values.push_back(rtc);
    return values;
}

} // namespace

bool RunTasMovieCutsceneRealWorkerScenario(
    const CliOptions& options, const ResolvedE2eScenarioEntry& entry,
    const char* argv0, savor::db::core::DBService* db_service,
    std::string* error_out)
{
    const auto rtc_values = ResolveRtcValues(options);
    if (entry.source != E2eScenarioEntrySource::FreshTasMovieValidation
        || !options.savestate_file.empty() || options.dtm_file.empty()
        || rtc_values.empty() || rtc_values.size() > 32
        || !db_service || !db_service->IsRunning()) {
        return Fail("tasmovie_cutscene requires a fresh root DTM, one exact RTC or an inclusive range of at most 32 RTC values, and no savestate argument", error_out);
    }

    std::string error;
    std::int64_t dtm_artifact_id = 0;
    if (!SeedStateDtmArtifact(db_service->StateDb(), options.dtm_file,
            &dtm_artifact_id, &error))
        return Fail("failed seeding Cutscene DTM artifact: " + error, error_out);

    BattleScenarioAuthoringIds authored{};
    if (!PrepareBattleScenarioAuthoring(options, entry.run_identity, true,
            db_service, &authored, &error))
        return Fail("failed seeding Cutscene Battle authoring: " + error,
            error_out);

    std::int64_t establishment_workflow_id = 0;
    if (!SeedTasMovieWorkflow(db_service->AuthoringDb(),
            db_service->ExecutionDb(), dtm_artifact_id,
            &establishment_workflow_id, &error))
        return Fail("failed seeding shared TAS root establishment: " + error,
            error_out);

    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::is_regular_file(worker_exe))
        return Fail("SavorWorker.exe was not found next to SavorE2E", error_out);
    const auto workspace_root = *options.workspace_root;
    savor::db::execution::programdb::ProgramKindRegistry registry;
    auto registry_config =
        savor::db::execution::programdb::MakeProductionProgramKindRegistryConfig(
            workspace_root / "workflow-runtime", worker_exe);
    registry_config.tas_movie_cutscene.enable_seed_call_progress =
        options.capture_seed_calls;
    if (!savor::db::execution::programdb::BuildProductionProgramKindRegistry({
            .execution_db = db_service->ExecutionDb(),
            .state_db = db_service->StateDb(),
            .analysis_db = db_service->AnalysisDb(),
            .authoring_db = db_service->AuthoringDb()},
            std::move(registry_config), &registry, &error))
        return Fail("failed building Cutscene production registry: " + error,
            error_out);

    std::string iso_sha;
    try { iso_sha = hash::sha256_of_file(options.iso_path.string()); }
    catch (const std::exception& e) {
        return Fail("failed hashing Cutscene ISO: " + std::string(e.what()),
            error_out);
    }
    savor::runtime::ArtifactCompatibilityToken compatibility{
        .game_id = std::string(
            savor::runtime::program::capabilities::kSupportedGameId),
        .iso_sha256 = std::move(iso_sha),
        .emulator_build = "dolphin-2506a",
        .runtime_revision = "worker-runtime-slice4"};
    savor::runner::parallel::savordb::WorkerCoordinatorConfig worker{
        .desired_workers = static_cast<std::size_t>(options.worker_count),
        .controller_sleep_ms = static_cast<std::uint32_t>(
            std::max<std::int64_t>(1, options.poll_ms)),
        .worker_start_timeout_ms = 60'000,
        .breakpoint_diagnostics = options.breakpoint_diagnostics,
        .worker_exe_path = worker_exe.string(),
        .iso_path = options.iso_path.string(),
        .dolphin_base_dir = options.dolphin_base_dir.string(),
        .worker_dir_root = options.worker_dir_root.value_or(
            workspace_root / ".workers").string(),
        .worker_binary_runtime_root =
            (workspace_root / "worker-runtime").string(),
        .worker_mode = options.visual_worker ? savor::runtime::WorkerMode::Visual
                                             : savor::runtime::WorkerMode::Headless,
        .runtime_artifact_root =
            (workspace_root / "runtime-artifacts").string()};
    const auto event_sink = [](const std::string& line) {
        std::cout << line << '\n';
    };
    savor::runner::parallel::savordb::CoordinatorRuntime runtime;
    ArmInitialWorkerPoolBarrier(options.wait_for_workers_ready,
        [&](bool paused) { runtime.SetExecutionPaused(paused); }, event_sink);
    if (!runtime.Start(db_service->ExecutionDb(), db_service->AuthoringDb(),
            &registry, {.worker = std::move(worker),
                .poll_interval = std::chrono::milliseconds(
                    std::max<std::int64_t>(1, options.poll_ms)),
                .state_compatibility = std::move(compatibility),
                .initially_paused = options.wait_for_workers_ready,
                .object_store_root = workspace_root / "object_store",
                .event_line_callback = event_sink}, &error))
        return Fail("Cutscene coordinator startup failed: " + error, error_out);

    const auto stop_with_failure = [&](std::string message) {
        std::string stop_error;
        (void)runtime.Stop(&stop_error);
        if (!stop_error.empty()) message += "; shutdown: " + stop_error;
        return Fail(std::move(message), error_out);
    };
    const auto barrier = WaitForInitialWorkerPool(options.wait_for_workers_ready,
        std::chrono::milliseconds(std::max<std::int64_t>(1, options.poll_ms)),
        [&]() { return runtime.SnapshotFleetStartup(); },
        [&](bool paused) { runtime.SetExecutionPaused(paused); }, event_sink);
    if (!barrier.satisfied)
        return stop_with_failure(barrier.diagnostic);

    const auto poll = std::chrono::milliseconds(
        std::max<std::int64_t>(1, options.poll_ms));
    savor::db::execution::workflow::WorkflowGraphSnapshot completed_graph{};
    if (!WaitForWorkflow(runtime, db_service->ExecutionDb(),
            establishment_workflow_id, poll, "shared TAS root establishment",
            &completed_graph, &error))
        return stop_with_failure(error);
    auto shared_root = FindNodeOutput(db_service->ExecutionDb(),
        establishment_workflow_id, "tas_1",
        "tmv_root_establishment_attempt");
    if (!shared_root)
        return stop_with_failure("shared TAS root establishment output is missing");

    std::int64_t annotation_workflow_id = 0;
    if (!SeedTasMovieInputEpochAnnotationWorkflow(db_service->AuthoringDb(),
            db_service->ExecutionDb(), *shared_root,
            entry.run_identity + "-shared-annotation",
            &annotation_workflow_id, &error))
        return stop_with_failure("failed seeding shared TAS annotation: " + error);
    if (!WaitForWorkflow(runtime, db_service->ExecutionDb(),
            annotation_workflow_id, poll, "shared TAS annotation",
            &completed_graph, &error))
        return stop_with_failure(error);
    auto shared_annotation = FindNodeOutput(db_service->ExecutionDb(),
        annotation_workflow_id, "annotate_1",
        "tmv_input_epoch_annotation_attempt");
    if (!shared_annotation)
        return stop_with_failure("shared TAS annotation output is missing");

    if (options.cutscene_delay) {
        const auto annotation = db_service->AnalysisDb()
            ->GetTasMovieInputEpochAnnotationAttempt(*shared_annotation);
        if (!annotation)
            return stop_with_failure("shared TAS annotation authority is unavailable");
        savor::db::execution::programdb::tasmovieinputepoch::
            TasMovieDelayPlacementResolution placement{};
        if (!savor::db::execution::programdb::tasmovieinputepoch::
                ResolveTasMovieDelayPlacement(db_service->StateDb(), *annotation,
                    std::nullopt, std::string_view("first_battle.final_dialog"),
                    &placement, &error))
            return stop_with_failure("failed resolving shared Cutscene delay: " + error);
        std::int64_t revise_workflow_id = 0;
        if (!SeedTasMovieInputEpochRewriteWorkflow(db_service->AuthoringDb(),
                db_service->ExecutionDb(), *shared_annotation,
                static_cast<std::int64_t>(placement.insert_before_epoch), 1,
                entry.run_identity + "-shared-delay",
                &revise_workflow_id, &error))
            return stop_with_failure("failed seeding shared Cutscene delay: " + error);
        if (!WaitForWorkflow(runtime, db_service->ExecutionDb(),
                revise_workflow_id, poll, "shared Cutscene delay",
                &completed_graph, &error))
            return stop_with_failure(error);
        shared_annotation = FindNodeOutput(db_service->ExecutionDb(),
            revise_workflow_id, "rewrite_1",
            "tmv_input_epoch_annotation_attempt");
        shared_root = FindNodeOutput(db_service->ExecutionDb(),
            revise_workflow_id, "rewrite_1",
            "tmv_root_establishment_attempt");
        if (!shared_annotation || !shared_root)
            return stop_with_failure("shared Cutscene delay authorities are missing");
    }

    std::vector<CutsceneBranch> branches;
    branches.reserve(rtc_values.size());
    for (const auto rtc : rtc_values) {
        CutsceneBranch branch{};
        branch.rtc = rtc;
        if (!SeedEstablishedBattleWorkflowForScenario(options,
                *shared_annotation, *shared_root,
                branch.rtc,
                entry.run_identity + "-rtc-" + std::to_string(branch.rtc),
                authored, db_service, &branch.battle_workflow_id, &error))
            return stop_with_failure("failed seeding RTC "
                + std::to_string(branch.rtc) + " Battle branch: " + error);
        branches.push_back(branch);
    }

    const auto fail_branch = [](CutsceneBranch& branch, std::string message) {
        branch.stage = CutsceneBranchStage::Failed;
        branch.diagnostic = std::move(message);
    };
    const auto branch_deadline = std::chrono::steady_clock::now()
        + kFanoutTimeout;
    bool coordinator_failed = false;
    std::string coordinator_failure;
    while (std::chrono::steady_clock::now() < branch_deadline) {
        bool all_terminal = true;
        for (auto& branch : branches) {
            if (branch.stage == CutsceneBranchStage::Completed
                || branch.stage == CutsceneBranchStage::Failed)
                continue;
            all_terminal = false;
            const auto workflow_id = branch.stage == CutsceneBranchStage::Battle
                ? branch.battle_workflow_id
                : branch.stage == CutsceneBranchStage::Recording
                    ? branch.recording_workflow_id
                    : branch.cutscene_workflow_id;
            const auto graph = db_service->ExecutionDb()->WorkflowQueryService()
                ->GetWorkflowGraph(workflow_id);
            if (!graph || !IsTerminal(graph->instance.state))
                continue;
            if (graph->instance.state != WorkflowInstanceState::Completed) {
                std::string diagnostic = "workflow "
                    + std::to_string(workflow_id) + " did not complete";
                for (const auto& step : graph->steps) {
                    if (step.state == WorkflowStepState::Failed)
                        diagnostic += "; " + step.step_key + ":"
                            + step.blocked_reason.value_or("failed");
                }
                fail_branch(branch, std::move(diagnostic));
                continue;
            }

            if (branch.stage == CutsceneBranchStage::Battle) {
                const auto battle_set_id = FindSingleOutput(
                    db_service->ExecutionDb(), branch.battle_workflow_id,
                    "analysis_battle.battle_set");
                const auto battle_set = battle_set_id
                    ? db_service->AnalysisDb()->GetBattleSet(*battle_set_id)
                    : std::nullopt;
                if (!battle_set_id || !battle_set
                    || battle_set->status != savor::db::BattleSetStatus::Victory) {
                    fail_branch(branch, "Battle branch did not reach Victory");
                    continue;
                }
                const auto selected = FindRecommendedCompletedVictory(
                    db_service->AnalysisDb(), *battle_set_id, &error);
                const auto completion = selected
                    ? db_service->AnalysisDb()
                        ->GetBattleCompletionForSelectedTurnJob(*selected)
                    : std::nullopt;
                if (!selected || !completion
                    || completion->status != "COMPLETED"
                    || completion->route_kind
                        != std::optional<std::string>("CUTSCENE")) {
                    fail_branch(branch,
                        error.empty() ? "Battle branch has no completed CUTSCENE Victory"
                                      : error);
                    error.clear();
                    continue;
                }
                savor::db::execution::workflow::BattleVictoryRecordingService
                    recorder(db_service->AuthoringDb(),
                        db_service->ExecutionDb(), db_service->AnalysisDb());
                savor::db::execution::workflow::RecordBattleVictoryReceipt
                    recording{};
                if (!recorder.Record({.turn_job_id = *selected,
                        .created_by = "SavorE2E"}, &recording, &error)
                    || recording.workflow_instance_id <= 0) {
                    fail_branch(branch, "failed launching Battle recording: "
                        + error);
                    error.clear();
                    continue;
                }
                branch.battle_set_id = *battle_set_id;
                branch.selected_turn_job_id = *selected;
                branch.recording_workflow_id = recording.workflow_instance_id;
                branch.stage = CutsceneBranchStage::Recording;
                continue;
            }

            if (branch.stage == CutsceneBranchStage::Recording) {
                const auto recording_id = FindSingleOutput(
                    db_service->ExecutionDb(), branch.recording_workflow_id,
                    "analysis_battle.battle_recording");
                const auto recording = recording_id
                    ? db_service->AnalysisDb()->GetBattleRecording(*recording_id)
                    : std::nullopt;
                if (!recording_id || !recording
                    || recording->status != "COMPLETED"
                    || !recording->tas_movie_tree_id) {
                    fail_branch(branch,
                        "Battle recording did not publish a completed TAS movie tree");
                    continue;
                }
                if (!LaunchCutscene(db_service->AuthoringDb(),
                        db_service->ExecutionDb(), *recording->tas_movie_tree_id,
                        branch.rtc,
                        entry.run_identity + "-rtc-"
                            + std::to_string(branch.rtc),
                        &branch.cutscene_workflow_id, &error)) {
                    fail_branch(branch,
                        "failed launching Cutscene workflow: " + error);
                    error.clear();
                    continue;
                }
                branch.recording_id = *recording_id;
                branch.recording_tree_id = *recording->tas_movie_tree_id;
                branch.stage = CutsceneBranchStage::Cutscene;
                continue;
            }

            branch.stage = CutsceneBranchStage::Completed;
        }
        if (all_terminal) break;
        const auto telemetry = runtime.SnapshotTelemetry();
        if (telemetry.execution.invariant_admission_paused) {
            coordinator_failed = true;
            coordinator_failure = telemetry.execution.last_error.empty()
                ? "Cutscene coordinator entered an invariant pause"
                : telemetry.execution.last_error;
            break;
        }
        std::this_thread::sleep_for(poll);
    }

    for (auto& branch : branches) {
        if (branch.stage != CutsceneBranchStage::Completed
            && branch.stage != CutsceneBranchStage::Failed)
            fail_branch(branch, "scenario-wide Cutscene fan-out timeout");
    }
    const auto warnings = runtime.SnapshotExecutionWarnings();
    std::string stop_error;
    const bool stopped = runtime.Stop(&stop_error);
    if (!stopped && !coordinator_failed) {
        coordinator_failed = true;
        coordinator_failure = "Cutscene coordinator shutdown failed: "
            + stop_error;
    }

    std::size_t completed_count = 0;
    std::size_t battle_count = 0;
    std::size_t recorded_count = 0;
    for (auto& branch : branches) {
        if (branch.battle_set_id > 0) ++battle_count;
        if (branch.recording_tree_id > 0) ++recorded_count;
        if (branch.stage != CutsceneBranchStage::Completed) {
            std::cout << "[cutscene-rtc-summary] rtc=" << branch.rtc
                      << " status=FAILED diagnostic=" << branch.diagnostic
                      << '\n';
            continue;
        }

        const std::array<std::string_view, 2> nodes{
            "cutscene_1", "cutscene_2"};
        std::array<std::int64_t, 2> attempt_ids{};
        std::array<savor::db::TasMovieCutsceneAttemptRecord, 2> attempts{};
        std::array<savor::db::TasMovieTreeRecord, 2> trees{};
        bool valid = true;
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            const auto attempt_id = FindNodeOutput(db_service->ExecutionDb(),
                branch.cutscene_workflow_id, nodes[index],
                "tmv_cutscene_attempt");
            const auto attempt = attempt_id
                ? db_service->AnalysisDb()->GetTasMovieCutsceneAttempt(
                    *attempt_id)
                : std::nullopt;
            const auto tree = attempt && attempt->output_tree_id
                ? db_service->StateDb()->GetTasMovieTree(
                    *attempt->output_tree_id)
                : std::nullopt;
            const auto checkpoint = attempt && attempt->output_savestate_id
                ? db_service->StateDb()->GetSavestate(
                    *attempt->output_savestate_id)
                : std::nullopt;
            if (!attempt_id || !attempt || !attempt->succeeded
                || attempt->endpoint_kind.empty()
                || attempt->final_movie_input_cursor
                    <= attempt->checkpoint_movie_input_cursor
                || !attempt->output_dtm_artifact_id || !tree || !checkpoint
                || !checkpoint->is_complete || !checkpoint->dtm_artifact_id
                || *checkpoint->dtm_artifact_id != tree->dtm_artifact_id
                || tree->source_context_kind != "CUTSCENE") {
                valid = false;
                break;
            }
            attempt_ids[index] = *attempt_id;
            attempts[index] = *attempt;
            trees[index] = *tree;
        }
        const auto validation_count = std::ranges::count_if(
            db_service->ExecutionDb()->WorkflowQueryService()
                ->GetWorkflowGraph(branch.cutscene_workflow_id)->steps,
            [](const auto& step) {
                return step.step_kind == "tasmovie.validate_tree"
                    && step.state == WorkflowStepState::Completed;
            });
        valid = valid
            && trees[0].parent_tas_movie_tree_id == branch.recording_tree_id
            && trees[1].parent_tas_movie_tree_id == attempts[0].output_tree_id
            && validation_count == 2;
        if (!valid) {
            fail_branch(branch, "Cutscene durable output or TAS tree lineage is incomplete");
            std::cout << "[cutscene-rtc-summary] rtc=" << branch.rtc
                      << " status=FAILED diagnostic=" << branch.diagnostic
                      << '\n';
            continue;
        }

        std::size_t seed_call_count = 0;
        if (options.capture_seed_calls) {
            for (const auto& attempt : attempts) {
                for (const auto& progress :
                    db_service->UiReadDb()->ListJobProgress(
                        attempt.source_job_id,
                        (std::numeric_limits<int>::max)())) {
                    if (progress.library_id
                        == "soa.progress.soa.seed_calls/1")
                        ++seed_call_count;
                }
            }
        }
        ++completed_count;
        std::cout << "[cutscene-rtc-summary] rtc=" << branch.rtc
                  << " status=COMPLETED battle_workflow="
                  << branch.battle_workflow_id
                  << " battle_set=" << branch.battle_set_id
                  << " victory_turn_job=" << branch.selected_turn_job_id
                  << " recording_workflow=" << branch.recording_workflow_id
                  << " recording_tree=" << branch.recording_tree_id
                  << " cutscene_workflow=" << branch.cutscene_workflow_id
                  << " cutscene_1_attempt=" << attempt_ids[0]
                  << " cutscene_1_endpoint=" << attempts[0].endpoint_kind
                  << " cutscene_2_attempt=" << attempt_ids[1]
                  << " cutscene_2_endpoint=" << attempts[1].endpoint_kind
                  << " seed_calls=" << seed_call_count << '\n';
    }

    std::cout << "[cutscene-fanout-summary] requested=" << branches.size()
              << " battle_complete=" << battle_count
              << " recorded=" << recorded_count
              << " cutscene_complete=" << completed_count
              << " failed=" << (branches.size() - completed_count)
              << " coordinator_warnings=" << warnings.size() << '\n';
    if (coordinator_failed)
        return Fail(coordinator_failure, error_out);
    if (completed_count != branches.size())
        return Fail("one or more Cutscene RTC branches failed", error_out);
    return true;
}

} // namespace savor::e2e
