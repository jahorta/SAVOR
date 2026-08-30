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
    std::string_view run_identity, std::int64_t* workflow_id_out,
    std::string* error_out) {
    const auto registry = savor::db::execution::workflow::
        BuildDefaultWorkflowUnitRegistry();
    const auto* unit = registry.Find("tas_movie_cutscene");
    if (!unit || unit->required_inputs.size() != 1)
        return Fail("tas_movie_cutscene workflow unit is unavailable", error_out);
    savor::db::SaveWorkflowGraphResult saved{};
    const savor::db::authoring::AuthoringRecipeMaterializer materializer(authoring);
    if (!materializer.SaveWorkflowGraph({
            .symbol = "e2e.cutscene",
            .name = "SavorE2E cutscene " + std::string(run_identity),
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
        .launch_key = "tasmovie-cutscene:" + std::to_string(tree_id),
        .input_bindings = {{
            .node_key = "cutscene_1", .input_key = "tas_movie_tree",
            .data_kind = "state.tas_movie_tree_id",
            .ref_kind = "state_tas_movie_tree", .ref_id = tree_id,
            .source_kind = "scenario"}},
    }, workflow_id_out, error_out);
}

} // namespace

bool RunTasMovieCutsceneRealWorkerScenario(
    const CliOptions& options, const ResolvedE2eScenarioEntry& entry,
    const char* argv0, savor::db::core::DBService* db_service,
    std::string* error_out) {
    if (entry.source != E2eScenarioEntrySource::FreshTasMovieValidation
        || !options.savestate_file.empty() || options.dtm_file.empty()
        || !options.tasmovie_rtc || options.worker_count != 1
        || !db_service || !db_service->IsRunning()) {
        return Fail("tasmovie_cutscene requires a fresh root DTM, exact RTC, one worker, and no savestate argument", error_out);
    }

    std::int64_t battle_workflow_id = 0;
    if (!RunBattleWorkflowGraphRealWorkerScenario(
            options, entry, argv0, db_service, error_out,
            &battle_workflow_id)) return false;
    const auto battle_set_id = FindSingleOutput(db_service->ExecutionDb(),
        battle_workflow_id, "analysis_battle.battle_set");
    if (!battle_set_id) return Fail(
        "cutscene E2E Battle workflow did not publish one BattleSet", error_out);
    const auto battle_set = db_service->AnalysisDb()->GetBattleSet(*battle_set_id);
    if (!battle_set || battle_set->status != savor::db::BattleSetStatus::Victory)
        return Fail("cutscene E2E BattleSet did not reach Victory", error_out);
    const auto selected = FindRecommendedCompletedVictory(
        db_service->AnalysisDb(), *battle_set_id, error_out);
    if (!selected) return false;
    const auto completion =
        db_service->AnalysisDb()->GetBattleCompletionForSelectedTurnJob(*selected);
    if (!completion || completion->status != "COMPLETED"
        || completion->route_kind != std::optional<std::string>("CUTSCENE")
        || !completion->transition_filename
        || completion->transition_filename->empty()) {
        return Fail("selected Battle Completion did not publish a CUTSCENE route",
            error_out);
    }

    savor::db::execution::workflow::BattleVictoryRecordingService recorder(
        db_service->AuthoringDb(), db_service->ExecutionDb(),
        db_service->AnalysisDb());
    savor::db::execution::workflow::RecordBattleVictoryReceipt recording{};
    if (!recorder.Record({.turn_job_id = *selected,
            .created_by = "SavorE2E"}, &recording, error_out)) return false;
    if (!recording.recording_workflow_created
        || recording.focused_existing_completion)
        return Fail("cutscene E2E did not launch the expected recording-only workflow", error_out);

    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::is_regular_file(worker_exe))
        return Fail("SavorWorker.exe was not found next to SavorE2E", error_out);
    const auto workspace_root = *options.workspace_root;
    std::string error;
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
        return Fail("failed building cutscene production registry: " + error, error_out);
    std::string iso_sha;
    try { iso_sha = hash::sha256_of_file(options.iso_path.string()); }
    catch (const std::exception& e) {
        return Fail("failed hashing cutscene ISO: " + std::string(e.what()), error_out);
    }
    savor::runtime::ArtifactCompatibilityToken compatibility{
        .game_id = std::string(savor::runtime::program::capabilities::kSupportedGameId),
        .iso_sha256 = std::move(iso_sha), .emulator_build = "dolphin-2506a",
        .runtime_revision = "worker-runtime-slice4"};
    savor::runner::parallel::savordb::WorkerCoordinatorConfig worker{
        .desired_workers = 1,
        .controller_sleep_ms = static_cast<std::uint32_t>(
            std::max<std::int64_t>(1, options.poll_ms)),
        .worker_start_timeout_ms = 60'000,
        .breakpoint_diagnostics = options.breakpoint_diagnostics,
        .worker_exe_path = worker_exe.string(),
        .iso_path = options.iso_path.string(),
        .dolphin_base_dir = options.dolphin_base_dir.string(),
        .worker_dir_root = options.worker_dir_root.value_or(
            workspace_root / ".workers").string(),
        .worker_binary_runtime_root = (workspace_root / "worker-runtime").string(),
        .worker_mode = options.visual_worker ? savor::runtime::WorkerMode::Visual
                                             : savor::runtime::WorkerMode::Headless,
        .runtime_artifact_root = (workspace_root / "runtime-artifacts").string()};
    const auto event_sink = [](const std::string& line) { std::cout << line << '\n'; };
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
        return Fail("cutscene coordinator startup failed: " + error, error_out);
    const auto barrier = WaitForInitialWorkerPool(options.wait_for_workers_ready,
        std::chrono::milliseconds(std::max<std::int64_t>(1, options.poll_ms)),
        [&]() { return runtime.SnapshotFleetStartup(); },
        [&](bool paused) { runtime.SetExecutionPaused(paused); }, event_sink);
    if (!barrier.satisfied) {
        std::string stop_error; (void)runtime.Stop(&stop_error);
        return Fail(barrier.diagnostic, error_out);
    }

    savor::db::execution::workflow::WorkflowGraphSnapshot recording_graph{};
    if (!WaitForWorkflow(runtime, db_service->ExecutionDb(),
            recording.workflow_instance_id,
            std::chrono::milliseconds(std::max<std::int64_t>(1, options.poll_ms)),
            "Battle recording", &recording_graph, &error)) {
        std::string stop_error; (void)runtime.Stop(&stop_error);
        return Fail(error, error_out);
    }
    const auto recording_id = FindSingleOutput(db_service->ExecutionDb(),
        recording.workflow_instance_id, "analysis_battle.battle_recording");
    const auto recording_record = recording_id
        ? db_service->AnalysisDb()->GetBattleRecording(*recording_id)
        : std::nullopt;
    if (!recording_record || recording_record->status != "COMPLETED"
        || !recording_record->tas_movie_tree_id) {
        std::string stop_error; (void)runtime.Stop(&stop_error);
        return Fail("Battle recording did not publish a completed TAS movie tree", error_out);
    }

    std::int64_t cutscene_workflow_id = 0;
    if (!LaunchCutscene(db_service->AuthoringDb(), db_service->ExecutionDb(),
            *recording_record->tas_movie_tree_id, entry.run_identity,
            &cutscene_workflow_id, &error)) {
        std::string stop_error; (void)runtime.Stop(&stop_error);
        return Fail("failed launching cutscene workflow: " + error, error_out);
    }
    savor::db::execution::workflow::WorkflowGraphSnapshot cutscene_graph{};
    const bool cutscene_ok = WaitForWorkflow(runtime, db_service->ExecutionDb(),
        cutscene_workflow_id,
        std::chrono::milliseconds(std::max<std::int64_t>(1, options.poll_ms)),
        "Cutscene", &cutscene_graph, &error);
    std::string stop_error;
    const bool stopped = runtime.Stop(&stop_error);
    if (!cutscene_ok) return Fail(error, error_out);
    if (!stopped) return Fail("cutscene coordinator shutdown failed: " + stop_error, error_out);

    const std::array<std::string_view, 2> nodes{"cutscene_1", "cutscene_2"};
    std::array<std::int64_t, 2> attempt_ids{};
    std::array<savor::db::TasMovieCutsceneAttemptRecord, 2> attempts{};
    std::array<savor::db::TasMovieTreeRecord, 2> trees{};
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto attempt_id = FindNodeOutput(db_service->ExecutionDb(),
            cutscene_workflow_id, nodes[index], "tmv_cutscene_attempt");
        const auto attempt = attempt_id
            ? db_service->AnalysisDb()->GetTasMovieCutsceneAttempt(*attempt_id)
            : std::nullopt;
        const auto tree = attempt && attempt->output_tree_id
            ? db_service->StateDb()->GetTasMovieTree(*attempt->output_tree_id)
            : std::nullopt;
        const auto checkpoint = attempt && attempt->output_savestate_id
            ? db_service->StateDb()->GetSavestate(*attempt->output_savestate_id)
            : std::nullopt;
        if (!attempt_id || !attempt || !attempt->succeeded
            || attempt->endpoint_kind.empty()
            || attempt->final_movie_input_cursor <= attempt->checkpoint_movie_input_cursor
            || !attempt->output_dtm_artifact_id || !tree || !checkpoint
            || !checkpoint->is_complete || !checkpoint->dtm_artifact_id
            || *checkpoint->dtm_artifact_id != tree->dtm_artifact_id
            || tree->source_context_kind != "CUTSCENE")
            return Fail("cutscene durable output is incomplete for node "
                + std::string(nodes[index]), error_out);
        attempt_ids[index] = *attempt_id;
        attempts[index] = *attempt;
        trees[index] = *tree;
    }
    if (trees[0].parent_tas_movie_tree_id
            != recording_record->tas_movie_tree_id
        || trees[1].parent_tas_movie_tree_id
            != attempts[0].output_tree_id) {
        return Fail("chained cutscene TAS tree lineage is invalid", error_out);
    }
    const auto validation_count = std::ranges::count_if(cutscene_graph.steps,
        [](const auto& step) { return step.step_kind == "tasmovie.validate_tree"
            && step.state == WorkflowStepState::Completed; });
    if (validation_count != 2)
        return Fail("each chained cutscene node must complete internal validation",
            error_out);

    std::size_t seed_call_count = 0;
    if (options.capture_seed_calls) {
        for (std::size_t index = 0; index < attempts.size(); ++index) {
            for (const auto& progress : db_service->UiReadDb()->ListJobProgress(
                    attempts[index].source_job_id,
                    (std::numeric_limits<int>::max)())) {
                if (progress.library_id != "soa.progress.soa.seed_calls/1")
                    continue;
                ++seed_call_count;
                std::cout << "[cutscene-seed-call] node=" << nodes[index]
                          << " job=" << attempts[index].source_job_id
                          << " invocation=" << progress.invocation_id
                          << " ordinal=" << progress.ordinal
                          << " " << progress.display_text << '\n';
            }
        }
    }

    const auto victory = db_service->AnalysisDb()->GetBattleTurnJob(*selected);
    std::cout << "[cutscene-summary] battle_workflow=" << battle_workflow_id
              << " battle_set=" << *battle_set_id
              << " victory_turn_job=" << *selected
              << " completion_route=" << *completion->route_kind
              << " transition_file=" << *completion->transition_filename
              << " ending_rng=" << (victory && victory->rng_seed
                    ? std::to_string(*victory->rng_seed) : "unknown")
              << " recording_workflow=" << recording.workflow_instance_id
              << " recording_tree=" << *recording_record->tas_movie_tree_id
              << " cutscene_workflow=" << cutscene_workflow_id
              << " cutscene_1_attempt=" << attempt_ids[0]
              << " cutscene_1_endpoint=" << attempts[0].endpoint_kind
              << " cutscene_1_tree=" << *attempts[0].output_tree_id
              << " cutscene_2_attempt=" << attempt_ids[1]
              << " cutscene_2_endpoint=" << attempts[1].endpoint_kind
              << " cutscene_2_tree=" << *attempts[1].output_tree_id
              << " seed_calls=" << seed_call_count << '\n';
    return true;
}

} // namespace savor::e2e
