#include "BattleSingleTurnScenario.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Common/DbService.h"
#include "Common/Types/UtcTimestamp.h"
#include "Core/Input/SoaBattle/ActionTypes.h"
#include "Core/Memory/Soa/SoaAddrProgramBuilder.h"
#include "Core/Memory/Soa/SoaAddrCatalog.h"
#include "CoordinatorProgress.h"
#include "DbSetup.h"
#include "DurableLogFile.h"
#include "Execution/ProgramDB/BattleContext/BattleContextProbePhaseRegistration.h"
#include "Execution/ProgramDB/BattleSingleTurn/BattleSingleTurnPhaseRegistration.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbePhaseRegistration.h"
#include "Execution/ProgramDB/TasMovie/TasMoviePhaseRegistration.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Phases/Programs/PlayTasMovie/TasMoviePayload.h"
#include "Runner/Parallel/SimCoreDB/DBWorkflowCoordinatorFactory.h"
#include "Runner/Parallel/SimCoreDB/DBWorkflowWorkerCoordinator.h"
#include "Runner/Breakpoints/BPRegistry.h"
#include "Runner/Breakpoints/Predicate.h"
#include "Runner/IPC/Wire.h"
#include "Tas/DtmFile.h"
#include "MultiLineProgressRenderer.h"

namespace simcore::e2e {
namespace {

constexpr const char* kWaveRefKind = "analysis_battle.turn_wave";

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

std::int64_t ComputeBattleScenarioTimeoutMs(const simcore::db::BattleRunSpecSnapshot& run_spec, const CliOptions& options) {
    const int fake_jobs_per_wave = std::max(1, std::abs(run_spec.max_fake_attacks - run_spec.min_fake_attacks) + 1);
    const std::int64_t per_wave_budget = static_cast<std::int64_t>(std::max(1, fake_jobs_per_wave))
        * std::max<std::int64_t>(1000, static_cast<std::int64_t>(run_spec.run_ms));
    return std::max<std::int64_t>(options.timeout_ms, (per_wave_budget * 4) + options.timeout_ms);
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

std::vector<std::uint8_t> BuildCurrentTurnAddressProgram() {
    addrprog::Builder builder;
    builder.op_base_key(addr::derived::battle::CurrentTurn);
    builder.op_end();
    return builder.blob();
}

std::int64_t ComputeSeedProbePreludeTimeoutMs(const CliOptions& options) {
    const std::int64_t grid_probe_count =
        static_cast<std::int64_t>(kSeedProbeSamplesPerAxis)
        * static_cast<std::int64_t>(kSeedProbeSamplesPerAxis)
        * 3;
    constexpr std::int64_t kAverageUniqueCountEstimate = 25;
    return std::max<std::int64_t>(
        options.timeout_ms,
        options.timeout_ms * (1 + grid_probe_count + kAverageUniqueCountEstimate));
}

bool RunSeedProbePrelude(
    const CliOptions& options,
    const std::filesystem::path& worker_exe,
    simcore::db::core::DBService* db_service,
    DurableLogFile* durable_log,
    std::int64_t* entry_savestate_id_out,
    std::int64_t* unique_seed_id_out,
    std::string* error_out) {
    if (db_service == nullptr || entry_savestate_id_out == nullptr || unique_seed_id_out == nullptr) {
        if (error_out) *error_out = "seed probe prelude args unavailable";
        return false;
    }

    std::string err;
    std::int64_t entry_savestate_id = 0;
    if (!SeedStateSavestate(db_service->StateDb(), options.savestate_file, &entry_savestate_id, &err)) {
        if (error_out) *error_out = "failed seeding StateDB seedprobe entry savestate: " + err;
        return false;
    }

    std::int64_t seed_probe_spec_id = 0;
    if (!SeedAuthoringSpec(db_service->AuthoringDb(), &seed_probe_spec_id, &err)) {
        if (error_out) *error_out = "failed seeding SeedProbe authoring spec: " + err;
        return false;
    }

    std::int64_t seedprobe_workflow_instance_id = 0;
    std::int64_t probe_run_id = 0;
    if (!SeedWorkflowGraphExecution(
            db_service->AuthoringDb(),
            db_service->ExecutionDb(),
            entry_savestate_id,
            seed_probe_spec_id,
            &seedprobe_workflow_instance_id,
            &err)) {
        if (error_out) *error_out = "failed seeding SeedProbe workflow: " + err;
        return false;
    }

    simcore::db::execution::programdb::ProgramKindRegistry registry;
    simcore::db::execution::programdb::seedprobe::SeedProbePhaseRegistrationConfig seed_config{};
    seed_config.authoring_db = db_service->AuthoringDb();
    simcore::db::execution::programdb::seedprobe::RegisterSeedProbePhaseDescriptors(
        &registry,
        db_service->ExecutionDb(),
        db_service->AnalysisDb(),
        std::move(seed_config));

    const auto battle_worker_dir_root = options.worker_dir_root.value_or(
        options.workspace_root.value_or(std::filesystem::temp_directory_path() / "simcoredbe2e-default") / ".workers");

    auto coordinator = simcore::runner::parallel::simcoredb::BuildDbBackedWorkflowCoordinator(
        db_service->ExecutionDb(),
        db_service->StateDb(),
        simcore::runner::parallel::simcoredb::DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 5u,
            .controller_sleep_ms = static_cast<std::uint32_t>(options.poll_ms),
            .worker_exe_path = worker_exe.string(),
            .iso_path = options.iso_path.string(),
            .dolphin_base_dir = options.dolphin_base_dir.string(),
            .worker_dir_root = battle_worker_dir_root.string(),
            .visual_workers = options.visual_worker,
            .auto_resume_visual_workers = false,
            .visual_screenshot_dir = options.visual_screenshot_dir.value_or(
                options.workspace_root.value_or(std::filesystem::temp_directory_path() / "simcoredbe2e-default")
                    / "visual-screenshots").string(),
        },
        simcore::runner::parallel::simcoredb::CoordinatorIntegrationConfig{},
        &registry);

    std::mutex lines_mtx;
    std::deque<std::string> pending_lines;
    const auto push_line = [&](std::string line) {
        if (durable_log != nullptr) {
            durable_log->AppendLine(line);
        }
        std::lock_guard<std::mutex> lock(lines_mtx);
        pending_lines.push_back(std::move(line));
    };
    const auto drain_lines = [&]() {
        std::deque<std::string> out;
        std::lock_guard<std::mutex> lock(lines_mtx);
        std::swap(out, pending_lines);
        return out;
    };
    coordinator.SetResultCallback([&](const simcore::PRResult& result) {
        std::ostringstream line;
        line << "[battle-seedprobe-worker-result] job=" << result.job_id
             << " worker=" << result.worker_id
             << " ok=" << (result.ps.ok ? "true" : "false")
             << " w_err=" << simcore::WErrToString(result.ps.w_err) << "(" << static_cast<int>(result.ps.w_err) << ")";
        push_line(line.str());
    });
    coordinator.SetResultMapEventCallback([&](const std::string& line) {
        push_line(line);
    });

    ScopedWorkflowCoordinatorService workflow_coordinator;
    if (!workflow_coordinator.Start(
            db_service->ExecutionDb(),
            &registry,
            options,
            &err,
            [&](const std::string& line) {
                push_line(line);
            })) {
        if (error_out) *error_out = err;
        return false;
    }

    coordinator.Start();
    const auto started = std::chrono::steady_clock::now();
    const auto timeout_ms = ComputeSeedProbePreludeTimeoutMs(options);
    const bool interactive_stdout = IsInteractiveStdout();
    MultiLineProgressRenderer progress_renderer;
    const auto interactive_refresh_cadence = std::chrono::milliseconds(100);
    bool completed = false;
    bool failed = false;
    std::string latest_state = "workflow=unavailable";
    std::vector<std::string> latest_lines;
    std::size_t poll_count = 0;
    std::size_t ticks_since_snapshot = 0;
    std::size_t terminal_steps_seen_count = 0;
    while (std::chrono::steady_clock::now() - started < std::chrono::milliseconds(timeout_ms)) {
        ++poll_count;
        ++ticks_since_snapshot;
        const auto event_lines = drain_lines();
        const auto graph = db_service->ExecutionDb()->WorkflowQueryService()->GetWorkflowGraph(seedprobe_workflow_instance_id);
        latest_lines = BuildCoordinatorProgressLines(
            db_service->ExecutionDb(),
            coordinator.SnapshotTelemetry(),
            coordinator.SnapshotWorkers(),
            graph);
        if (interactive_stdout) {
            progress_renderer.SetLines(latest_lines);
            progress_renderer.WriteEventLines(std::cout, event_lines);
            progress_renderer.RenderIfDue(std::cout, std::chrono::steady_clock::now(), interactive_refresh_cadence);
        } else {
            for (const auto& line : event_lines) {
                std::cout << line << '\n';
            }
            if (ticks_since_snapshot >= 10 || poll_count == 1) {
                ticks_since_snapshot = 0;
                std::cout << "[battle-seedprobe] ";
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
            latest_state = FormatWorkflowStateLine(*graph);
            using simcore::db::execution::workflow::WorkflowInstanceState;
            if (graph->instance.state == WorkflowInstanceState::Completed) {
                completed = true;
                break;
            }
            if (graph->instance.state == WorkflowInstanceState::Failed
                || graph->instance.state == WorkflowInstanceState::Canceled) {
                failed = true;
                break;
            }
            if (AreWorkflowStepsTerminal(*graph)) {
                ++terminal_steps_seen_count;
                if (terminal_steps_seen_count >= 2) {
                    if (HasFailedWorkflowStep(*graph)) {
                        failed = true;
                    } else {
                        completed = true;
                    }
                    break;
                }
            } else {
                terminal_steps_seen_count = 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
    }
    coordinator.Stop();
    workflow_coordinator.Stop();
    const auto final_event_lines = drain_lines();
    if (interactive_stdout) {
        progress_renderer.WriteEventLines(std::cout, final_event_lines);
    } else {
        for (const auto& line : final_event_lines) {
            std::cout << line << '\n';
        }
    }
    const auto final_graph = db_service->ExecutionDb()->WorkflowQueryService()->GetWorkflowGraph(seedprobe_workflow_instance_id);
    latest_lines = BuildCoordinatorProgressLines(
        db_service->ExecutionDb(),
        coordinator.SnapshotTelemetry(),
        coordinator.SnapshotWorkers(),
        final_graph);
    if (final_graph.has_value()) {
        latest_state = FormatWorkflowStateLine(*final_graph);
    }
    if (interactive_stdout) {
        progress_renderer.SetLines(latest_lines);
        progress_renderer.Render(std::cout);
    }
    if (final_graph.has_value()) {
        for (const auto& step : final_graph->steps) {
            if ((step.step_key == "probe_1" || step.step_kind == "seed_probe_chain")
                && step.input_ref_kind.has_value()
                && *step.input_ref_kind == "sp_probe_run"
                && step.input_ref_id.has_value()
                && *step.input_ref_id > 0) {
                probe_run_id = *step.input_ref_id;
                break;
            }
        }
    }

    std::cout << "[battle-seedprobe-final] status=" << (completed ? "success" : failed ? "failure" : "timeout") << '\n';
    std::cout << "  timeout_ms=" << timeout_ms << '\n';
    std::cout << "  " << latest_state << '\n';
    if (!completed) {
        if (error_out) *error_out = failed
            ? "SeedProbe prelude workflow did not complete successfully"
            : "SeedProbe prelude workflow did not reach COMPLETED state before timeout";
        return false;
    }

    const auto unique_rows = db_service->AnalysisDb()->ListSeedProbeUniqueSeeds(probe_run_id);
    if (unique_rows.empty()) {
        if (error_out) *error_out = "SeedProbe prelude completed with no unique seeds";
        return false;
    }
    *entry_savestate_id_out = entry_savestate_id;
    *unique_seed_id_out = unique_rows.front().unique_seed_id;
    std::cout << "[battle-seedprobe-selected] probe_run_id=" << probe_run_id
              << " unique_seed_id=" << *unique_seed_id_out
              << " unique_count=" << unique_rows.size() << '\n';
    return true;
}

bool SeedBattleAuthoringRows(
    simcore::db::IAuthoringDb* authoring_db,
    int min_fake_attacks,
    int max_fake_attacks,
    bool require_electribox_drop,
    std::int64_t* battle_run_spec_id_out,
    std::int64_t* explorer_settings_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || battle_run_spec_id_out == nullptr || explorer_settings_id_out == nullptr) {
        if (error_out) *error_out = "authoring db unavailable";
        return false;
    }
    const auto now = simcore::db::types::UtcNow();

    if (!authoring_db->SaveBattleRunSpec(
            {
                .name = "SimCoreDBe2e first battle single-turn",
                .priority = 1,
                .run_ms = 10000,
                .vi_stall_ms = 2000,
                .progress_enable = true,
                .use_single_turn_runner = true,
                .auto_wave_trigger_enable = true,
                .min_fake_attacks = min_fake_attacks,
                .max_fake_attacks = max_fake_attacks,
                .created_at_utc = now,
                .event_id = "simcoredbe2e.battle.authoring.run_spec",
                .correlation_id = "simcoredbe2e.battle",
                .causation_id = "simcoredbe2e.seed",
            },
            battle_run_spec_id_out,
            error_out)) {
        return false;
    }

    std::int64_t plan_id = 0;
    if (!authoring_db->SavePlan(
            {
                .name = "SimCoreDBe2e battle plan",
                .fingerprint = "simcoredbe2e-battle-plan-v1",
                .num_turns = 2,
                .created_at_utc = now,
                .event_id = "simcoredbe2e.battle.authoring.plan",
                .correlation_id = "simcoredbe2e.battle",
                .causation_id = "simcoredbe2e.seed",
            },
            &plan_id,
            error_out)) {
        return false;
    }

    std::int64_t attack_any_enemy_preset_id = 0;
    if (!authoring_db->SaveBattlePlanActionPreset(
            {
                .name = "SimCoreDBe2e attack any enemy",
                .macro = soa::battle::actions::BattleAction::Attack,
                .target_kind = simcore::db::BattlePlanTargetKind::AnyEnemy,
                .created_at_utc = now,
                .event_id = "simcoredbe2e.battle.authoring.action_preset.attack_any_enemy",
                .correlation_id = "simcoredbe2e.battle",
                .causation_id = "plan-" + std::to_string(plan_id),
            },
            &attack_any_enemy_preset_id,
            error_out)) {
        return false;
    }

    std::int64_t attack_same_target_preset_id = 0;
    if (!authoring_db->SaveBattlePlanActionPreset(
            {
                .name = "SimCoreDBe2e attack same target as PC0",
                .macro = soa::battle::actions::BattleAction::Attack,
                .target_kind = simcore::db::BattlePlanTargetKind::SameAsOtherPC,
                .target_same_as_actor_slot = 0,
                .created_at_utc = now,
                .event_id = "simcoredbe2e.battle.authoring.action_preset.attack_same_target",
                .correlation_id = "simcoredbe2e.battle",
                .causation_id = "plan-" + std::to_string(plan_id),
            },
            &attack_same_target_preset_id,
            error_out)) {
        return false;
    }

    std::int64_t turn_id = 0;
    if (!authoring_db->SaveBattlePlanTurn(
            {
                .plan_id = plan_id,
                .turn_index = 1,
                .actions = {
                    {
                        .actor_slot = 0,
                        .action_preset_id = attack_any_enemy_preset_id,
                        .ordinal = 0,
                    },
                    {
                        .actor_slot = 1,
                        .action_preset_id = attack_same_target_preset_id,
                        .ordinal = 1,
                    }
                },
                .created_at_utc = now,
                .event_id = "simcoredbe2e.battle.authoring.plan_turn_1",
                .correlation_id = "simcoredbe2e.battle",
                .causation_id = "plan-" + std::to_string(plan_id),
            },
            &turn_id,
            error_out)) {
        return false;
    }

    if (!authoring_db->SaveBattlePlanTurn(
            {
                .plan_id = plan_id,
                .turn_index = 2,
                .actions = {
                    {
                        .actor_slot = 0,
                        .action_preset_id = attack_any_enemy_preset_id,
                        .ordinal = 0,
                    },
                    {
                        .actor_slot = 1,
                        .action_preset_id = attack_same_target_preset_id,
                        .ordinal = 1,
                    },
                },
                .created_at_utc = now,
                .event_id = "simcoredbe2e.battle.authoring.plan_turn_2",
                .correlation_id = "simcoredbe2e.battle",
                .causation_id = "plan-" + std::to_string(plan_id),
            },
            &turn_id,
            error_out)) {
        return false;
    }

    std::vector<std::int64_t> predicate_spec_ids;
    if (require_electribox_drop) {
        std::string desc;
        addrprog::Builder builder;
        addrprog::catalog::item_drop_amt(builder, 273, desc);
        std::int64_t address_program_id = 0;
        if (!authoring_db->EnsureAddressProgram(
                {
                    .program_version = static_cast<int>(addrprog::PROG_VERSION),
                    .prog_bytes = builder.blob(),
                    .derived_buffer_version = 1,
                    .description = desc,
                },
                &address_program_id,
                error_out)) {
            return false;
        }

        std::int64_t electribox_predicate_spec_id = 0;
        if (!authoring_db->SavePredicateSpec(
                {
                    .name = "SimCoreDBe2e one electribox per turn",
                    .breakpoint_id = bp::battle::EndTurn,
                    .lhs_value = 0,
                    .rhs_value = static_cast<std::int64_t>(addr::battle::CurrentTurn),
                    .cmp_op = simcore::db::PredicateComparisonOp::EQ,
                    .width = 1,
                    .flag_mask = static_cast<std::int64_t>(
                        static_cast<std::uint32_t>(simcore::pred::PredFlag::Active)
                        | static_cast<std::uint32_t>(simcore::pred::PredFlag::LhsIsProg)
                        | static_cast<std::uint32_t>(simcore::pred::PredFlag::RhsIsKey)),
                    .lhs_address_program_id = address_program_id,
                    .abort_on_fail = true,
                    .created_at_utc = now,
                    .event_id = "simcoredbe2e.battle.authoring.predicate.1",
                    .correlation_id = "simcoredbe2e.battle",
                    .causation_id = "address-program-" + std::to_string(address_program_id),
                },
                &electribox_predicate_spec_id,
                error_out)) {
            return false;
        }
        predicate_spec_ids.push_back(electribox_predicate_spec_id);
    }

    std::int64_t turn_order_predicate_spec_id = 0;
    if (!authoring_db->SavePredicateSpec(
        {
            .name = "SimCoreDBe2e players before enemies",
            .breakpoint_id = bp::battle::TurnIsReady,
            .lhs_value = static_cast<std::int64_t>(addr::derived::battle::TurnOrderPcMax),
            .rhs_value = static_cast<std::int64_t>(addr::derived::battle::TurnOrderEcMin),
            .cmp_op = simcore::db::PredicateComparisonOp::LT,
            .width = 1,
            .flag_mask = static_cast<std::int64_t>(
                static_cast<std::uint32_t>(simcore::pred::PredFlag::Active)
                | static_cast<std::uint32_t>(simcore::pred::PredFlag::LhsIsKey)
                | static_cast<std::uint32_t>(simcore::pred::PredFlag::RhsIsKey)),
            .abort_on_fail = false,
            .created_at_utc = now,
            .event_id = "simcoredbe2e.battle.authoring.predicate.2",
            .correlation_id = "simcoredbe2e.battle",
            .causation_id = "simcoredbe2e.pred2",
        },
        &turn_order_predicate_spec_id,
        error_out)) {
        return false;
    }
    predicate_spec_ids.push_back(turn_order_predicate_spec_id);

    std::int64_t predicate_set_id = 0;
    if (!authoring_db->SavePredicateSet(
            {
                .predicate_spec_ids = predicate_spec_ids,
                .created_at_utc = now,
            },
            &predicate_set_id,
            error_out)) {
        return false;
    }

    return authoring_db->SaveExplorerSettings(
        {
            .name = "SimCoreDBe2e first battle settings",
            .description = "First battle single-turn e2e setup",
            .default_plan_id = plan_id,
            .default_predicate_set_id = predicate_set_id,
            .created_at_utc = now,
            .event_id = "simcoredbe2e.battle.authoring.settings",
            .correlation_id = "simcoredbe2e.battle",
            .causation_id = "plan-" + std::to_string(plan_id),
        },
        explorer_settings_id_out,
        error_out);
}

bool SeedBattleAnalysisAndWorkflowRows(
    simcore::db::IAnalysisDb* analysis_db,
    simcore::db::IExecutionDb* execution_db,
    std::int64_t entry_savestate_id,
    std::int64_t battle_run_spec_id,
    std::int64_t explorer_settings_id,
    std::int64_t source_unique_seed_id,
    std::int64_t* battle_set_id_out,
    std::int64_t* wave_id_out,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (analysis_db == nullptr || execution_db == nullptr) {
        if (error_out) *error_out = "analysis/execution db unavailable";
        return false;
    }
    const auto now = simcore::db::types::UtcNow();

    std::int64_t battle_set_id = 0;
    if (!analysis_db->CreateBattleSet(
            {
                .name = "SimCoreDBe2e first battle set",
                .entry_savestate_id = entry_savestate_id,
                .battle_run_spec_id = battle_run_spec_id,
                .explorer_settings_id = explorer_settings_id,
                .status = simcore::db::BattleSetStatus::Active,
                .created_at_utc = now,
                .event_id = "simcoredbe2e.battle.analysis.battle_set",
                .correlation_id = "simcoredbe2e.battle",
                .causation_id = "simcoredbe2e.seed",
            },
            &battle_set_id,
            error_out)) {
        return false;
    }

    std::int64_t seed_candidate_id = 0;
    if (!analysis_db->AddBattleSeedCandidate(
            {
                .battle_set_id = battle_set_id,
                .source_unique_seed_id = source_unique_seed_id,
                .seed_value = 0,
                .source_kind = simcore::db::BattleSeedCandidateSourceKind::SeedProbeUnique,
                .candidate_status = simcore::db::BattleSeedCandidateStatus::Ready,
                .created_at_utc = now,
                .event_id = "simcoredbe2e.battle.analysis.seed_candidate",
                .correlation_id = "simcoredbe2e.battle",
                .causation_id = "unique-seed-" + std::to_string(source_unique_seed_id),
            },
            &seed_candidate_id,
            error_out)) {
        return false;
    }

    std::int64_t wave_id = 0;
    if (!analysis_db->CreateBattleTurnWave(
            {
                .battle_set_id = battle_set_id,
                .turn_index = 1,
                .seed_candidate_id = seed_candidate_id,
                .status = simcore::db::BattleTurnWaveStatus::Ready,
                .created_at_utc = now,
                .event_id = "simcoredbe2e.battle.analysis.turn_wave_1",
                .correlation_id = "simcoredbe2e.battle",
                .causation_id = "seed-candidate-" + std::to_string(seed_candidate_id),
            },
            &wave_id,
            error_out)) {
        return false;
    }

    simcore::db::execution::workflow::WorkflowCreateInstanceCommand workflow{};
    workflow.workflow_kind = "BATTLE_SINGLE_TURN_CHAIN";
    workflow.root_scope_kind = "run";
    workflow.root_scope_id = battle_set_id;
    workflow.created_by = "simcoredbe2e";
    workflow.created_at_utc = now.time_since_epoch().count();
    simcore::db::execution::workflow::WorkflowCreateStepSpec step{};
    step.step_key = "BattleContext/t1/w" + std::to_string(wave_id);
    step.step_kind = "battle.context_probe";
    step.priority = 1;
    step.max_attempts = 1;
    step.input_ref_kind = std::string(kWaveRefKind);
    step.input_ref_id = wave_id;
    workflow.steps.push_back(std::move(step));
    if (!execution_db->CreateWorkflowInstance(workflow, workflow_instance_id_out, error_out)) {
        return false;
    }

    if (battle_set_id_out) *battle_set_id_out = battle_set_id;
    if (wave_id_out) *wave_id_out = wave_id;
    return true;
}

bool SeedTasMovieSeedProbeBattleGraphExecution(
    simcore::db::IAuthoringDb* authoring_db,
    simcore::db::IExecutionDb* execution_db,
    std::int64_t dtm_artifact_id,
    std::int64_t seed_probe_spec_id,
    std::int64_t battle_chain_spec_id,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || execution_db == nullptr || workflow_instance_id_out == nullptr) {
        if (error_out) *error_out = "authoring/execution db unavailable";
        return false;
    }
    if (dtm_artifact_id <= 0 || seed_probe_spec_id <= 0 || battle_chain_spec_id <= 0) {
        if (error_out) *error_out = "dtm artifact, seed probe spec, and battle chain spec ids must be > 0";
        return false;
    }

    simcore::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = "SimCoreDBe2e TAS SeedProbe Battle graph",
                .description = "TasMovie -> SeedProbe -> Battle graph-style e2e scenario",
                .graph_version = 1,
                .graph_hash = "simcoredbe2e.workflow_graph.tasmovie_seedprobe_battle.v1",
                .nodes = {
                    {
                        .node_key = "tas_1",
                        .unit_kind = "tas_movie",
                        .display_name = "TAS Movie",
                        .inputs = {
                            { .input_key = "dtm_artifact", .data_kind = "state_artifact.dtm_artifact_id", .display_name = "DTM artifact" },
                        },
                        .possible_outputs = {
                            { .output_key = "output_savestate", .data_kind = "state.savestate_id", .display_name = "Output savestate" },
                        },
                    },
                    {
                        .node_key = "probe_1",
                        .unit_kind = "seed_probe_chain",
                        .display_name = "Seed Probe Chain",
                        .authored_ref_kind = std::string("seed_probe_spec"),
                        .authored_ref_id = seed_probe_spec_id,
                        .inputs = {
                            { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .display_name = "Entry savestate" },
                        },
                        .possible_outputs = {
                            { .output_key = "unique_input_frames", .data_kind = "analysis.input_frame_set_id", .display_name = "Unique input frames" },
                        },
                    },
                    {
                        .node_key = "battle_1",
                        .unit_kind = "battle_chain",
                        .display_name = "Battle Chain",
                        .authored_ref_kind = std::string("authoring.battle_chain_spec"),
                        .authored_ref_id = battle_chain_spec_id,
                        .inputs = {
                            { .input_key = "entry_savestate", .data_kind = "state.savestate_id", .display_name = "Entry savestate" },
                            { .input_key = "initial_input_frames", .data_kind = "analysis.input_frame_set_id", .display_name = "Initial input frames" },
                        },
                        .possible_outputs = {
                            { .output_key = "battle_context", .data_kind = "analysisbattle.context_probe", .display_name = "Battle context" },
                        },
                    },
                },
                .edges = {
                    { .from_node_key = "tas_1", .output_key = "output_savestate", .to_node_key = "probe_1", .input_key = "entry_savestate" },
                    { .from_node_key = "tas_1", .output_key = "output_savestate", .to_node_key = "battle_1", .input_key = "entry_savestate" },
                    { .from_node_key = "probe_1", .output_key = "unique_input_frames", .to_node_key = "battle_1", .input_key = "initial_input_frames" },
                },
                .created_at_utc = simcore::db::types::UtcNow(),
                .event_id = "simcoredbe2e.authoring.workflow_graph.tasmovie_seedprobe_battle",
                .correlation_id = "simcoredbe2e.workflow_graph.tasmovie_seedprobe_battle",
                .causation_id = "simcoredbe2e.seed",
            },
            &saved,
            error_out)) {
        return false;
    }

    simcore::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph_tasmovie_seedprobe_battle";
    command.root_scope_kind = "manual";
    command.root_scope_id = dtm_artifact_id;
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "simcoredbe2e";
    command.created_at_utc = simcore::db::types::UtcNow().time_since_epoch().count();
    command.steps.push_back({ .step_key = "tas_1", .step_kind = "tas_movie", .priority = 1, .max_attempts = 1 });
    command.input_bindings.push_back({
        .node_key = "tas_1",
        .input_key = "dtm_artifact",
        .data_kind = "state_artifact.dtm_artifact_id",
        .ref_kind = "state_artifact",
        .ref_id = dtm_artifact_id,
        .source_kind = "external",
    });
    command.arguments.push_back({ .node_key = "tas_1", .argument_key = "rtc", .value_type = "integer", .integer_value = 4, .source_kind = "scenario" });
    command.arguments.push_back({ .node_key = "battle_1", .argument_key = "fake_attack_min", .value_type = "integer", .integer_value = 22, .source_kind = "scenario" });
    command.arguments.push_back({ .node_key = "battle_1", .argument_key = "fake_attack_max", .value_type = "integer", .integer_value = 25, .source_kind = "scenario" });
    return execution_db->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
}

std::int64_t ResolveProbeRunIdFromGraph(
    const std::optional<simcore::db::execution::workflow::WorkflowGraphSnapshot>& graph) {
    if (!graph.has_value()) {
        return 0;
    }
    for (const auto& step : graph->steps) {
        if ((step.step_key == "probe_1" || step.step_key == "probe_1/Unique")
            && step.input_ref_kind.has_value()
            && *step.input_ref_kind == "sp_probe_run"
            && step.input_ref_id.has_value()
            && *step.input_ref_id > 0) {
            return *step.input_ref_id;
        }
    }
    return 0;
}

std::int64_t ResolveBattleContextProbeIdFromGraph(
    const std::optional<simcore::db::execution::workflow::WorkflowGraphSnapshot>& graph) {
    if (!graph.has_value()) {
        return 0;
    }
    for (const auto& step : graph->steps) {
        if (step.step_key == "battle_1"
            && step.output_ref_kind.has_value()
            && *step.output_ref_kind == "analysisbattle.context_probe"
            && step.output_ref_id.has_value()
            && *step.output_ref_id > 0) {
            return *step.output_ref_id;
        }
    }
    return 0;
}

} // namespace

bool RunBattleSingleTurnRealWorkerScenario(
    const CliOptions& options,
    const char* argv0,
    simcore::db::core::DBService* db_service,
    std::string* error_out) {
    if (db_service == nullptr) {
        if (error_out) *error_out = "DBService is null";
        return false;
    }
    if (db_service->ExecutionDb() == nullptr
        || db_service->StateDb() == nullptr
        || db_service->AnalysisDb() == nullptr
        || db_service->AuthoringDb() == nullptr) {
        if (error_out) *error_out = "one or more SimCoreDB contexts are unavailable";
        return false;
    }

    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::exists(worker_exe)) {
        if (error_out) *error_out = "SimCoreWorker.exe was not found next to SimCoreDBe2e: " + worker_exe.string();
        return false;
    }

    DurableLogFile durable_log;
    if (!durable_log.Open(options, options.scenario, error_out)) {
        return false;
    }
    std::cout << "[durable-log] path=" << durable_log.path().string() << '\n';

    std::int64_t entry_savestate_id = 0;
    std::int64_t unique_seed_id = 0;
    std::string err;
    if (!RunSeedProbePrelude(
            options,
            worker_exe,
            db_service,
            &durable_log,
            &entry_savestate_id,
            &unique_seed_id,
            &err)) {
        if (error_out) *error_out = "failed running SeedProbe prelude for battle scenario: " + err;
        return false;
    }

    std::int64_t battle_run_spec_id = 0;
    std::int64_t explorer_settings_id = 0;
    if (!SeedBattleAuthoringRows(
            db_service->AuthoringDb(),
            0,
            2,
            true,
            &battle_run_spec_id,
            &explorer_settings_id,
            &err)) {
        if (error_out) *error_out = "failed seeding battle authoring rows: " + err;
        return false;
    }

    std::int64_t battle_set_id = 0;
    std::int64_t wave_id = 0;
    std::int64_t workflow_instance_id = 0;
    if (!SeedBattleAnalysisAndWorkflowRows(
            db_service->AnalysisDb(),
            db_service->ExecutionDb(),
            entry_savestate_id,
            battle_run_spec_id,
            explorer_settings_id,
            unique_seed_id,
            &battle_set_id,
            &wave_id,
            &workflow_instance_id,
            &err)) {
        if (error_out) *error_out = "failed seeding battle analysis/workflow rows: " + err;
        return false;
    }

    const auto run_spec = db_service->AuthoringDb()->GetBattleRunSpec(battle_run_spec_id);
    const auto scenario_timeout_ms = run_spec.has_value()
        ? ComputeBattleScenarioTimeoutMs(*run_spec, options)
        : std::max<std::int64_t>(options.timeout_ms, 120000);

    simcore::db::execution::programdb::ProgramKindRegistry registry;
    simcore::db::execution::programdb::battlecontext::BattleContextProbePhaseRegistrationConfig context_config{};
    context_config.authoring_db = db_service->AuthoringDb();
    context_config.working_dir_root = options.workspace_root.value_or(
        std::filesystem::temp_directory_path() / "simcoredbe2e-default") / "battle-context";
    simcore::db::execution::programdb::battlecontext::RegisterBattleContextProbePhaseDescriptor(
        &registry,
        db_service->ExecutionDb(),
        db_service->AnalysisDb(),
        std::move(context_config));

    simcore::db::execution::programdb::battle::BattleSingleTurnPhaseRegistrationConfig config{};
    config.authoring_db = db_service->AuthoringDb();
    config.working_dir_root = options.workspace_root.value_or(
        std::filesystem::temp_directory_path() / "simcoredbe2e-default") / "battle-single-turn";
    simcore::db::execution::programdb::battle::RegisterBattleSingleTurnPhaseDescriptor(
        &registry,
        db_service->ExecutionDb(),
        db_service->StateDb(),
        db_service->AnalysisDb(),
        std::move(config));

    if (!registry.HasRequiredAdapters(static_cast<std::int32_t>(simcore::PK_BattleContextProbe))
        || !registry.HasRequiredAdaptersForStepKind("battle.context_probe")
        || !registry.HasRequiredAdapters(static_cast<std::int32_t>(simcore::PK_BattleSingleTurnRunner))
        || !registry.HasRequiredAdaptersForStepKind("battle.single_turn")) {
        if (error_out) *error_out = "battle descriptor registration is incomplete";
        return false;
    }

    std::cout << "[battle-single-turn-setup] seeded entry_savestate_id=" << entry_savestate_id
              << " unique_seed_id=" << unique_seed_id
              << " battle_run_spec_id=" << battle_run_spec_id
              << " explorer_settings_id=" << explorer_settings_id
              << " battle_set_id=" << battle_set_id
              << " initial_wave_id=" << wave_id
              << " workflow_instance_id=" << workflow_instance_id << "\n";

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
                options.workspace_root.value_or(std::filesystem::temp_directory_path() / "simcoredbe2e-default") / ".workers").string(),
            .visual_workers = options.visual_worker,
            .auto_resume_visual_workers = false,
            .visual_screenshot_dir = options.visual_screenshot_dir.value_or(
                options.workspace_root.value_or(std::filesystem::temp_directory_path() / "simcoredbe2e-default")
                    / "visual-screenshots").string(),
        },
        simcore::runner::parallel::simcoredb::CoordinatorIntegrationConfig{},
        &registry);

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
    std::unordered_map<std::size_t, simcore::PRProgress> last_progress_by_worker;

    coordinator.SetResultCallback([&](const simcore::PRResult& result) {
        std::ostringstream line;
        line << "[battle-single-turn-worker-result] job=" << result.job_id
             << " worker=" << result.worker_id
             << " ok=" << (result.ps.ok ? "true" : "false")
             << " w_err=" << simcore::WErrToString(result.ps.w_err) << "(" << static_cast<int>(result.ps.w_err) << ")";
        push_line(line.str());
    });
    coordinator.SetResultMapEventCallback([&](const std::string& line) {
        push_line(line);
    });
    coordinator.SetProgressCallback([&](const simcore::PRProgress& progress) {
        std::lock_guard<std::mutex> lock(progress_mtx);
        last_progress_by_worker[progress.worker_id] = progress;
    });

    ScopedWorkflowCoordinatorService workflow_coordinator;
    if (!workflow_coordinator.Start(
            db_service->ExecutionDb(),
            &registry,
            options,
            &err,
            [&](const std::string& line) {
                push_line(line);
            })) {
        if (error_out) *error_out = err;
        return false;
    }

    coordinator.Start();
    const auto started = std::chrono::steady_clock::now();
    const bool interactive_stdout = IsInteractiveStdout();
    MultiLineProgressRenderer progress_renderer;
    const auto interactive_refresh_cadence = std::chrono::milliseconds(100);
    bool completed = false;
    bool failed = false;
    std::string latest_state = "workflow=unavailable";
    std::size_t poll_count = 0;
    std::size_t ticks_since_snapshot = 0;
    std::size_t terminal_steps_seen_count = 0;
    std::vector<std::string> latest_lines;
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
            progress_renderer.WriteEventLines(std::cout, event_lines);
            progress_renderer.RenderIfDue(std::cout, std::chrono::steady_clock::now(), interactive_refresh_cadence);
        } else {
            for (const auto& line : event_lines) {
                std::cout << line << '\n';
            }
            if (ticks_since_snapshot >= 10 || poll_count == 1) {
                ticks_since_snapshot = 0;
                std::cout << "[battle-single-turn] ";
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
            latest_state = FormatWorkflowStateLine(*graph);
            using simcore::db::execution::workflow::WorkflowInstanceState;
            if (graph->instance.state == WorkflowInstanceState::Completed) {
                completed = true;
                break;
            }
            if (graph->instance.state == WorkflowInstanceState::Failed
                || graph->instance.state == WorkflowInstanceState::Canceled) {
                failed = true;
                break;
            }
            if (AreWorkflowStepsTerminal(*graph)) {
                ++terminal_steps_seen_count;
                if (terminal_steps_seen_count >= 2) {
                    if (HasFailedWorkflowStep(*graph)) {
                        failed = true;
                    } else {
                        completed = true;
                    }
                    break;
                }
            } else {
                terminal_steps_seen_count = 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
    }
    coordinator.Stop();
    workflow_coordinator.Stop();
    const auto final_event_lines = drain_lines();
    if (interactive_stdout) {
        progress_renderer.WriteEventLines(std::cout, final_event_lines);
    } else {
        for (const auto& line : final_event_lines) {
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
        latest_state = FormatWorkflowStateLine(*final_graph);
    }
    if (interactive_stdout) {
        progress_renderer.SetLines(latest_lines);
        progress_renderer.Render(std::cout);
    }

    const auto waves = db_service->AnalysisDb()->ListBattleTurnWaves(battle_set_id);
    std::size_t job_count = 0;
    std::size_t succeeded_count = 0;
    for (const auto& wave : waves) {
        const auto jobs = db_service->AnalysisDb()->ListBattleTurnJobsForWave(wave.wave_id);
        job_count += jobs.size();
        succeeded_count += static_cast<std::size_t>(std::count_if(jobs.begin(), jobs.end(), [](const auto& job) {
            return job.job_state == simcore::db::BattleTurnJobState::Succeeded;
        }));
    }

    std::cout << "[battle-single-turn-final] status=" << (completed ? "success" : failed ? "failure" : "timeout") << '\n';
    std::cout << "  timeout_ms=" << scenario_timeout_ms << '\n';
    std::cout << "  " << latest_state << '\n';
    std::cout << "  waves=" << waves.size()
              << " turn_jobs=" << job_count
              << " succeeded_turn_jobs=" << succeeded_count << '\n';

    if (!completed) {
        if (error_out) *error_out = failed
            ? "workflow did not complete successfully"
            : "workflow did not reach COMPLETED state before timeout";
        return false;
    }
    return true;
}

bool RunTasMovieSeedProbeBattleWorkflowGraphRealWorkerScenario(
    const CliOptions& options,
    const char* argv0,
    simcore::db::core::DBService* db_service,
    std::string* error_out) {
    if (db_service == nullptr || !db_service->IsRunning()) {
        if (error_out) *error_out = "DBService must be running";
        return false;
    }
    if (db_service->ExecutionDb() == nullptr
        || db_service->StateDb() == nullptr
        || db_service->AnalysisDb() == nullptr
        || db_service->AuthoringDb() == nullptr) {
        if (error_out) *error_out = "one or more SimCoreDB contexts are unavailable";
        return false;
    }

    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::exists(worker_exe)) {
        if (error_out) *error_out = "SimCoreWorker.exe was not found next to SimCoreDBe2e: " + worker_exe.string();
        return false;
    }

    DurableLogFile durable_log;
    if (!durable_log.Open(options, options.scenario, error_out)) {
        return false;
    }
    std::cout << "[durable-log] path=" << durable_log.path().string() << '\n';

    std::string err;
    std::int64_t dtm_artifact_id = 0;
    if (!SeedStateDtmArtifact(db_service->StateDb(), options.dtm_file, &dtm_artifact_id, &err)) {
        if (error_out) *error_out = "failed seeding StateDB DTM artifact: " + err;
        return false;
    }

    std::int64_t seed_probe_spec_id = 0;
    if (!SeedAuthoringSpec(db_service->AuthoringDb(), &seed_probe_spec_id, &err)) {
        if (error_out) *error_out = "failed seeding AuthoringDB seedprobe spec: " + err;
        return false;
    }

    std::int64_t battle_run_spec_id = 0;
    std::int64_t explorer_settings_id = 0;
    if (!SeedBattleAuthoringRows(
            db_service->AuthoringDb(),
            0,
            0,
            false,
            &battle_run_spec_id,
            &explorer_settings_id,
            &err)) {
        if (error_out) *error_out = "failed seeding battle authoring rows: " + err;
        return false;
    }

    std::int64_t battle_chain_spec_id = 0;
    if (!db_service->AuthoringDb()->SaveBattleChainSpec(
            {
                .name = "SimCoreDBe2e TAS SeedProbe Battle chain spec",
                .description = "First battle graph-style chain spec",
                .battle_run_spec_id = battle_run_spec_id,
                .explorer_settings_id = explorer_settings_id,
                .created_at_utc = simcore::db::types::UtcNow(),
                .event_id = "simcoredbe2e.authoring.battle_chain_spec.tasmovie_seedprobe_battle",
                .correlation_id = "simcoredbe2e.workflow_graph.tasmovie_seedprobe_battle",
                .causation_id = "simcoredbe2e.seed",
            },
            &battle_chain_spec_id,
            &err)) {
        if (error_out) *error_out = "failed seeding battle chain spec: " + err;
        return false;
    }

    std::int64_t workflow_instance_id = 0;
    if (!SeedTasMovieSeedProbeBattleGraphExecution(
            db_service->AuthoringDb(),
            db_service->ExecutionDb(),
            dtm_artifact_id,
            seed_probe_spec_id,
            battle_chain_spec_id,
            &workflow_instance_id,
            &err)) {
        if (error_out) *error_out = "failed seeding graph workflow execution rows: " + err;
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
    tas_config.blueprint.headroom_x10 = 50;
    tas_config.working_dir_root = options.workspace_root.value_or(
        std::filesystem::temp_directory_path() / "simcoredbe2e-default") / "tasmovie";
    simcore::db::execution::programdb::tasmovie::RegisterTasMoviePhaseDescriptor(
        &registry,
        db_service->ExecutionDb(),
        db_service->StateDb(),
        db_service->AnalysisDb(),
        std::move(tas_config));

    simcore::db::execution::programdb::seedprobe::SeedProbePhaseRegistrationConfig seed_config{};
    seed_config.authoring_db = db_service->AuthoringDb();
    simcore::db::execution::programdb::seedprobe::RegisterSeedProbePhaseDescriptors(
        &registry,
        db_service->ExecutionDb(),
        db_service->AnalysisDb(),
        std::move(seed_config));

    simcore::db::execution::programdb::battlecontext::BattleContextProbePhaseRegistrationConfig context_config{};
    context_config.authoring_db = db_service->AuthoringDb();
    context_config.working_dir_root = options.workspace_root.value_or(
        std::filesystem::temp_directory_path() / "simcoredbe2e-default") / "battle-context";
    simcore::db::execution::programdb::battlecontext::RegisterBattleContextProbePhaseDescriptor(
        &registry,
        db_service->ExecutionDb(),
        db_service->AnalysisDb(),
        std::move(context_config));

    simcore::db::execution::programdb::battle::BattleSingleTurnPhaseRegistrationConfig battle_config{};
    battle_config.authoring_db = db_service->AuthoringDb();
    battle_config.working_dir_root = options.workspace_root.value_or(
        std::filesystem::temp_directory_path() / "simcoredbe2e-default") / "battle-single-turn";
    simcore::db::execution::programdb::battle::RegisterBattleSingleTurnPhaseDescriptor(
        &registry,
        db_service->ExecutionDb(),
        db_service->StateDb(),
        db_service->AnalysisDb(),
        std::move(battle_config));

    if (!registry.HasRequiredAdapters(static_cast<std::int32_t>(simcore::PK_TasMovie))
        || !registry.HasRequiredAdaptersForStepKind("tas_movie")
        || !registry.HasRequiredAdaptersForStepKind("seed_probe_chain")
        || !registry.HasRequiredAdaptersForStepKind("battle_chain")
        || !registry.HasRequiredAdaptersForStepKind("battle.context_probe")
        || !registry.HasRequiredAdaptersForStepKind("battle.single_turn")) {
        if (error_out) *error_out = "graph workflow descriptor registration is incomplete";
        return false;
    }

    const auto run_spec = db_service->AuthoringDb()->GetBattleRunSpec(battle_run_spec_id);
    const auto tas_budget_ms = ComputeTasMovieRunMs(options.dtm_file, 35, options.timeout_ms * 2) + options.timeout_ms;
    const auto seedprobe_budget_ms = options.timeout_ms
        * (1 + (static_cast<std::int64_t>(kSeedProbeSamplesPerAxis) * kSeedProbeSamplesPerAxis * 3) + 25);
    const auto battle_budget_ms = run_spec.has_value()
        ? ComputeBattleScenarioTimeoutMs(*run_spec, options)
        : std::max<std::int64_t>(options.timeout_ms, 300000);
    const auto scenario_timeout_ms = tas_budget_ms + seedprobe_budget_ms + battle_budget_ms;

    std::cout << "[tasmovie-seedprobe-battle-graph-setup] dtm_artifact_id=" << dtm_artifact_id
              << " seed_probe_spec_id=" << seed_probe_spec_id
              << " battle_run_spec_id=" << battle_run_spec_id
              << " explorer_settings_id=" << explorer_settings_id
              << " battle_chain_spec_id=" << battle_chain_spec_id
              << " workflow_instance_id=" << workflow_instance_id
              << " rtc=4"
              << " fake_attacks=22..25\n";

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
                options.workspace_root.value_or(std::filesystem::temp_directory_path() / "simcoredbe2e-default") / ".workers").string(),
            .visual_workers = options.visual_worker,
            .auto_resume_visual_workers = false,
            .visual_screenshot_dir = options.visual_screenshot_dir.value_or(
                options.workspace_root.value_or(std::filesystem::temp_directory_path() / "simcoredbe2e-default")
                    / "visual-screenshots").string(),
        },
        simcore::runner::parallel::simcoredb::CoordinatorIntegrationConfig{},
        &registry);

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
    std::unordered_map<std::size_t, simcore::PRProgress> last_progress_by_worker;

    coordinator.SetResultCallback([&](const simcore::PRResult& result) {
        std::ostringstream line;
        line << "[tasmovie-seedprobe-battle-worker-result] job=" << result.job_id
             << " worker=" << result.worker_id
             << " ok=" << (result.ps.ok ? "true" : "false")
             << " w_err=" << simcore::WErrToString(result.ps.w_err) << "(" << static_cast<int>(result.ps.w_err) << ")";
        push_line(line.str());
    });
    coordinator.SetResultMapEventCallback([&](const std::string& line) {
        push_line(line);
    });
    coordinator.SetProgressCallback([&](const simcore::PRProgress& progress) {
        std::lock_guard<std::mutex> lock(progress_mtx);
        last_progress_by_worker[progress.worker_id] = progress;
    });

    ScopedWorkflowCoordinatorService workflow_coordinator;
    if (!workflow_coordinator.Start(
            db_service->ExecutionDb(),
            &registry,
            options,
            &err,
            [&](const std::string& line) {
                push_line(line);
            })) {
        if (error_out) *error_out = err;
        return false;
    }

    coordinator.Start();
    const auto started = std::chrono::steady_clock::now();
    const bool interactive_stdout = IsInteractiveStdout();
    MultiLineProgressRenderer progress_renderer;
    const auto interactive_refresh_cadence = std::chrono::milliseconds(100);
    bool completed = false;
    bool failed = false;
    std::string latest_state = "workflow=unavailable";
    std::size_t poll_count = 0;
    std::size_t ticks_since_snapshot = 0;
    std::size_t terminal_steps_seen_count = 0;
    std::vector<std::string> latest_lines;
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
            progress_renderer.WriteEventLines(std::cout, event_lines);
            progress_renderer.RenderIfDue(std::cout, std::chrono::steady_clock::now(), interactive_refresh_cadence);
        } else {
            for (const auto& line : event_lines) {
                std::cout << line << '\n';
            }
            if (ticks_since_snapshot >= 10 || poll_count == 1) {
                ticks_since_snapshot = 0;
                std::cout << "[tasmovie-seedprobe-battle-graph] ";
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
            latest_state = FormatWorkflowStateLine(*graph);
            using simcore::db::execution::workflow::WorkflowInstanceState;
            if (graph->instance.state == WorkflowInstanceState::Completed) {
                completed = true;
                break;
            }
            if (graph->instance.state == WorkflowInstanceState::Failed
                || graph->instance.state == WorkflowInstanceState::Canceled) {
                failed = true;
                break;
            }
            if (AreWorkflowStepsTerminal(*graph)) {
                ++terminal_steps_seen_count;
                if (terminal_steps_seen_count >= 2) {
                    if (HasFailedWorkflowStep(*graph)) {
                        failed = true;
                    } else {
                        completed = true;
                    }
                    break;
                }
            } else {
                terminal_steps_seen_count = 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
    }

    coordinator.Stop();
    workflow_coordinator.Stop();
    const auto final_event_lines = drain_lines();
    if (interactive_stdout) {
        progress_renderer.WriteEventLines(std::cout, final_event_lines);
    } else {
        for (const auto& line : final_event_lines) {
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
        latest_state = FormatWorkflowStateLine(*final_graph);
    }
    if (interactive_stdout) {
        progress_renderer.SetLines(latest_lines);
        progress_renderer.Render(std::cout);
    }

    const auto probe_run_id = ResolveProbeRunIdFromGraph(final_graph);
    const auto unique_rows = db_service->AnalysisDb()->ListSeedProbeUniqueSeeds(probe_run_id);
    const auto context_probe_id = ResolveBattleContextProbeIdFromGraph(final_graph);
    const auto context_probe = db_service->AnalysisDb()->GetBattleContextProbe(context_probe_id);
    const auto waves = db_service->AnalysisDb()->ListBattleTurnWavesForContextProbe(context_probe_id);
    const auto all_waves = !waves.empty()
        ? db_service->AnalysisDb()->ListBattleTurnWaves(waves.front().battle_set_id)
        : std::vector<simcore::db::BattleTurnWaveSnapshot>{};
    std::size_t job_count = 0;
    std::size_t succeeded_count = 0;
    std::size_t victory_count = 0;
    for (const auto& wave : all_waves) {
        const auto jobs = db_service->AnalysisDb()->ListBattleTurnJobsForWave(wave.wave_id);
        job_count += jobs.size();
        for (const auto& job : jobs) {
            if (job.job_state == simcore::db::BattleTurnJobState::Succeeded) {
                ++succeeded_count;
            }
            if (job.battle_outcome.has_value()
                && *job.battle_outcome == simcore::battle::Outcome::Victory) {
                ++victory_count;
            }
        }
    }

    std::cout << "[tasmovie-seedprobe-battle-graph-final] status=" << (completed ? "success" : failed ? "failure" : "timeout") << '\n';
    std::cout << "  timeout_ms=" << scenario_timeout_ms << '\n';
    std::cout << "  " << latest_state << '\n';
    std::cout << "  probe_run_id=" << probe_run_id
              << " unique_count=" << unique_rows.size()
              << " context_probe_id=" << context_probe_id
              << " context_status=" << (context_probe.has_value() ? static_cast<int>(context_probe->probe_status) : 0)
              << " first_turn_waves=" << waves.size()
              << " all_waves=" << all_waves.size()
              << " turn_jobs=" << job_count
              << " succeeded_turn_jobs=" << succeeded_count
              << " victory_jobs=" << victory_count << '\n';

    if (!completed) {
        if (error_out) *error_out = failed
            ? "workflow did not complete successfully"
            : "workflow did not reach COMPLETED state before timeout";
        return false;
    }
    if (probe_run_id <= 0 || unique_rows.empty()) {
        if (error_out) *error_out = "graph workflow completed without a SeedProbe run with uniques";
        return false;
    }
    if (context_probe_id <= 0 || !context_probe.has_value()) {
        if (error_out) *error_out = "graph workflow completed without a battle context probe output";
        return false;
    }
    if (waves.size() != unique_rows.size()) {
        if (error_out) *error_out = "battle context wave fanout mismatch: waves="
            + std::to_string(waves.size()) + " uniques=" + std::to_string(unique_rows.size());
        return false;
    }
    if (victory_count == 0) {
        if (error_out) *error_out = "battle graph workflow completed without a victory job";
        return false;
    }
    return true;
}

} // namespace simcore::e2e
