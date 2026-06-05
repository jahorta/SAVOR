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
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Runner/Parallel/SimCoreDB/DBWorkflowCoordinatorFactory.h"
#include "Runner/Parallel/SimCoreDB/DBWorkflowWorkerCoordinator.h"
#include "Runner/Breakpoints/BPRegistry.h"
#include "Runner/Breakpoints/Predicate.h"
#include "Runner/IPC/Wire.h"
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
    if (!SeedExecutionWorkflow(
            db_service->AnalysisDb(),
            db_service->ExecutionDb(),
            entry_savestate_id,
            seed_probe_spec_id,
            &seedprobe_workflow_instance_id,
            &probe_run_id,
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

    coordinator.Start();
    const auto started = std::chrono::steady_clock::now();
    const auto timeout_ms = ComputeSeedProbePreludeTimeoutMs(options);
    const bool interactive_stdout = IsInteractiveStdout();
    MultiLineProgressRenderer progress_renderer;
    bool completed = false;
    bool failed = false;
    std::string latest_state = "workflow=unavailable";
    std::vector<std::string> latest_lines;
    std::size_t poll_count = 0;
    std::size_t ticks_since_snapshot = 0;
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
                .min_fake_attacks = 0,
                .max_fake_attacks = 2,
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

    std::int64_t turn_id = 0;
    if (!authoring_db->SaveBattlePlanTurn(
            {
                .plan_id = plan_id,
                .turn_index = 1,
                .actions = {
                    {
                        .actor_slot = 0,
                        .macro = soa::battle::actions::BattleAction::Attack,
                        .target_kind = simcore::db::BattlePlanTargetKind::AnyEnemy,
                        .ordinal = 0,
                    },
                    {
                        .actor_slot = 1,
                        .macro = soa::battle::actions::BattleAction::Attack,
                        .target_kind = simcore::db::BattlePlanTargetKind::SameAsOtherPC,
                        .target_same_as_actor_slot = 0,
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
                        .macro = soa::battle::actions::BattleAction::Attack,
                        .target_kind = simcore::db::BattlePlanTargetKind::AnyEnemy,
                        .ordinal = 0,
                    },
                    {
                        .actor_slot = 1,
                        .macro = soa::battle::actions::BattleAction::Attack,
                        .target_kind = simcore::db::BattlePlanTargetKind::SameAsOtherPC,
                        .target_same_as_actor_slot = 0,
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
                .lhs_kind = simcore::db::PredicateOperandKind::Absolute,
                .lhs_value = 0,
                .rhs_kind = simcore::db::PredicateOperandKind::Memory,
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

    std::int64_t turn_order_predicate_spec_id = 0;
    if (!authoring_db->SavePredicateSpec(
        {
            .name = "SimCoreDBe2e players before enemies",
            .breakpoint_id = bp::battle::TurnIsReady,
            .lhs_kind = simcore::db::PredicateOperandKind::Memory,
            .lhs_value = static_cast<std::int64_t>(addr::derived::battle::TurnOrderPcMax),
            .rhs_kind = simcore::db::PredicateOperandKind::Memory,
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

    std::int64_t predicate_set_id = 0;
    if (!authoring_db->SavePredicateSet(
            {
                .predicate_spec_ids = { electribox_predicate_spec_id, turn_order_predicate_spec_id },
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
    workflow.input_ref_kind = std::string(kWaveRefKind);
    workflow.input_ref_id = wave_id;
    workflow.created_by = "simcoredbe2e";
    workflow.created_at_utc = now.time_since_epoch().count();
    workflow.available_inputs = { "analysis_battle.turn_wave.wave_id" };
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
            .desired_workers = 4u,
            .controller_sleep_ms = static_cast<std::uint32_t>(options.poll_ms),
            .worker_exe_path = worker_exe.string(),
            .iso_path = options.iso_path.string(),
            .dolphin_base_dir = options.dolphin_base_dir.string(),
            .worker_dir_root = options.worker_dir_root.value_or(
                options.workspace_root.value_or(std::filesystem::temp_directory_path() / "simcoredbe2e-default") / ".workers").string(),
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

    coordinator.Start();
    const auto started = std::chrono::steady_clock::now();
    const bool interactive_stdout = IsInteractiveStdout();
    MultiLineProgressRenderer progress_renderer;
    bool completed = false;
    bool failed = false;
    std::string latest_state = "workflow=unavailable";
    std::size_t poll_count = 0;
    std::size_t ticks_since_snapshot = 0;
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

} // namespace simcore::e2e
