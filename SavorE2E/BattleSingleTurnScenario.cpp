#include "BattleSingleTurnScenario.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
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
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/ProgramDB/ProductionProgramKindRegistry.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Execution/Workflow/WorkflowUnitActivationFactory.h"
#include "Phases/Programs/PlayTasMovie/TasMoviePayload.h"
#include "Execution/DBWorkflowCoordinatorFactory.h"
#include "Execution/DBWorkflowWorkerCoordinator.h"
#include "Runner/Breakpoints/BpRegistry.h"
#include "Runner/Breakpoints/Predicate.h"
#include "Runner/IPC/Wire.h"
#include "Tas/DtmFile.h"
#include "MultiLineProgressRenderer.h"
#include "WorkerCoordinatorPerf.h"

namespace savor::e2e {
namespace {

constexpr const char* kWaveRefKind = "analysis_battle.turn_wave";

void AppendTasMovieRtcArgument(
    savor::db::execution::workflow::WorkflowCreateInstanceCommand* command,
    std::int64_t rtc_value) {
    if (command == nullptr) {
        return;
    }
    command->arguments.push_back({
        .node_key = "tas_1",
        .argument_key = "rtc",
        .value_type = "integer",
        .integer_value = rtc_value,
        .source_kind = "scenario",
    });
}

void AppendTasMovieHeadroomArgument(
    savor::db::execution::workflow::WorkflowCreateInstanceCommand* command,
    const CliOptions& options) {
    if (command == nullptr) {
        return;
    }
    command->arguments.push_back({
        .node_key = "tas_1",
        .argument_key = "headroom",
        .value_type = "integer",
        .integer_value = options.tasmovie_headroom_x10.value_or(50),
        .source_kind = "scenario",
    });
}

const char* ToString(savor::db::execution::workflow::WorkflowInstanceState state) {
    using savor::db::execution::workflow::WorkflowInstanceState;
    switch (state) {
    case WorkflowInstanceState::Pending: return "PENDING";
    case WorkflowInstanceState::Running: return "RUNNING";
    case WorkflowInstanceState::Completed: return "COMPLETED";
    case WorkflowInstanceState::Failed: return "FAILED";
    case WorkflowInstanceState::Canceled: return "CANCELED";
    }
    return "UNKNOWN";
}

std::string FormatWorkflowStateLine(const savor::db::execution::workflow::WorkflowGraphSnapshot& graph) {
    std::size_t completed = 0;
    for (const auto& step : graph.steps) {
        if (step.state == savor::db::execution::workflow::WorkflowStepState::Completed) {
            ++completed;
        }
    }
    std::ostringstream oss;
    oss << "workflow=" << ToString(graph.instance.state)
        << " steps=" << completed << "/" << graph.steps.size();
    return oss.str();
}

std::int64_t ComputeBattleScenarioTimeoutMs(const savor::db::BattleRunSpecSnapshot& run_spec, const CliOptions& options) {
    const int fake_low = options.battle_fake_attack_low.value_or(0);
    const int fake_high = options.battle_fake_attack_high.value_or(fake_low);
    const int fake_jobs_per_wave = std::max(1, std::abs(fake_high - fake_low) + 1);
    const std::int64_t per_wave_budget = static_cast<std::int64_t>(std::max(1, fake_jobs_per_wave))
        * std::max<std::int64_t>(1000, static_cast<std::int64_t>(run_spec.run_ms));
    return std::max<std::int64_t>(options.timeout_ms, (per_wave_budget * 4) + options.timeout_ms);
}

std::int64_t ComputeTasMovieRunMs(
    const std::filesystem::path& dtm_file,
    std::uint8_t headroom_x10,
    std::int64_t fallback_ms) {
    savor::tas::DtmFile dtm;
    if (!dtm.load(dtm_file.string())) {
        return fallback_ms;
    }
    const auto info = dtm.info();
    const auto run_ms = savor::tasmovie::compute_run_ms_from_counts(
        info.vi_count,
        info.input_count,
        static_cast<double>(headroom_x10) / 10.0);
    return run_ms > 0 ? static_cast<std::int64_t>(run_ms) : fallback_ms;
}

std::string BattleSeedSuffix(
    std::string scenario,
    int min_fake_attacks,
    int max_fake_attacks,
    bool require_electribox_drop) {
    if (scenario.empty()) {
        scenario = "scenario";
    }
    for (char& ch : scenario) {
        const bool is_digit = ch >= '0' && ch <= '9';
        const bool is_upper = ch >= 'A' && ch <= 'Z';
        const bool is_lower = ch >= 'a' && ch <= 'z';
        if (!is_digit && !is_upper && !is_lower) {
            ch = '-';
        }
    }
    return scenario
        + "-fake-" + std::to_string(min_fake_attacks)
        + "-" + std::to_string(max_fake_attacks)
        + (require_electribox_drop ? "-drop" : "-nodrop");
}

std::vector<std::uint8_t> BuildCurrentTurnAddressProgram() {
    addrprog::Builder builder;
    builder.op_base_key(addr::derived::battle::CurrentTurn);
    builder.op_end();
    return builder.blob();
}

std::int64_t ComputeSeedProbePreludeTimeoutMs(const CliOptions& options) {
    const auto samples_per_axis = options.seedprobe_samples_per_axis.value_or(kSeedProbeSamplesPerAxis);
    const std::int64_t grid_probe_count =
        static_cast<std::int64_t>(samples_per_axis)
        * static_cast<std::int64_t>(samples_per_axis)
        * 3;
    constexpr std::int64_t kAverageUniqueCountEstimate = 25;
    return std::max<std::int64_t>(
        options.timeout_ms,
        options.timeout_ms * (1 + grid_probe_count + kAverageUniqueCountEstimate));
}

bool RunSeedProbePrelude(
    const CliOptions& options,
    const std::filesystem::path& worker_exe,
    savor::db::core::DBService* db_service,
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
    if (!SeedAuthoringSpec(db_service->AuthoringDb(), options, &seed_probe_spec_id, &err)) {
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
            options,
            &seedprobe_workflow_instance_id,
            &err)) {
        if (error_out) *error_out = "failed seeding SeedProbe workflow: " + err;
        return false;
    }

    const auto scenario_workspace_root = options.workspace_root.value_or(
        std::filesystem::temp_directory_path() / "savor-e2e-default");
    auto registry_config =
        savor::db::execution::programdb::MakeProductionProgramKindRegistryConfig(
            scenario_workspace_root / "workflow-runtime");
    savor::db::execution::programdb::ProgramKindRegistry registry;
    if (!savor::db::execution::programdb::BuildProductionProgramKindRegistry(
            savor::db::execution::programdb::ProductionProgramKindRegistryDependencies{
                .execution_db = db_service->ExecutionDb(),
                .state_db = db_service->StateDb(),
                .analysis_db = db_service->AnalysisDb(),
                .authoring_db = db_service->AuthoringDb(),
            },
            std::move(registry_config),
            &registry,
            &err)) {
        if (error_out) *error_out = "failed building production program registry: " + err;
        return false;
    }

    const auto battle_worker_dir_root = options.worker_dir_root.value_or(
        options.workspace_root.value_or(
            std::filesystem::temp_directory_path() / "savor-e2e-default")
            / ".workers");

    auto coordinator = savor::runner::parallel::savordb::BuildDbBackedWorkflowCoordinator(
        db_service->ExecutionDb(),
        db_service->StateDb(),
        savor::runner::parallel::savordb::DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 5u,
            .controller_sleep_ms = static_cast<std::uint32_t>(options.poll_ms),
            .worker_exe_path = worker_exe.string(),
            .iso_path = options.iso_path.string(),
            .dolphin_base_dir = options.dolphin_base_dir.string(),
            .worker_dir_root = battle_worker_dir_root.string(),
            .visual_workers = options.visual_worker,
            .auto_resume_visual_workers = false,
            .visual_screenshot_dir = options.visual_screenshot_dir.value_or(
                options.workspace_root.value_or(std::filesystem::temp_directory_path() / "savor-e2e-default")
                    / "visual-screenshots").string(),
        },
        savor::runner::parallel::savordb::CoordinatorIntegrationConfig{},
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
    coordinator.SetResultCallback([&](const savor::PRResult& result) {
        std::ostringstream line;
        line << "[battle-seedprobe-worker-result] job=" << result.job_id
             << " worker=" << result.worker_id
             << " ok=" << (result.ps.ok ? "true" : "false")
             << " w_err=" << savor::WErrToString(result.ps.w_err) << "(" << static_cast<int>(result.ps.w_err) << ")";
        push_line(line.str());
    });
    coordinator.SetResultMapEventCallback([&](const std::string& line) {
        push_line(line);
    });

    ScopedWorkflowCoordinatorService workflow_coordinator;
    if (!workflow_coordinator.Start(
            db_service->ExecutionDb(),
            db_service->AuthoringDb(),
            &registry,
            options,
            &err,
            [&](const std::string& line) {
                push_line(line);
            },
            false,
            coordinator.ItemCreditSource())) {
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
        const auto telemetry = coordinator.SnapshotTelemetry();
        const auto worker_snapshot = coordinator.SnapshotWorkers();
        RecordWorkerCoordinatorPerfSample(options, telemetry, worker_snapshot);
        latest_lines = BuildCoordinatorProgressLines(
            db_service->ExecutionDb(),
            telemetry,
            worker_snapshot,
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
            using savor::db::execution::workflow::WorkflowInstanceState;
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
    const auto final_telemetry = coordinator.SnapshotTelemetry();
    const auto final_worker_snapshot = coordinator.SnapshotWorkers();
    RecordWorkerCoordinatorPerfSample(options, final_telemetry, final_worker_snapshot);
    latest_lines = BuildCoordinatorProgressLines(
        db_service->ExecutionDb(),
        final_telemetry,
        final_worker_snapshot,
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
            if ((step.step_key == "probe_1"
                    || step.step_key == "probe_1/Grid"
                    || step.step_key == "probe_1/Unique"
                    || step.step_kind == "seed_probe_chain")
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
    savor::db::IAuthoringDb* authoring_db,
    const std::string& suffix,
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
    const auto now = savor::db::types::UtcNow();

    if (!authoring_db->SaveBattleRunSpec(
            {
                .name = "SavorE2E first battle single-turn " + suffix,
                .priority = 1,
                .run_ms = 10000,
                .vi_stall_ms = 2000,
                .progress_enable = true,
                .use_single_turn_runner = true,
                .auto_wave_trigger_enable = true,
                .created_at_utc = now,
                .correlation_id = "savor-e2e.battle",
                .causation_id = "savor-e2e.seed",
            },
            battle_run_spec_id_out,
            error_out)) {
        return false;
    }

    std::int64_t plan_id = 0;
    if (!authoring_db->SavePlan(
            {
                .name = "SavorE2E battle plan " + suffix,
                .fingerprint = "savor-e2e-battle-plan-v1-" + suffix,
                .num_turns = 3,
                .created_at_utc = now,
                .correlation_id = "savor-e2e.battle",
                .causation_id = "savor-e2e.seed",
            },
            &plan_id,
            error_out)) {
        return false;
    }

    std::int64_t attack_any_enemy_preset_id = 0;
    if (!authoring_db->SaveBattlePlanActionPreset(
            {
                .name = "SavorE2E attack any enemy " + suffix,
                .macro = soa::battle::actions::BattleAction::Attack,
                .target_kind = savor::db::BattlePlanTargetKind::AnyEnemy,
                .created_at_utc = now,
                .correlation_id = "savor-e2e.battle",
                .causation_id = "plan-" + std::to_string(plan_id),
            },
            &attack_any_enemy_preset_id,
            error_out)) {
        return false;
    }

    std::int64_t attack_same_target_preset_id = 0;
    if (!authoring_db->SaveBattlePlanActionPreset(
            {
                .name = "SavorE2E attack same target as PC0 " + suffix,
                .macro = soa::battle::actions::BattleAction::Attack,
                .target_kind = savor::db::BattlePlanTargetKind::SameAsOtherPC,
                .target_same_as_actor_slot = 0,
                .created_at_utc = now,
                .correlation_id = "savor-e2e.battle",
                .causation_id = "plan-" + std::to_string(plan_id),
            },
            &attack_same_target_preset_id,
            error_out)) {
        return false;
    }

    for (int turn_index = 1; turn_index <= 3; ++turn_index) {
        std::int64_t turn_id = 0;
        if (!authoring_db->SaveBattlePlanTurn(
                {
                    .plan_id = plan_id,
                    .turn_index = turn_index,
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
                    .correlation_id = "savor-e2e.battle",
                    .causation_id = "plan-" + std::to_string(plan_id),
                },
                &turn_id,
                error_out)) {
            return false;
        }
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
                    .name = "SavorE2E one electribox per turn " + suffix,
                    .breakpoint_id = bp::battle::EndTurn,
                    .lhs_value = 0,
                    .rhs_value = static_cast<std::int64_t>(addr::battle::CurrentTurn),
                    .cmp_op = savor::db::PredicateComparisonOp::EQ,
                    .width = 1,
                    .flag_mask = static_cast<std::int64_t>(
                        static_cast<std::uint32_t>(savor::pred::PredFlag::Active)
                        | static_cast<std::uint32_t>(savor::pred::PredFlag::LhsIsProg)
                        | static_cast<std::uint32_t>(savor::pred::PredFlag::RhsIsKey)),
                    .lhs_address_program_id = address_program_id,
                    .abort_on_fail = true,
                    .created_at_utc = now,
                    .correlation_id = "savor-e2e.battle",
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
            .name = "SavorE2E players before enemies " + suffix,
            .breakpoint_id = bp::battle::TurnIsReady,
            .lhs_value = static_cast<std::int64_t>(addr::derived::battle::TurnOrderPcMax),
            .rhs_value = static_cast<std::int64_t>(addr::derived::battle::TurnOrderEcMin),
            .cmp_op = savor::db::PredicateComparisonOp::LT,
            .width = 1,
            .flag_mask = static_cast<std::int64_t>(
                static_cast<std::uint32_t>(savor::pred::PredFlag::Active)
                | static_cast<std::uint32_t>(savor::pred::PredFlag::LhsIsKey)
                | static_cast<std::uint32_t>(savor::pred::PredFlag::RhsIsKey)),
            .abort_on_fail = false,
            .created_at_utc = now,
            .correlation_id = "savor-e2e.battle",
            .causation_id = "savor-e2e.pred2",
        },
        &turn_order_predicate_spec_id,
        error_out)) {
        return false;
    }
    predicate_spec_ids.push_back(turn_order_predicate_spec_id);

    std::int64_t predicate_set_id = 0;
    if (!authoring_db->SavePredicateSet(
            {
                .name = "SavorE2E battle predicates " + suffix,
                .predicate_spec_ids = predicate_spec_ids,
                .created_at_utc = now,
            },
            &predicate_set_id,
            error_out)) {
        return false;
    }

    return authoring_db->SaveExplorerSettings(
        {
            .name = "SavorE2E first battle settings " + suffix,
            .description = "First battle single-turn e2e setup",
            .default_plan_id = plan_id,
            .default_predicate_set_id = predicate_set_id,
            .created_at_utc = now,
            .correlation_id = "savor-e2e.battle",
            .causation_id = "plan-" + std::to_string(plan_id),
        },
        explorer_settings_id_out,
        error_out);
}

bool SeedBattleAnalysisAndWorkflowRows(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IAnalysisDb* analysis_db,
    savor::db::IExecutionDb* execution_db,
    const std::string& suffix,
    std::int64_t entry_savestate_id,
    std::int64_t battle_run_spec_id,
    std::int64_t explorer_settings_id,
    int min_fake_attacks,
    int max_fake_attacks,
    std::int64_t source_unique_seed_id,
    std::int64_t* battle_set_id_out,
    std::int64_t* wave_id_out,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || analysis_db == nullptr || execution_db == nullptr) {
        if (error_out) *error_out = "authoring/analysis/execution db unavailable";
        return false;
    }
    const auto now = savor::db::types::UtcNow();

    std::int64_t battle_set_id = 0;
    if (!analysis_db->CreateBattleSet(
            {
                .name = "SavorE2E first battle set " + suffix,
                .entry_savestate_id = entry_savestate_id,
                .battle_run_spec_id = battle_run_spec_id,
                .explorer_settings_id = explorer_settings_id,
                .launch_fake_attack_min = min_fake_attacks,
                .launch_fake_attack_max = max_fake_attacks,
                .status = savor::db::BattleSetStatus::Active,
                .created_at_utc = now,
                .correlation_id = "savor-e2e.battle",
                .causation_id = "savor-e2e.seed",
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
                .source_kind = savor::db::BattleSeedCandidateSourceKind::SeedProbeUnique,
                .candidate_status = savor::db::BattleSeedCandidateStatus::Ready,
                .created_at_utc = now,
                .correlation_id = "savor-e2e.battle",
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
                .status = savor::db::BattleTurnWaveStatus::Ready,
                .created_at_utc = now,
                .correlation_id = "savor-e2e.battle",
                .causation_id = "seed-candidate-" + std::to_string(seed_candidate_id),
            },
            &wave_id,
            error_out)) {
        return false;
    }

    savor::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = "SavorE2E battle single-turn graph " + suffix,
                .description = "Graph-wrapped battle context probe scenario",
                .graph_version = 1,
                .graph_hash = "savor-e2e.workflow_graph.battle_single_turn.v1." + suffix,
                .nodes = {
                    {
                        .node_key = "battle_context_1",
                        .unit_kind = "battle.context_probe",
                        .display_name = "Battle Context Probe",
                        .inputs = {
                            { .input_key = "turn_wave", .data_kind = "analysis_battle.turn_wave_id", .display_name = "Turn wave" },
                        },
                        .possible_outputs = {
                            { .output_key = "battle_context", .data_kind = "analysisbattle.context_probe", .display_name = "Battle context" },
                        },
                    },
                },
                .created_at_utc = now,
                .correlation_id = "savor-e2e.workflow_graph.battle_single_turn",
                .causation_id = "savor-e2e.seed",
            },
            &saved,
            error_out)) {
        return false;
    }

    savor::db::execution::workflow::WorkflowCreateInstanceCommand workflow{};
    workflow.workflow_kind = "workflow_graph";
    workflow.root_scope_kind = "run";
    workflow.root_scope_id = battle_set_id;
    workflow.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    workflow.created_by = "savor-e2e";
    workflow.created_at_utc = now.time_since_epoch().count();
    const auto registry = savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto battle_context_activation = savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
        registry,
        "battle_context_1",
        "battle_context_1",
        "battle.context_probe",
        "Battle Context Probe",
        std::nullopt,
        std::nullopt,
        {},
        &activation_error);
    if (!battle_context_activation.has_value()) {
        if (error_out) *error_out = activation_error;
        return false;
    }
    battle_context_activation->steps.front().input_ref_kind = std::string(kWaveRefKind);
    battle_context_activation->steps.front().input_ref_id = wave_id;
    workflow.unit_activations.push_back(std::move(*battle_context_activation));
    workflow.input_bindings.push_back({
        .node_key = "battle_context_1",
        .input_key = "turn_wave",
        .data_kind = "analysis_battle.turn_wave_id",
        .ref_kind = kWaveRefKind,
        .ref_id = wave_id,
        .source_kind = "external",
    });
    if (!execution_db->CreateWorkflowInstance(workflow, workflow_instance_id_out, error_out)) {
        return false;
    }

    if (battle_set_id_out) *battle_set_id_out = battle_set_id;
    if (wave_id_out) *wave_id_out = wave_id;
    return true;
}

bool EnsureNeutralAuthoringInputSet(
    savor::db::IAuthoringDb* authoring_db,
    std::int64_t* input_set_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || input_set_id_out == nullptr) {
        if (error_out) *error_out = "authoring db/input set output unavailable";
        return false;
    }

    return authoring_db->EnsureAuthoringInputSet(
        {
            .name = "SavorE2E neutral input",
            .frames = {
                {
                    .main_x = 128,
                    .main_y = 128,
                    .cstick_x = 128,
                    .cstick_y = 128,
                    .trigger_x = 0,
                    .trigger_y = 0,
                },
            },
            .created_at_utc = savor::db::types::UtcNow(),
        },
        input_set_id_out,
        error_out);
}

bool EnsureBattleOnlyAuthoringInputSet(
    savor::db::IAuthoringDb* authoring_db,
    std::int64_t* input_set_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || input_set_id_out == nullptr) {
        if (error_out) *error_out = "authoring db/input set output unavailable";
        return false;
    }

    return authoring_db->EnsureAuthoringInputSet(
        {
            .name = "SavorE2E battle-only joystick input",
            .frames = {
                {
                    .main_x = 128,
                    .main_y = 128,
                    .cstick_x = 128,
                    .cstick_y = 128,
                    .trigger_x = 0,
                    .trigger_y = 0,
                },
                {
                    .main_x = 160,
                    .main_y = 128,
                    .cstick_x = 128,
                    .cstick_y = 128,
                    .trigger_x = 0,
                    .trigger_y = 0,
                },
                {
                    .main_x = 128,
                    .main_y = 160,
                    .cstick_x = 128,
                    .cstick_y = 128,
                    .trigger_x = 0,
                    .trigger_y = 0,
                },
            },
            .created_at_utc = savor::db::types::UtcNow(),
        },
        input_set_id_out,
        error_out);
}

bool SeedTasMovieSeedProbeBattleGraphExecution(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    std::int64_t dtm_artifact_id,
    std::int64_t seed_probe_spec_id,
    std::int64_t battle_chain_spec_id,
    const CliOptions& options,
    std::optional<std::int64_t> override_input_set_id,
    std::int64_t rtc_value,
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

    const auto graph_suffix = BattleSeedSuffix(
        options.scenario,
        options.battle_fake_attack_low.value_or(0),
        options.battle_fake_attack_high.value_or(0),
        override_input_set_id.has_value());
    savor::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = "SavorE2E TAS SeedProbe Battle graph " + graph_suffix,
                .description = "TasMovie -> SeedProbe -> Battle graph-style e2e scenario",
                .graph_version = 1,
                .graph_hash = "savor-e2e.workflow_graph.tasmovie_seedprobe_battle.v1." + graph_suffix,
                .nodes = {
                    {
                        .node_key = "tas_1",
                        .unit_kind = "tas_movie",
                        .display_name = "TAS Movie",
                        .inputs = {
                            { .input_key = "dtm_artifact", .data_kind = "state_artifact.dtm_artifact_id", .display_name = "DTM artifact" },
                        },
                        .possible_outputs = {
                            { .output_key = "savestate", .data_kind = "state.savestate_id", .display_name = "Output savestate" },
                        },
                    },
                    {
                        .node_key = "probe_1",
                        .unit_kind = "battle_seed_probe",
                        .display_name = "Battle Seed Probe",
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
                    { .from_node_key = "tas_1", .output_key = "savestate", .to_node_key = "probe_1", .input_key = "entry_savestate" },
                    { .from_node_key = "tas_1", .output_key = "savestate", .to_node_key = "battle_1", .input_key = "entry_savestate" },
                    { .from_node_key = "probe_1", .output_key = "unique_input_frames", .to_node_key = "battle_1", .input_key = "initial_input_frames" },
                },
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "savor-e2e.workflow_graph.tasmovie_seedprobe_battle",
                .causation_id = "savor-e2e.seed",
            },
            &saved,
            error_out)) {
        return false;
    }

    savor::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.root_scope_id = dtm_artifact_id;
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "savor-e2e";
    command.created_at_utc = savor::db::types::UtcNow().time_since_epoch().count();
    const auto registry = savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto tas_activation = savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
        registry,
        "tas_1",
        "tas_1",
        "tas_movie",
        "TAS Movie",
        std::nullopt,
        std::nullopt,
        {},
        &activation_error);
    if (!tas_activation.has_value()) {
        if (error_out) *error_out = activation_error;
        return false;
    }
    command.unit_activations.push_back(std::move(*tas_activation));
    auto probe_activation = savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
        registry,
        "probe_1",
        "probe_1",
        "battle_seed_probe",
        "Battle Seed Probe",
        std::optional<std::string>("seed_probe_spec"),
        seed_probe_spec_id,
        { "tas_1" },
        &activation_error);
    if (!probe_activation.has_value()) {
        if (error_out) *error_out = activation_error;
        return false;
    }
    command.unit_activations.push_back(std::move(*probe_activation));
    auto battle_activation = savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
        registry,
        "battle_1",
        "battle_1",
        "battle_chain",
        "Battle Chain",
        std::optional<std::string>("authoring.battle_chain_spec"),
        battle_chain_spec_id,
        { "tas_1", "probe_1" },
        &activation_error);
    if (!battle_activation.has_value()) {
        if (error_out) *error_out = activation_error;
        return false;
    }
    command.unit_activations.push_back(std::move(*battle_activation));
    command.input_bindings.push_back({
        .node_key = "tas_1",
        .input_key = "dtm_artifact",
        .data_kind = "state_artifact.dtm_artifact_id",
        .ref_kind = "state_artifact",
        .ref_id = dtm_artifact_id,
        .source_kind = "external",
    });
    if (override_input_set_id.has_value()) {
        command.input_bindings.push_back({
            .node_key = "battle_1",
            .input_key = "initial_input_frames",
            .data_kind = "analysis.input_frame_set_id",
            .ref_kind = "au.input_set",
            .ref_id = *override_input_set_id,
            .source_kind = "external_override",
        });
    }
    AppendTasMovieHeadroomArgument(&command, options);
    AppendTasMovieRtcArgument(&command, rtc_value);
    command.arguments.push_back({
        .node_key = "probe_1",
        .argument_key = "samples_per_axis",
        .value_type = "integer",
        .integer_value = options.seedprobe_samples_per_axis.value_or(kSeedProbeSamplesPerAxis),
        .source_kind = "scenario",
    });
    command.arguments.push_back({ .node_key = "battle_1", .argument_key = "fake_attack_min", .value_type = "integer", .integer_value = options.battle_fake_attack_low.value_or(0), .source_kind = "scenario" });
    command.arguments.push_back({ .node_key = "battle_1", .argument_key = "fake_attack_max", .value_type = "integer", .integer_value = options.battle_fake_attack_high.value_or(0), .source_kind = "scenario" });
    return execution_db->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
}

bool SeedTasMovieBattleGraphExecution(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    std::int64_t dtm_artifact_id,
    std::int64_t battle_chain_spec_id,
    std::int64_t input_set_id,
    const CliOptions& options,
    std::int64_t rtc_value,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || execution_db == nullptr || workflow_instance_id_out == nullptr) {
        if (error_out) *error_out = "authoring/execution db unavailable";
        return false;
    }
    if (dtm_artifact_id <= 0 || battle_chain_spec_id <= 0 || input_set_id <= 0) {
        if (error_out) *error_out = "dtm artifact, battle chain spec, and input set ids must be > 0";
        return false;
    }

    const auto graph_suffix = BattleSeedSuffix(
        options.scenario,
        options.battle_fake_attack_low.value_or(0),
        options.battle_fake_attack_high.value_or(0),
        false);
    savor::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = "SavorE2E TAS Battle graph " + graph_suffix,
                .description = "TasMovie -> Battle graph-style e2e scenario with external input frames",
                .graph_version = 1,
                .graph_hash = "savor-e2e.workflow_graph.tasmovie_battle.v1." + graph_suffix,
                .nodes = {
                    {
                        .node_key = "tas_1",
                        .unit_kind = "tas_movie",
                        .display_name = "TAS Movie",
                        .inputs = {
                            { .input_key = "dtm_artifact", .data_kind = "state_artifact.dtm_artifact_id", .display_name = "DTM artifact" },
                        },
                        .possible_outputs = {
                            { .output_key = "savestate", .data_kind = "state.savestate_id", .display_name = "Output savestate" },
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
                    { .from_node_key = "tas_1", .output_key = "savestate", .to_node_key = "battle_1", .input_key = "entry_savestate" },
                },
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "savor-e2e.workflow_graph.tasmovie_battle",
                .causation_id = "savor-e2e.seed",
            },
            &saved,
            error_out)) {
        return false;
    }

    savor::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.root_scope_id = dtm_artifact_id;
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "savor-e2e";
    command.created_at_utc = savor::db::types::UtcNow().time_since_epoch().count();
    const auto registry = savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto tas_activation = savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
        registry,
        "tas_1",
        "tas_1",
        "tas_movie",
        "TAS Movie",
        std::nullopt,
        std::nullopt,
        {},
        &activation_error);
    if (!tas_activation.has_value()) {
        if (error_out) *error_out = activation_error;
        return false;
    }
    command.unit_activations.push_back(std::move(*tas_activation));
    auto battle_activation = savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
        registry,
        "battle_1",
        "battle_1",
        "battle_chain",
        "Battle Chain",
        std::optional<std::string>("authoring.battle_chain_spec"),
        battle_chain_spec_id,
        { "tas_1" },
        &activation_error);
    if (!battle_activation.has_value()) {
        if (error_out) *error_out = activation_error;
        return false;
    }
    command.unit_activations.push_back(std::move(*battle_activation));
    command.input_bindings.push_back({
        .node_key = "tas_1",
        .input_key = "dtm_artifact",
        .data_kind = "state_artifact.dtm_artifact_id",
        .ref_kind = "state_artifact",
        .ref_id = dtm_artifact_id,
        .source_kind = "external",
    });
    command.input_bindings.push_back({
        .node_key = "battle_1",
        .input_key = "initial_input_frames",
        .data_kind = "analysis.input_frame_set_id",
        .ref_kind = "au.input_set",
        .ref_id = input_set_id,
        .source_kind = "external",
    });
    AppendTasMovieHeadroomArgument(&command, options);
    AppendTasMovieRtcArgument(&command, rtc_value);
    command.arguments.push_back({ .node_key = "battle_1", .argument_key = "fake_attack_min", .value_type = "integer", .integer_value = options.battle_fake_attack_low.value_or(0), .source_kind = "scenario" });
    command.arguments.push_back({ .node_key = "battle_1", .argument_key = "fake_attack_max", .value_type = "integer", .integer_value = options.battle_fake_attack_high.value_or(0), .source_kind = "scenario" });
    return execution_db->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
}

bool SeedBattleGraphExecution(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    std::int64_t entry_savestate_id,
    std::int64_t battle_chain_spec_id,
    std::int64_t input_set_id,
    const CliOptions& options,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || execution_db == nullptr || workflow_instance_id_out == nullptr) {
        if (error_out) *error_out = "authoring/execution db unavailable";
        return false;
    }
    if (entry_savestate_id <= 0 || battle_chain_spec_id <= 0 || input_set_id <= 0) {
        if (error_out) *error_out = "entry savestate, battle chain spec, and input set ids must be > 0";
        return false;
    }

    const auto graph_suffix = BattleSeedSuffix(
        options.scenario,
        options.battle_fake_attack_low.value_or(0),
        options.battle_fake_attack_high.value_or(0),
        false);
    savor::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = "SavorE2E Battle graph " + graph_suffix,
                .description = "Battle graph-style e2e scenario with external input frames",
                .graph_version = 1,
                .graph_hash = "savor-e2e.workflow_graph.battle.v1." + graph_suffix,
                .nodes = {
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
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "savor-e2e.workflow_graph.battle",
                .causation_id = "savor-e2e.seed",
            },
            &saved,
            error_out)) {
        return false;
    }

    savor::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.root_scope_id = entry_savestate_id;
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "savor-e2e";
    command.created_at_utc = savor::db::types::UtcNow().time_since_epoch().count();
    const auto registry = savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto battle_activation = savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
        registry,
        "battle_1",
        "battle_1",
        "battle_chain",
        "Battle Chain",
        std::optional<std::string>("authoring.battle_chain_spec"),
        battle_chain_spec_id,
        {},
        &activation_error);
    if (!battle_activation.has_value()) {
        if (error_out) *error_out = activation_error;
        return false;
    }
    command.unit_activations.push_back(std::move(*battle_activation));
    command.input_bindings.push_back({
        .node_key = "battle_1",
        .input_key = "entry_savestate",
        .data_kind = "state.savestate_id",
        .ref_kind = "state.savestate",
        .ref_id = entry_savestate_id,
        .source_kind = "external",
    });
    command.input_bindings.push_back({
        .node_key = "battle_1",
        .input_key = "initial_input_frames",
        .data_kind = "analysis.input_frame_set_id",
        .ref_kind = "au.input_set",
        .ref_id = input_set_id,
        .source_kind = "external",
    });
    command.arguments.push_back({ .node_key = "battle_1", .argument_key = "fake_attack_min", .value_type = "integer", .integer_value = options.battle_fake_attack_low.value_or(0), .source_kind = "scenario" });
    command.arguments.push_back({ .node_key = "battle_1", .argument_key = "fake_attack_max", .value_type = "integer", .integer_value = options.battle_fake_attack_high.value_or(0), .source_kind = "scenario" });
    return execution_db->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
}

std::int64_t ResolveProbeRunIdFromGraph(
    const std::optional<savor::db::execution::workflow::WorkflowGraphSnapshot>& graph) {
    if (!graph.has_value()) {
        return 0;
    }
    for (const auto& step : graph->steps) {
        if ((step.step_key == "probe_1"
                || step.step_key == "probe_1/Grid"
                || step.step_key == "probe_1/Unique")
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
    const std::optional<savor::db::execution::workflow::WorkflowGraphSnapshot>& graph) {
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

bool RunSeedProbeBattleRealWorkerScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out) {
    if (db_service == nullptr) {
        if (error_out) *error_out = "DBService is null";
        return false;
    }
    if (db_service->ExecutionDb() == nullptr
        || db_service->StateDb() == nullptr
        || db_service->AnalysisDb() == nullptr
        || db_service->AuthoringDb() == nullptr) {
        if (error_out) *error_out = "one or more SavorDb contexts are unavailable";
        return false;
    }

    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::exists(worker_exe)) {
        if (error_out) *error_out = "SavorWorker.exe was not found next to SavorE2E: " + worker_exe.string();
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
    const auto battle_suffix = BattleSeedSuffix(
        options.scenario,
        options.battle_fake_attack_low.value_or(0),
        options.battle_fake_attack_high.value_or(2),
        true);
    if (!SeedBattleAuthoringRows(
            db_service->AuthoringDb(),
            battle_suffix,
            options.battle_fake_attack_low.value_or(0),
            options.battle_fake_attack_high.value_or(2),
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
            db_service->AuthoringDb(),
            db_service->AnalysisDb(),
            db_service->ExecutionDb(),
            battle_suffix,
            entry_savestate_id,
            battle_run_spec_id,
            explorer_settings_id,
            options.battle_fake_attack_low.value_or(0),
            options.battle_fake_attack_high.value_or(2),
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

    const auto scenario_workspace_root = options.workspace_root.value_or(
        std::filesystem::temp_directory_path() / "savor-e2e-default");
    auto registry_config =
        savor::db::execution::programdb::MakeProductionProgramKindRegistryConfig(
            scenario_workspace_root / "workflow-runtime");
    registry_config.battle_context.working_dir_root =
        scenario_workspace_root / "battle-context";
    registry_config.battle_single_turn.working_dir_root =
        scenario_workspace_root / "battle-single-turn";
    savor::db::execution::programdb::ProgramKindRegistry registry;
    if (!savor::db::execution::programdb::BuildProductionProgramKindRegistry(
            savor::db::execution::programdb::ProductionProgramKindRegistryDependencies{
                .execution_db = db_service->ExecutionDb(),
                .state_db = db_service->StateDb(),
                .analysis_db = db_service->AnalysisDb(),
                .authoring_db = db_service->AuthoringDb(),
            },
            std::move(registry_config),
            &registry,
            &err)) {
        if (error_out) *error_out = "failed building production program registry: " + err;
        return false;
    }

    if (!registry.HasRequiredAdapters(static_cast<std::int32_t>(savor::PK_BattleContextProbe))
        || !registry.HasRequiredAdaptersForStepKind("battle.context_probe")
        || !registry.HasRequiredAdapters(static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner))
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

    auto coordinator = savor::runner::parallel::savordb::BuildDbBackedWorkflowCoordinator(
        db_service->ExecutionDb(),
        db_service->StateDb(),
        savor::runner::parallel::savordb::DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = static_cast<std::size_t>(options.worker_count),
            .controller_sleep_ms = static_cast<std::uint32_t>(options.poll_ms),
            .worker_exe_path = worker_exe.string(),
            .iso_path = options.iso_path.string(),
            .dolphin_base_dir = options.dolphin_base_dir.string(),
            .worker_dir_root = options.worker_dir_root.value_or(
                options.workspace_root.value_or(std::filesystem::temp_directory_path() / "savor-e2e-default") / ".workers").string(),
            .visual_workers = options.visual_worker,
            .auto_resume_visual_workers = false,
            .visual_screenshot_dir = options.visual_screenshot_dir.value_or(
                options.workspace_root.value_or(std::filesystem::temp_directory_path() / "savor-e2e-default")
                    / "visual-screenshots").string(),
        },
        savor::runner::parallel::savordb::CoordinatorIntegrationConfig{},
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
    std::unordered_map<std::size_t, savor::PRProgress> last_progress_by_worker;

    coordinator.SetResultCallback([&](const savor::PRResult& result) {
        std::ostringstream line;
        line << "[battle-single-turn-worker-result] job=" << result.job_id
             << " worker=" << result.worker_id
             << " ok=" << (result.ps.ok ? "true" : "false")
             << " w_err=" << savor::WErrToString(result.ps.w_err) << "(" << static_cast<int>(result.ps.w_err) << ")";
        push_line(line.str());
    });
    coordinator.SetResultMapEventCallback([&](const std::string& line) {
        push_line(line);
    });
    coordinator.SetProgressCallback([&](const savor::PRProgress& progress) {
        std::lock_guard<std::mutex> lock(progress_mtx);
        last_progress_by_worker[progress.worker_id] = progress;
    });

    ScopedWorkflowCoordinatorService workflow_coordinator;
    if (!workflow_coordinator.Start(
            db_service->ExecutionDb(),
            db_service->AuthoringDb(),
            &registry,
            options,
            &err,
            [&](const std::string& line) {
                push_line(line);
            },
            false,
            coordinator.ItemCreditSource())) {
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
        const auto telemetry = coordinator.SnapshotTelemetry();
        const auto worker_snapshot = coordinator.SnapshotWorkers();
        RecordWorkerCoordinatorPerfSample(options, telemetry, worker_snapshot);
        latest_lines = BuildCoordinatorProgressLines(
            db_service->ExecutionDb(),
            telemetry,
            worker_snapshot,
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
            using savor::db::execution::workflow::WorkflowInstanceState;
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
    const auto final_telemetry = coordinator.SnapshotTelemetry();
    const auto final_worker_snapshot = coordinator.SnapshotWorkers();
    RecordWorkerCoordinatorPerfSample(options, final_telemetry, final_worker_snapshot);
    latest_lines = BuildCoordinatorProgressLines(
        db_service->ExecutionDb(),
        final_telemetry,
        final_worker_snapshot,
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
            return job.job_state == savor::db::BattleTurnJobState::Succeeded;
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
            : "workflow did not reach COMPLETED state before timeout - timed out";
        return false;
    }
    return true;
}

bool RunBattleWorkflowGraphRealWorkerScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out) {
    if (db_service == nullptr || !db_service->IsRunning()) {
        if (error_out) *error_out = "DBService must be running";
        return false;
    }
    if (db_service->ExecutionDb() == nullptr
        || db_service->StateDb() == nullptr
        || db_service->AnalysisDb() == nullptr
        || db_service->AuthoringDb() == nullptr) {
        if (error_out) *error_out = "one or more SavorDb contexts are unavailable";
        return false;
    }

    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::exists(worker_exe)) {
        if (error_out) *error_out = "SavorWorker.exe was not found next to SavorE2E: " + worker_exe.string();
        return false;
    }

    DurableLogFile durable_log;
    if (!durable_log.Open(options, options.scenario, error_out)) {
        return false;
    }
    std::cout << "[durable-log] path=" << durable_log.path().string() << '\n';

    std::string err;
    std::int64_t entry_savestate_id = 0;
    if (!SeedStateSavestate(db_service->StateDb(), options.savestate_file, &entry_savestate_id, &err)) {
        if (error_out) *error_out = "failed seeding StateDB battle entry savestate: " + err;
        return false;
    }

    std::int64_t battle_run_spec_id = 0;
    std::int64_t explorer_settings_id = 0;
    const auto battle_suffix = BattleSeedSuffix(
        options.scenario,
        options.battle_fake_attack_low.value_or(0),
        options.battle_fake_attack_high.value_or(0),
        false);
    if (!SeedBattleAuthoringRows(
            db_service->AuthoringDb(),
            battle_suffix,
            options.battle_fake_attack_low.value_or(0),
            options.battle_fake_attack_high.value_or(0),
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
                .name = "SavorE2E Battle chain spec " + battle_suffix,
                .description = "Battle-only graph-style chain spec",
                .battle_run_spec_id = battle_run_spec_id,
                .explorer_settings_id = explorer_settings_id,
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "savor-e2e.workflow_graph.battle",
                .causation_id = "savor-e2e.seed",
            },
            &battle_chain_spec_id,
            &err)) {
        if (error_out) *error_out = "failed seeding battle chain spec: " + err;
        return false;
    }

    std::int64_t input_set_id = 0;
    if (!EnsureBattleOnlyAuthoringInputSet(
            db_service->AuthoringDb(),
            &input_set_id,
            &err)) {
        if (error_out) *error_out = "failed seeding authored battle input set: " + err;
        return false;
    }

    std::int64_t workflow_instance_id = 0;
    if (!SeedBattleGraphExecution(
            db_service->AuthoringDb(),
            db_service->ExecutionDb(),
            entry_savestate_id,
            battle_chain_spec_id,
            input_set_id,
            options,
            &workflow_instance_id,
            &err)) {
        if (error_out) *error_out = "failed seeding Battle graph workflow execution rows: " + err;
        return false;
    }

    const auto scenario_workspace_root = options.workspace_root.value_or(
        std::filesystem::temp_directory_path() / "savor-e2e-default");
    auto registry_config =
        savor::db::execution::programdb::MakeProductionProgramKindRegistryConfig(
            scenario_workspace_root / "workflow-runtime");
    registry_config.battle_context.working_dir_root =
        scenario_workspace_root / "battle-context";
    registry_config.battle_single_turn.working_dir_root =
        scenario_workspace_root / "battle-single-turn";
    savor::db::execution::programdb::ProgramKindRegistry registry;
    if (!savor::db::execution::programdb::BuildProductionProgramKindRegistry(
            savor::db::execution::programdb::ProductionProgramKindRegistryDependencies{
                .execution_db = db_service->ExecutionDb(),
                .state_db = db_service->StateDb(),
                .analysis_db = db_service->AnalysisDb(),
                .authoring_db = db_service->AuthoringDb(),
            },
            std::move(registry_config),
            &registry,
            &err)) {
        if (error_out) *error_out = "failed building production program registry: " + err;
        return false;
    }

    if (!registry.HasRequiredAdaptersForStepKind("battle_chain")
        || !registry.HasRequiredAdaptersForStepKind("battle.context_probe")
        || !registry.HasRequiredAdaptersForStepKind("battle.single_turn")) {
        if (error_out) *error_out = "battle graph workflow descriptor registration is incomplete";
        return false;
    }

    const auto run_spec = db_service->AuthoringDb()->GetBattleRunSpec(battle_run_spec_id);
    const auto scenario_timeout_ms = run_spec.has_value()
        ? ComputeBattleScenarioTimeoutMs(*run_spec, options)
        : std::max<std::int64_t>(options.timeout_ms, 300000);

    std::cout << "[battle-graph-setup] entry_savestate_id=" << entry_savestate_id
              << " battle_run_spec_id=" << battle_run_spec_id
              << " explorer_settings_id=" << explorer_settings_id
              << " battle_chain_spec_id=" << battle_chain_spec_id
              << " input_set_id=" << input_set_id
              << " workflow_instance_id=" << workflow_instance_id
              << " fake_attacks=" << options.battle_fake_attack_low.value_or(0)
              << ".." << options.battle_fake_attack_high.value_or(0) << "\n";

    auto coordinator = savor::runner::parallel::savordb::BuildDbBackedWorkflowCoordinator(
        db_service->ExecutionDb(),
        db_service->StateDb(),
        savor::runner::parallel::savordb::DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = static_cast<std::size_t>(options.worker_count),
            .controller_sleep_ms = static_cast<std::uint32_t>(options.poll_ms),
            .worker_exe_path = worker_exe.string(),
            .iso_path = options.iso_path.string(),
            .dolphin_base_dir = options.dolphin_base_dir.string(),
            .worker_dir_root = options.worker_dir_root.value_or(
                options.workspace_root.value_or(std::filesystem::temp_directory_path() / "savor-e2e-default") / ".workers").string(),
            .visual_workers = options.visual_worker,
            .auto_resume_visual_workers = false,
            .visual_screenshot_dir = options.visual_screenshot_dir.value_or(
                options.workspace_root.value_or(std::filesystem::temp_directory_path() / "savor-e2e-default")
                    / "visual-screenshots").string(),
        },
        savor::runner::parallel::savordb::CoordinatorIntegrationConfig{},
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
    std::unordered_map<std::size_t, savor::PRProgress> last_progress_by_worker;

    coordinator.SetResultCallback([&](const savor::PRResult& result) {
        std::ostringstream line;
        line << "[battle-graph-worker-result] job=" << result.job_id
             << " worker=" << result.worker_id
             << " ok=" << (result.ps.ok ? "true" : "false")
             << " w_err=" << savor::WErrToString(result.ps.w_err) << "(" << static_cast<int>(result.ps.w_err) << ")";
        push_line(line.str());
    });
    coordinator.SetResultMapEventCallback([&](const std::string& line) {
        push_line(line);
    });
    coordinator.SetProgressCallback([&](const savor::PRProgress& progress) {
        std::lock_guard<std::mutex> lock(progress_mtx);
        last_progress_by_worker[progress.worker_id] = progress;
    });

    ScopedWorkflowCoordinatorService workflow_coordinator;
    if (!workflow_coordinator.Start(
            db_service->ExecutionDb(),
            db_service->AuthoringDb(),
            &registry,
            options,
            &err,
            [&](const std::string& line) {
                push_line(line);
            },
            false,
            coordinator.ItemCreditSource())) {
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
        const auto telemetry = coordinator.SnapshotTelemetry();
        const auto worker_snapshot = coordinator.SnapshotWorkers();
        RecordWorkerCoordinatorPerfSample(options, telemetry, worker_snapshot);
        latest_lines = BuildCoordinatorProgressLines(
            db_service->ExecutionDb(),
            telemetry,
            worker_snapshot,
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
                std::cout << "[battle-graph] ";
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
            using savor::db::execution::workflow::WorkflowInstanceState;
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
    const auto final_telemetry = coordinator.SnapshotTelemetry();
    const auto final_worker_snapshot = coordinator.SnapshotWorkers();
    RecordWorkerCoordinatorPerfSample(options, final_telemetry, final_worker_snapshot);
    latest_lines = BuildCoordinatorProgressLines(
        db_service->ExecutionDb(),
        final_telemetry,
        final_worker_snapshot,
        final_graph,
        &final_progress_snapshot);
    if (final_graph.has_value()) {
        latest_state = FormatWorkflowStateLine(*final_graph);
    }
    if (interactive_stdout) {
        progress_renderer.SetLines(latest_lines);
        progress_renderer.Render(std::cout);
    }

    const auto authored_input_frames = db_service->AuthoringDb()->ListAuthoringInputSetFrames(input_set_id);
    const auto context_probe_id = ResolveBattleContextProbeIdFromGraph(final_graph);
    const auto context_probe = db_service->AnalysisDb()->GetBattleContextProbe(context_probe_id);
    const auto waves = context_probe_id > 0
        ? db_service->AnalysisDb()->ListBattleTurnWavesForContextProbe(context_probe_id)
        : std::vector<savor::db::BattleTurnWaveSnapshot>{};
    const auto all_waves = !waves.empty()
        ? db_service->AnalysisDb()->ListBattleTurnWaves(waves.front().battle_set_id)
        : std::vector<savor::db::BattleTurnWaveSnapshot>{};
    std::size_t job_count = 0;
    std::size_t succeeded_count = 0;
    std::size_t victory_count = 0;
    for (const auto& wave : all_waves) {
        const auto jobs = db_service->AnalysisDb()->ListBattleTurnJobsForWave(wave.wave_id);
        job_count += jobs.size();
        for (const auto& job : jobs) {
            if (job.job_state == savor::db::BattleTurnJobState::Succeeded) {
                ++succeeded_count;
            }
            if (job.battle_outcome.has_value()
                && *job.battle_outcome == savor::battle::Outcome::Victory) {
                ++victory_count;
            }
        }
    }

    std::cout << "[battle-graph-final] status=" << (completed ? "success" : failed ? "failure" : "timeout") << '\n';
    std::cout << "  timeout_ms=" << scenario_timeout_ms << '\n';
    std::cout << "  " << latest_state << '\n';
    std::cout << "  workflow_instance_id=" << workflow_instance_id
              << " input_set_id=" << input_set_id
              << " input_frames=" << authored_input_frames.size()
              << " context_probe_id=" << context_probe_id
              << " first_turn_waves=" << waves.size()
              << " expected_first_turn_waves=" << authored_input_frames.size()
              << " turn_jobs=" << job_count
              << " succeeded_turn_jobs=" << succeeded_count
              << " victory_jobs=" << victory_count << '\n';

    if (!completed) {
        if (error_out) *error_out = failed
            ? "workflow did not complete successfully"
            : "workflow did not reach COMPLETED state before timeout - timed out";
        return false;
    }
    if (authored_input_frames.empty()) {
        if (error_out) *error_out = "battle graph workflow completed with an empty authored battle input set";
        return false;
    }
    if (context_probe_id <= 0 || !context_probe.has_value()) {
        if (error_out) *error_out = "battle graph workflow completed without a battle context probe output";
        return false;
    }
    if (waves.size() != authored_input_frames.size()) {
        if (error_out) *error_out = "battle context wave fanout mismatch: waves="
            + std::to_string(waves.size()) + " expected=" + std::to_string(authored_input_frames.size());
        return false;
    }
    if (job_count == 0) {
        if (error_out) *error_out = "battle graph workflow completed without turn jobs";
        return false;
    }
    return true;
}

bool RunTasMovieSeedProbeBattleWorkflowGraphRealWorkerScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out) {
    if (db_service == nullptr || !db_service->IsRunning()) {
        if (error_out) *error_out = "DBService must be running";
        return false;
    }
    if (db_service->ExecutionDb() == nullptr
        || db_service->StateDb() == nullptr
        || db_service->AnalysisDb() == nullptr
        || db_service->AuthoringDb() == nullptr) {
        if (error_out) *error_out = "one or more SavorDb contexts are unavailable";
        return false;
    }

    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::exists(worker_exe)) {
        if (error_out) *error_out = "SavorWorker.exe was not found next to SavorE2E: " + worker_exe.string();
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
    if (!SeedAuthoringSpec(db_service->AuthoringDb(), options, &seed_probe_spec_id, &err)) {
        if (error_out) *error_out = "failed seeding AuthoringDB seedprobe spec: " + err;
        return false;
    }

    std::int64_t battle_run_spec_id = 0;
    std::int64_t explorer_settings_id = 0;
    const auto battle_suffix = BattleSeedSuffix(
        options.scenario,
        options.battle_fake_attack_low.value_or(0),
        options.battle_fake_attack_high.value_or(0),
        false);
    if (!SeedBattleAuthoringRows(
            db_service->AuthoringDb(),
            battle_suffix,
            options.battle_fake_attack_low.value_or(0),
            options.battle_fake_attack_high.value_or(0),
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
                .name = "SavorE2E TAS SeedProbe Battle chain spec " + battle_suffix,
                .description = "First battle graph-style chain spec",
                .battle_run_spec_id = battle_run_spec_id,
                .explorer_settings_id = explorer_settings_id,
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "savor-e2e.workflow_graph.tasmovie_seedprobe_battle",
                .causation_id = "savor-e2e.seed",
            },
            &battle_chain_spec_id,
            &err)) {
        if (error_out) *error_out = "failed seeding battle chain spec: " + err;
        return false;
    }

    std::vector<std::int64_t> workflow_instance_ids;
    std::optional<std::int64_t> external_input_set_id;
    const bool override_input_frames = options.scenario == "tasmovie_seedprobe_battle_override";
    const bool tasmovie_battle_only = options.scenario == "tasmovie_battle";
    if (override_input_frames || tasmovie_battle_only) {
        std::int64_t seeded_input_set_id = 0;
        if (!EnsureNeutralAuthoringInputSet(
                db_service->AuthoringDb(),
                &seeded_input_set_id,
                &err)) {
            if (error_out) *error_out = "failed seeding authored neutral input set: " + err;
            return false;
        }
        external_input_set_id = seeded_input_set_id;
    }

    const auto rtc_range = ResolveTasMovieRtcRange(options, 4);
    workflow_instance_ids.reserve(static_cast<std::size_t>(rtc_range.high - rtc_range.low + 1));
    for (std::int64_t rtc = rtc_range.low; rtc <= rtc_range.high; ++rtc) {
        std::int64_t workflow_instance_id = 0;
        if (override_input_frames || tasmovie_battle_only) {
            if (!SeedTasMovieBattleGraphExecution(
                    db_service->AuthoringDb(),
                    db_service->ExecutionDb(),
                    dtm_artifact_id,
                    battle_chain_spec_id,
                    *external_input_set_id,
                    options,
                    rtc,
                    &workflow_instance_id,
                    &err)) {
                if (error_out) *error_out = "failed seeding TAS Battle graph workflow execution rows: " + err;
                return false;
            }
        } else {
            if (!SeedTasMovieSeedProbeBattleGraphExecution(
                    db_service->AuthoringDb(),
                    db_service->ExecutionDb(),
                    dtm_artifact_id,
                    seed_probe_spec_id,
                    battle_chain_spec_id,
                    options,
                    external_input_set_id,
                    rtc,
                    &workflow_instance_id,
                    &err)) {
                if (error_out) *error_out = "failed seeding graph workflow execution rows: " + err;
                return false;
            }
        }
        if (workflow_instance_id > 0) {
            workflow_instance_ids.push_back(workflow_instance_id);
        }
    }
    if (workflow_instance_ids.empty()) {
        if (error_out) *error_out = "no graph workflow instances were seeded";
        return false;
    }

    const auto scenario_workspace_root = options.workspace_root.value_or(
        std::filesystem::temp_directory_path() / "savor-e2e-default");
    auto registry_config =
        savor::db::execution::programdb::MakeProductionProgramKindRegistryConfig(
            scenario_workspace_root / "workflow-runtime");
    auto& tas_config = registry_config.tas_movie;
    tas_config.blueprint.base_dtm_artifact_id = dtm_artifact_id;
    tas_config.blueprint.rtc_low = 0;
    tas_config.blueprint.rtc_high = 0;
    tas_config.blueprint.run_ms = 0;
    tas_config.blueprint.vi_stall_ms = 2000;
    tas_config.blueprint.progress_enable = true;
    const auto tas_headroom_x10 =
        static_cast<std::uint8_t>(options.tasmovie_headroom_x10.value_or(50));
    tas_config.blueprint.headroom_x10 = tas_headroom_x10;
    tas_config.working_dir_root = scenario_workspace_root / "tasmovie";
    registry_config.battle_context.working_dir_root =
        scenario_workspace_root / "battle-context";
    registry_config.battle_single_turn.working_dir_root =
        scenario_workspace_root / "battle-single-turn";
    savor::db::execution::programdb::ProgramKindRegistry registry;
    if (!savor::db::execution::programdb::BuildProductionProgramKindRegistry(
            savor::db::execution::programdb::ProductionProgramKindRegistryDependencies{
                .execution_db = db_service->ExecutionDb(),
                .state_db = db_service->StateDb(),
                .analysis_db = db_service->AnalysisDb(),
                .authoring_db = db_service->AuthoringDb(),
            },
            std::move(registry_config),
            &registry,
            &err)) {
        if (error_out) *error_out = "failed building production program registry: " + err;
        return false;
    }

    if (!registry.HasRequiredAdapters(static_cast<std::int32_t>(savor::PK_TasMovie))
        || !registry.HasRequiredAdaptersForStepKind("tas_movie")
        || !registry.HasRequiredAdaptersForStepKind("seed_probe_chain")
        || !registry.HasRequiredAdaptersForStepKind("battle_chain")
        || !registry.HasRequiredAdaptersForStepKind("battle.context_probe")
        || !registry.HasRequiredAdaptersForStepKind("battle.single_turn")) {
        if (error_out) *error_out = "graph workflow descriptor registration is incomplete";
        return false;
    }

    const auto run_spec = db_service->AuthoringDb()->GetBattleRunSpec(battle_run_spec_id);
    const auto tas_budget_ms =
        ComputeTasMovieRunMs(options.dtm_file, tas_headroom_x10, options.timeout_ms * 2)
        + options.timeout_ms;
    const auto seedprobe_budget_ms = options.timeout_ms
        * (1
            + (static_cast<std::int64_t>(options.seedprobe_samples_per_axis.value_or(kSeedProbeSamplesPerAxis))
                * options.seedprobe_samples_per_axis.value_or(kSeedProbeSamplesPerAxis)
                * 3)
            + 25);
    const auto battle_budget_ms = run_spec.has_value()
        ? ComputeBattleScenarioTimeoutMs(*run_spec, options)
        : std::max<std::int64_t>(options.timeout_ms, 300000);
    const auto scenario_timeout_ms = tas_budget_ms + seedprobe_budget_ms + battle_budget_ms;

    std::cout << "[tasmovie-seedprobe-battle-graph-setup] dtm_artifact_id=" << dtm_artifact_id
              << " seed_probe_spec_id=" << seed_probe_spec_id
              << " battle_run_spec_id=" << battle_run_spec_id
              << " explorer_settings_id=" << explorer_settings_id
              << " battle_chain_spec_id=" << battle_chain_spec_id
              << " workflow_instances=" << workflow_instance_ids.size()
              << " first_workflow_instance_id=" << workflow_instance_ids.front()
              << " external_input_set_id=" << external_input_set_id.value_or(0)
              << " rtc=" << rtc_range.low << ".." << rtc_range.high
              << " fake_attacks=" << options.battle_fake_attack_low.value_or(0)
              << ".." << options.battle_fake_attack_high.value_or(0) << "\n";

    auto coordinator = savor::runner::parallel::savordb::BuildDbBackedWorkflowCoordinator(
        db_service->ExecutionDb(),
        db_service->StateDb(),
        savor::runner::parallel::savordb::DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = static_cast<std::size_t>(options.worker_count),
            .controller_sleep_ms = static_cast<std::uint32_t>(options.poll_ms),
            .worker_exe_path = worker_exe.string(),
            .iso_path = options.iso_path.string(),
            .dolphin_base_dir = options.dolphin_base_dir.string(),
            .worker_dir_root = options.worker_dir_root.value_or(
                options.workspace_root.value_or(std::filesystem::temp_directory_path() / "savor-e2e-default") / ".workers").string(),
            .visual_workers = options.visual_worker,
            .auto_resume_visual_workers = false,
            .visual_screenshot_dir = options.visual_screenshot_dir.value_or(
                options.workspace_root.value_or(std::filesystem::temp_directory_path() / "savor-e2e-default")
                    / "visual-screenshots").string(),
        },
        savor::runner::parallel::savordb::CoordinatorIntegrationConfig{},
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
    std::unordered_map<std::size_t, savor::PRProgress> last_progress_by_worker;

    coordinator.SetResultCallback([&](const savor::PRResult& result) {
        std::ostringstream line;
        line << "[tasmovie-seedprobe-battle-worker-result] job=" << result.job_id
             << " worker=" << result.worker_id
             << " ok=" << (result.ps.ok ? "true" : "false")
             << " w_err=" << savor::WErrToString(result.ps.w_err) << "(" << static_cast<int>(result.ps.w_err) << ")";
        push_line(line.str());
    });
    coordinator.SetResultMapEventCallback([&](const std::string& line) {
        push_line(line);
    });
    coordinator.SetProgressCallback([&](const savor::PRProgress& progress) {
        std::lock_guard<std::mutex> lock(progress_mtx);
        last_progress_by_worker[progress.worker_id] = progress;
    });

    ScopedWorkflowCoordinatorService workflow_coordinator;
    if (!workflow_coordinator.Start(
            db_service->ExecutionDb(),
            db_service->AuthoringDb(),
            &registry,
            options,
            &err,
            [&](const std::string& line) {
                push_line(line);
            },
            false,
            coordinator.ItemCreditSource())) {
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
        const auto graph = [&]() -> std::optional<savor::db::execution::workflow::WorkflowGraphSnapshot> {
            std::optional<savor::db::execution::workflow::WorkflowGraphSnapshot> last_graph;
            for (const auto workflow_id : workflow_instance_ids) {
                auto workflow_graph = db_service->ExecutionDb()->WorkflowQueryService()->GetWorkflowGraph(workflow_id);
                if (!workflow_graph.has_value()) {
                    continue;
                }
                last_graph = workflow_graph;
                if (workflow_graph->instance.state != savor::db::execution::workflow::WorkflowInstanceState::Completed) {
                    return workflow_graph;
                }
            }
            return last_graph;
        }();
        WorkerProgressById progress_snapshot;
        {
            std::lock_guard<std::mutex> lock(progress_mtx);
            progress_snapshot = last_progress_by_worker;
        }
        const auto telemetry = coordinator.SnapshotTelemetry();
        const auto worker_snapshot = coordinator.SnapshotWorkers();
        RecordWorkerCoordinatorPerfSample(options, telemetry, worker_snapshot);
        latest_lines = BuildCoordinatorProgressLines(
            db_service->ExecutionDb(),
            telemetry,
            worker_snapshot,
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
            using savor::db::execution::workflow::WorkflowInstanceState;
            bool all_completed = true;
            bool any_failed = false;
            bool all_steps_terminal = true;
            bool any_failed_step = false;
            for (const auto workflow_id : workflow_instance_ids) {
                const auto workflow_graph = db_service->ExecutionDb()->WorkflowQueryService()->GetWorkflowGraph(workflow_id);
                if (!workflow_graph.has_value()) {
                    all_completed = false;
                    all_steps_terminal = false;
                    continue;
                }
                all_completed = all_completed && workflow_graph->instance.state == WorkflowInstanceState::Completed;
                any_failed = any_failed
                    || workflow_graph->instance.state == WorkflowInstanceState::Failed
                    || workflow_graph->instance.state == WorkflowInstanceState::Canceled;
                all_steps_terminal = all_steps_terminal && AreWorkflowStepsTerminal(*workflow_graph);
                any_failed_step = any_failed_step || HasFailedWorkflowStep(*workflow_graph);
            }
            if (all_completed) {
                completed = true;
                break;
            }
            if (any_failed) {
                failed = true;
                break;
            }
            if (all_steps_terminal) {
                ++terminal_steps_seen_count;
                if (terminal_steps_seen_count >= 2) {
                    if (any_failed_step) {
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

    const auto final_graph = [&]() -> std::optional<savor::db::execution::workflow::WorkflowGraphSnapshot> {
        std::optional<savor::db::execution::workflow::WorkflowGraphSnapshot> last_graph;
        for (const auto workflow_id : workflow_instance_ids) {
            auto workflow_graph = db_service->ExecutionDb()->WorkflowQueryService()->GetWorkflowGraph(workflow_id);
            if (!workflow_graph.has_value()) {
                continue;
            }
            last_graph = workflow_graph;
            if (workflow_graph->instance.state != savor::db::execution::workflow::WorkflowInstanceState::Completed) {
                return workflow_graph;
            }
        }
        return last_graph;
    }();
    WorkerProgressById final_progress_snapshot;
    {
        std::lock_guard<std::mutex> lock(progress_mtx);
        final_progress_snapshot = last_progress_by_worker;
    }
    const auto final_telemetry = coordinator.SnapshotTelemetry();
    const auto final_worker_snapshot = coordinator.SnapshotWorkers();
    RecordWorkerCoordinatorPerfSample(options, final_telemetry, final_worker_snapshot);
    latest_lines = BuildCoordinatorProgressLines(
        db_service->ExecutionDb(),
        final_telemetry,
        final_worker_snapshot,
        final_graph,
        &final_progress_snapshot);
    if (final_graph.has_value()) {
        latest_state = FormatWorkflowStateLine(*final_graph);
    }
    if (interactive_stdout) {
        progress_renderer.SetLines(latest_lines);
        progress_renderer.Render(std::cout);
    }

    std::size_t total_unique_count = 0;
    std::size_t total_first_turn_waves = 0;
    std::size_t total_expected_first_turn_waves = 0;
    std::size_t context_probe_count = 0;
    std::int64_t first_probe_run_id = 0;
    std::int64_t first_context_probe_id = 0;
    std::size_t job_count = 0;
    std::size_t succeeded_count = 0;
    std::size_t victory_count = 0;
    bool missing_probe_run = false;
    bool missing_battle_context = false;
    bool wave_fanout_mismatch = false;
    const auto authored_input_frames = external_input_set_id.has_value()
        ? db_service->AuthoringDb()->ListAuthoringInputSetFrames(*external_input_set_id)
        : std::vector<savor::db::AuthoringInputSetFrameSnapshot>{};
    for (const auto workflow_id : workflow_instance_ids) {
        const auto workflow_graph = db_service->ExecutionDb()->WorkflowQueryService()->GetWorkflowGraph(workflow_id);
        if (!workflow_graph.has_value()) {
            missing_battle_context = true;
            continue;
        }
        const auto graph_probe_run_id = ResolveProbeRunIdFromGraph(workflow_graph);
        if (first_probe_run_id <= 0) {
            first_probe_run_id = graph_probe_run_id;
        }
        const auto unique_rows = db_service->AnalysisDb()->ListSeedProbeUniqueSeeds(graph_probe_run_id);
        total_unique_count += unique_rows.size();
        if (!override_input_frames && !tasmovie_battle_only && (graph_probe_run_id <= 0 || unique_rows.empty())) {
            missing_probe_run = true;
        }

        const auto context_probe_id = ResolveBattleContextProbeIdFromGraph(workflow_graph);
        if (first_context_probe_id <= 0) {
            first_context_probe_id = context_probe_id;
        }
        const auto context_probe = db_service->AnalysisDb()->GetBattleContextProbe(context_probe_id);
        if (context_probe_id <= 0 || !context_probe.has_value()) {
            missing_battle_context = true;
            continue;
        }
        ++context_probe_count;
        const auto waves = db_service->AnalysisDb()->ListBattleTurnWavesForContextProbe(context_probe_id);
        const auto all_waves = !waves.empty()
            ? db_service->AnalysisDb()->ListBattleTurnWaves(waves.front().battle_set_id)
            : std::vector<savor::db::BattleTurnWaveSnapshot>{};
        const auto expected_first_turn_waves = external_input_set_id.has_value()
            ? authored_input_frames.size()
            : unique_rows.size();
        total_first_turn_waves += waves.size();
        total_expected_first_turn_waves += expected_first_turn_waves;
        if (waves.size() != expected_first_turn_waves) {
            wave_fanout_mismatch = true;
        }
        for (const auto& wave : all_waves) {
            const auto jobs = db_service->AnalysisDb()->ListBattleTurnJobsForWave(wave.wave_id);
            job_count += jobs.size();
            for (const auto& job : jobs) {
                if (job.job_state == savor::db::BattleTurnJobState::Succeeded) {
                    ++succeeded_count;
                }
                if (job.battle_outcome.has_value()
                    && *job.battle_outcome == savor::battle::Outcome::Victory) {
                    ++victory_count;
                }
            }
        }
    }

    std::cout << "[tasmovie-seedprobe-battle-graph-final] status=" << (completed ? "success" : failed ? "failure" : "timeout") << '\n';
    std::cout << "  timeout_ms=" << scenario_timeout_ms << '\n';
    std::cout << "  " << latest_state << '\n';
    std::cout << "  workflow_instances=" << workflow_instance_ids.size()
              << " probe_run_id=" << first_probe_run_id
              << " unique_count=" << total_unique_count
              << " context_probe_id=" << first_context_probe_id
              << " context_probe_count=" << context_probe_count
              << " first_turn_waves=" << total_first_turn_waves
              << " expected_first_turn_waves=" << total_expected_first_turn_waves
              << " turn_jobs=" << job_count
              << " succeeded_turn_jobs=" << succeeded_count
              << " victory_jobs=" << victory_count << '\n';

    if (!completed) {
        if (error_out) *error_out = failed
            ? "workflow did not complete successfully"
            : "workflow did not reach COMPLETED state before timeout - timed out";
        return false;
    }
    if (missing_probe_run) {
        if (error_out) *error_out = "graph workflow completed without a SeedProbe run with uniques";
        return false;
    }
    if (total_expected_first_turn_waves == 0) {
        if (error_out) *error_out = external_input_set_id.has_value()
            ? "graph workflow completed with an empty authored battle input set"
            : "graph workflow completed without battle input frames";
        return false;
    }
    if (missing_battle_context) {
        if (error_out) *error_out = "graph workflow completed without a battle context probe output";
        return false;
    }
    if (wave_fanout_mismatch) {
        if (error_out) *error_out = "battle context wave fanout mismatch: waves="
            + std::to_string(total_first_turn_waves) + " expected=" + std::to_string(total_expected_first_turn_waves);
        return false;
    }
    if (victory_count == 0) {
        if (error_out) *error_out = "battle graph workflow completed without a victory job";
        return false;
    }
    return true;
}

bool RunTasMovieSeedProbeBattleOverrideWorkflowGraphRealWorkerScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out) {
    return RunTasMovieSeedProbeBattleWorkflowGraphRealWorkerScenario(options, argv0, db_service, error_out);
}

bool RunTasMovieBattleWorkflowGraphRealWorkerScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out) {
    return RunTasMovieSeedProbeBattleWorkflowGraphRealWorkerScenario(options, argv0, db_service, error_out);
}

} // namespace savor::e2e
