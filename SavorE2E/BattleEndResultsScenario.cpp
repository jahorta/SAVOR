#include "BattleEndResultsScenario.h"

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
#include "CoordinatorProgress.h"
#include "DbSetup.h"
#include "DurableLogFile.h"
#include "Execution/DBWorkflowCoordinatorFactory.h"
#include "Execution/DBWorkflowWorkerCoordinator.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/ProgramDB/ProductionProgramKindRegistry.h"
#include "Execution/Workflow/WorkflowComposition.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Execution/Workflow/WorkflowUnitActivationFactory.h"
#include "MultiLineProgressRenderer.h"
#include "Runner/IPC/Wire.h"
#include "WorkerCoordinatorPerf.h"

#ifdef GetJob
#undef GetJob
#endif

namespace savor::e2e {
namespace {

constexpr const char* kCompletionUnitKind = "battle_completion";
constexpr const char* kCompletionStepKind = "battle.completion";
constexpr const char* kCompletionNodeKey = "battle_completion_1";
constexpr const char* kSeedProbeUnitKind = "field_return_seed_probe";
constexpr const char* kSeedProbeStepKind = "battle.field_return_seed_probe";
constexpr const char* kSeedProbeGridStepKind = "battle.field_return_seed_probe.grid";
constexpr const char* kSeedProbeUniqueStepKind = "battle.field_return_seed_probe.unique";
constexpr const char* kSeedProbeMaterializeStepKind = "battle.field_return_seed_probe.materialize";
constexpr const char* kSeedProbeNodeKey = "field_return_seed_probe_1";
constexpr const char* kResultsUnitKind = "battle_results_screen";
constexpr const char* kResultsStepKind = "battle.results_screen";
constexpr const char* kResultsNodeKey = "battle_results_screen_1";

const char* SeedSelectorName(BattleEndSeedSelector selector) {
    switch (selector) {
    case BattleEndSeedSelector::Neutral: return "neutral";
    case BattleEndSeedSelector::SeedValue: return "seed_value";
    case BattleEndSeedSelector::SeedDelta: return "seed_delta";
    }
    return "neutral";
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

const char* ToString(savor::db::execution::workflow::WorkflowStepState state) {
    using savor::db::execution::workflow::WorkflowStepState;
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

std::filesystem::path WorkspaceRoot(const CliOptions& options) {
    return options.workspace_root.value_or(
        std::filesystem::temp_directory_path() / "savor-e2e-default");
}

std::string DescribeWorkflow(
    const std::optional<savor::db::execution::workflow::WorkflowGraphSnapshot>& graph) {
    if (!graph.has_value()) {
        return "workflow=unavailable";
    }
    std::ostringstream out;
    out << "workflow=" << ToString(graph->instance.state);
    for (const auto& step : graph->steps) {
        out << " step=" << step.step_key << ':' << ToString(step.state);
        if (step.blocked_reason.has_value() && !step.blocked_reason->empty()) {
            out << " blocked_reason=\"" << *step.blocked_reason << '\"';
        }
    }
    for (const auto& activation : graph->unit_activations) {
        if (activation.failure_code.has_value() && !activation.failure_code->empty()) {
            out << " failure_code=\"" << *activation.failure_code << '\"';
        }
        if (activation.failure_text.has_value() && !activation.failure_text->empty()) {
            out << " failure_text=\"" << *activation.failure_text << '\"';
        }
    }
    return out.str();
}

bool CreateBattleEndWorkflow(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    const BattleEndWorkflowLaunchOptions& launch,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || execution_db == nullptr || workflow_instance_id_out == nullptr) {
        if (error_out != nullptr) *error_out = "battle-end/results authoring or execution database is unavailable";
        return false;
    }
    if (launch.source_victory_savestate_id <= 0 || launch.seed_probe_spec_id <= 0) {
        if (error_out != nullptr) *error_out = "battle-end source savestate and seed-probe spec ids must be positive";
        return false;
    }
    if (launch.seed_selector == BattleEndSeedSelector::Neutral
        && launch.seed_selector_value.has_value()) {
        if (error_out != nullptr) *error_out = "neutral battle-end seed selection cannot carry a value";
        return false;
    }
    if (launch.seed_selector != BattleEndSeedSelector::Neutral
        && !launch.seed_selector_value.has_value()) {
        if (error_out != nullptr) *error_out = "exact battle-end seed selection requires a value";
        return false;
    }

    savor::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = "SavorE2E Battle End",
                .description = "Coordinator-routed battle completion, field-return seed selection, and results-screen workflow.",
                .hidden = true,
                .graph_version = 2,
                .graph_hash = "savor-e2e.workflow_graph.battle_end.v2",
                .nodes = {
                    {
                        .node_key = kCompletionNodeKey,
                        .unit_kind = kCompletionUnitKind,
                        .display_name = "Battle Completion",
                        .inputs = {
                            {
                                .input_key = "entry_savestate",
                                .data_kind = "state.savestate_id",
                                .display_name = "Battle Victory savestate",
                            },
                        },
                        .possible_outputs = {
                            {
                                .output_key = "completion",
                                .data_kind = "analysis_battle.battle_completion_id",
                                .display_name = "Battle completion",
                            },
                        },
                    },
                    {
                        .node_key = kSeedProbeNodeKey,
                        .unit_kind = kSeedProbeUnitKind,
                        .display_name = "Field Return Seed Probe",
                        .authored_ref_kind = std::string("seed_probe_spec"),
                        .authored_ref_id = launch.seed_probe_spec_id,
                        .inputs = {
                            {
                                .input_key = "completion",
                                .data_kind = "analysis_battle.battle_completion_id",
                                .display_name = "Battle completion",
                            },
                        },
                        .possible_outputs = {
                            {
                                .output_key = "seeded_savestate",
                                .data_kind = "state.savestate_id",
                                .display_name = "Field-return seeded savestate",
                            },
                        },
                    },
                    {
                        .node_key = kResultsNodeKey,
                        .unit_kind = kResultsUnitKind,
                        .display_name = "Battle Results Screen",
                        .inputs = {
                            {
                                .input_key = "completion",
                                .data_kind = "analysis_battle.battle_completion_id",
                                .display_name = "Battle completion",
                            },
                            {
                                .input_key = "seeded_savestate",
                                .data_kind = "state.savestate_id",
                                .display_name = "Field-return seeded savestate",
                            },
                        },
                        .possible_outputs = {
                            {
                                .output_key = "terminal_savestate",
                                .data_kind = "state.savestate_id",
                                .display_name = "BATTLE_END savestate",
                            },
                        },
                    },
                },
                .edges = {
                    {
                        .from_node_key = kCompletionNodeKey,
                        .output_key = "completion",
                        .to_node_key = kSeedProbeNodeKey,
                        .input_key = "completion",
                    },
                    {
                        .from_node_key = kCompletionNodeKey,
                        .output_key = "completion",
                        .to_node_key = kResultsNodeKey,
                        .input_key = "completion",
                    },
                    {
                        .from_node_key = kSeedProbeNodeKey,
                        .output_key = "seeded_savestate",
                        .to_node_key = kResultsNodeKey,
                        .input_key = "seeded_savestate",
                    },
                },
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "savor-e2e.battle-end",
                .causation_id = "savestate-" + std::to_string(launch.source_victory_savestate_id),
            },
            &saved,
            error_out)) {
        return false;
    }

    const auto unit_registry = savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto completion_activation = savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
        unit_registry,
        kCompletionNodeKey,
        kCompletionNodeKey,
        kCompletionUnitKind,
        "Battle Completion",
        std::nullopt,
        std::nullopt,
        {},
        &activation_error);
    if (!completion_activation.has_value()) {
        if (error_out != nullptr) {
            *error_out = "failed building hidden battle completion activation: " + activation_error;
        }
        return false;
    }
    auto seed_probe_activation = savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
        unit_registry,
        kSeedProbeNodeKey,
        kSeedProbeNodeKey,
        kSeedProbeUnitKind,
        "Field Return Seed Probe",
        std::optional<std::string>("seed_probe_spec"),
        launch.seed_probe_spec_id,
        { kCompletionNodeKey },
        &activation_error);
    if (!seed_probe_activation.has_value()) {
        if (error_out != nullptr) {
            *error_out = "failed building field-return seed-probe activation: " + activation_error;
        }
        return false;
    }
    auto results_activation = savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
        unit_registry,
        kResultsNodeKey,
        kResultsNodeKey,
        kResultsUnitKind,
        "Battle Results Screen",
        std::nullopt,
        std::nullopt,
        { kCompletionNodeKey, kSeedProbeNodeKey },
        &activation_error);
    if (!results_activation.has_value()) {
        if (error_out != nullptr) {
            *error_out = "failed building battle results-screen activation: " + activation_error;
        }
        return false;
    }

    savor::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.root_scope_id = launch.source_victory_savestate_id;
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "savor-e2e";
    command.created_at_utc = savor::db::types::UtcNow().time_since_epoch().count();
    command.unit_activations.push_back(std::move(*completion_activation));
    command.unit_activations.push_back(std::move(*seed_probe_activation));
    command.unit_activations.push_back(std::move(*results_activation));
    command.input_bindings.push_back({
        .node_key = kCompletionNodeKey,
        .input_key = "entry_savestate",
        .data_kind = "state.savestate_id",
        .ref_kind = "state.savestate",
        .ref_id = launch.source_victory_savestate_id,
        .source_kind = "external",
    });
    command.arguments.push_back({
        .node_key = kSeedProbeNodeKey,
        .argument_key = "seed_selector",
        .value_type = "text",
        .text_value = std::string(SeedSelectorName(launch.seed_selector)),
        .source_kind = "scenario",
    });
    if (launch.seed_selector_value.has_value()) {
        command.arguments.push_back({
            .node_key = kSeedProbeNodeKey,
            .argument_key = "seed_selector_value",
            .value_type = "integer",
            .integer_value = static_cast<std::int64_t>(*launch.seed_selector_value),
            .source_kind = "scenario",
        });
    }
    return execution_db->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
}

bool VerifyCompletedBattleEndWorkflow(
    savor::db::core::DBService* db_service,
    std::int64_t workflow_instance_id,
    std::int64_t source_savestate_id,
    std::string* summary_out,
    std::string* error_out) {
    using namespace savor::db;
    using namespace savor::db::execution::workflow;
    if (db_service == nullptr || db_service->ExecutionDb() == nullptr
        || db_service->AnalysisDb() == nullptr || db_service->StateDb() == nullptr
        || db_service->ExecutionDb()->WorkflowQueryService() == nullptr) {
        if (error_out != nullptr) *error_out = "battle-end verification databases are unavailable";
        return false;
    }
    const auto graph = db_service->ExecutionDb()->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    const auto outputs = db_service->ExecutionDb()->WorkflowQueryService()->ListStepOutputs(workflow_instance_id);
    if (!graph.has_value() || graph->instance.state != WorkflowInstanceState::Completed
        || graph->unit_activations.size() != 3 || outputs.size() != 3) {
        if (error_out != nullptr) *error_out = "completed workflow does not have exactly three phase activations and outputs";
        return false;
    }
    const auto find_output = [&](std::string_view node, std::string_view key, std::string_view kind) {
        std::vector<const WorkflowStepOutputRecord*> matches;
        for (const auto& output : outputs) {
            if (output.graph_node_key == node && output.output_key == key && output.data_kind == kind
                && output.ref_id > 0) matches.push_back(&output);
        }
        return matches;
    };
    const auto completion_outputs = find_output(kCompletionNodeKey, "completion", "analysis_battle.battle_completion_id");
    const auto seed_outputs = find_output(kSeedProbeNodeKey, "seeded_savestate", "state.savestate_id");
    const auto terminal_outputs = find_output(kResultsNodeKey, "terminal_savestate", "state.savestate_id");
    if (completion_outputs.size() != 1 || seed_outputs.size() != 1 || terminal_outputs.size() != 1
        || completion_outputs.front()->ref_kind != "analysis_battle.battle_completion"
        || seed_outputs.front()->ref_kind != "state.savestate"
        || terminal_outputs.front()->ref_kind != "state.savestate") {
        if (error_out != nullptr) *error_out = "workflow phase outputs are missing or ambiguous";
        return false;
    }

    const auto completion_id = completion_outputs.front()->ref_id;
    const auto seeded_savestate_id = seed_outputs.front()->ref_id;
    const auto terminal_savestate_id = terminal_outputs.front()->ref_id;
    const auto completion = db_service->AnalysisDb()->GetBattleCompletion(completion_id);
    const auto terminal_state = db_service->StateDb()->GetSavestate(terminal_savestate_id);
    if (!completion.has_value() || completion->status != "COMPLETED"
        || !completion->exec_job_id.has_value() || !completion->completion_savestate_id.has_value()
        || completion->entry_savestate_id != source_savestate_id
        || !completion->manifest_artifact_id.has_value()
        || !terminal_state.has_value() || terminal_state->savestate_type != "BATTLE_END") {
        if (error_out != nullptr) *error_out = "completion aggregate or terminal BATTLE_END savestate is invalid";
        return false;
    }
    const auto completion_job = (db_service->ExecutionDb()->GetJob)(*completion->exec_job_id);
    if (!completion_job.has_value() || completion_job->state != "SUCCEEDED"
        || completion_job->program_kind != static_cast<std::int32_t>(savor::PK_BattleCompletionRunner)
        || completion_job->program_ref_kind != "analysis_battle.battle_completion"
        || completion_job->program_ref_id != completion_id) {
        if (error_out != nullptr) *error_out = "completion aggregate is not bound to its successful execution job";
        return false;
    }

    const auto completion_derivations = db_service->StateDb()->ListIncomingSavestateDerivations(
        *completion->completion_savestate_id);
    const auto seed_derivations = db_service->StateDb()->ListIncomingSavestateDerivations(seeded_savestate_id);
    const auto terminal_derivations = db_service->StateDb()->ListIncomingSavestateDerivations(terminal_savestate_id);
    const auto completion_edge_count = std::count_if(
        completion_derivations.begin(), completion_derivations.end(), [&](const auto& row) {
            return row.from_savestate_id == source_savestate_id && row.method_kind == "battle_completion"
                && row.source_context_kind == "analysis_battle.battle_completion"
                && row.source_context_id == completion_id;
        });
    const auto seed_edge = std::find_if(seed_derivations.begin(), seed_derivations.end(), [&](const auto& row) {
        return row.from_savestate_id == *completion->completion_savestate_id
            && row.method_kind == "field_return_seed_materialization"
            && (row.source_context_kind == "analysisseedprobe.neutral_seed"
                || row.source_context_kind == "analysisseedprobe.unique_seed")
            && row.source_context_id > 0;
    });
    const auto results_edge = std::find_if(terminal_derivations.begin(), terminal_derivations.end(), [&](const auto& row) {
        return row.from_savestate_id == seeded_savestate_id && row.method_kind == "battle_results_screen"
            && row.source_context_kind == "analysis_battle.battle_results" && row.source_context_id > 0;
    });
    if (completion_derivations.size() != 1 || seed_derivations.size() != 1
        || terminal_derivations.size() != 1 || completion_edge_count != 1
        || seed_edge == seed_derivations.end() || results_edge == terminal_derivations.end()) {
        if (error_out != nullptr) *error_out = "battle-end savestate lineage is incomplete or ambiguous";
        return false;
    }

    const auto results = db_service->AnalysisDb()->GetBattleResults(results_edge->source_context_id);
    if (!results.has_value() || results->status != "COMPLETED" || !results->exec_job_id.has_value()
        || results->battle_completion_id != completion_id
        || results->entry_savestate_id != seeded_savestate_id
        || results->final_savestate_id != std::optional<std::int64_t>(terminal_savestate_id)
        || results->selected_seed_ref_kind != seed_edge->source_context_kind
        || results->selected_seed_ref_id != seed_edge->source_context_id
        || results->rng_effect_kind != RngEffectKind::Preserve
        || results->fixed_draw_count != std::optional<std::int64_t>(0)
        || !results->entry_rng_seed.has_value() || !results->final_rng_seed.has_value()
        || *results->entry_rng_seed != results->selected_seed_value
        || *results->final_rng_seed != results->selected_seed_value
        || !results->result_artifact_id.has_value()) {
        if (error_out != nullptr) *error_out = "battle results aggregate violates selected-seed or Preserve(0) invariants";
        return false;
    }
    const auto results_job = (db_service->ExecutionDb()->GetJob)(*results->exec_job_id);
    if (!results_job.has_value() || results_job->state != "SUCCEEDED"
        || results_job->program_kind != static_cast<std::int32_t>(savor::PK_BattleResultsScreenRunner)
        || results_job->program_ref_kind != "analysis_battle.battle_results"
        || results_job->program_ref_id != results->battle_results_id) {
        if (error_out != nullptr) *error_out = "battle results aggregate is not bound to its successful execution job";
        return false;
    }

    std::int64_t selected_seed_value = -1;
    std::int64_t selected_probe_run_id = 0;
    if (results->selected_seed_ref_kind == "analysisseedprobe.neutral_seed") {
        const auto seed = db_service->AnalysisDb()->GetSeedProbeNeutralSeed(results->selected_seed_ref_id);
        if (seed.has_value()) {
            selected_seed_value = seed->neutral_seed_value;
            selected_probe_run_id = seed->probe_run_id;
        }
    } else if (results->selected_seed_ref_kind == "analysisseedprobe.unique_seed") {
        const auto seed = db_service->AnalysisDb()->GetSeedProbeUniqueSeed(results->selected_seed_ref_id);
        if (seed.has_value()) {
            selected_seed_value = seed->seed_value;
            selected_probe_run_id = seed->probe_run_id;
        }
    }
    const auto probe_run = db_service->AnalysisDb()->GetSeedProbeRun(selected_probe_run_id);
    if (selected_seed_value != results->selected_seed_value || !probe_run.has_value()
        || probe_run->codec_version != 2 || probe_run->probe_flavor != "FIELD_RETURN"
        || probe_run->entry_savestate_id != *completion->completion_savestate_id) {
        if (error_out != nullptr) *error_out = "selected direct seed row does not belong to the completion-state probe run";
        return false;
    }

    if (summary_out != nullptr) {
        *summary_out = "completion_id=" + std::to_string(completion_id)
            + " completion_job=" + std::to_string(*completion->exec_job_id)
            + " seed_ref=" + results->selected_seed_ref_kind + ":" + std::to_string(results->selected_seed_ref_id)
            + " seed=" + std::to_string(results->selected_seed_value)
            + " results_id=" + std::to_string(results->battle_results_id)
            + " results_job=" + std::to_string(*results->exec_job_id)
            + " terminal_savestate_id=" + std::to_string(terminal_savestate_id)
            + " rng_effect=PRESERVE fixed_draw_count=0 successor=none";
    }
    if (error_out != nullptr) error_out->clear();
    return true;
}

} // namespace

bool RunBattleEndResultsScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out) {
    if (db_service == nullptr || !db_service->IsRunning()) {
        if (error_out != nullptr) *error_out = "DBService must be running";
        return false;
    }
    if (db_service->ExecutionDb() == nullptr
        || db_service->StateDb() == nullptr
        || db_service->AnalysisDb() == nullptr
        || db_service->AuthoringDb() == nullptr) {
        if (error_out != nullptr) *error_out = "one or more SavorDb contexts are unavailable";
        return false;
    }
    if (!options.source_savestate_id.has_value() || *options.source_savestate_id <= 0) {
        if (error_out != nullptr) {
            *error_out = "battle_end requires --source-savestate-id with a positive BattleSingleTurn Victory savestate id";
        }
        return false;
    }
    const auto source_savestate_id = *options.source_savestate_id;
    if (!db_service->StateDb()->GetSavestate(source_savestate_id).has_value()) {
        if (error_out != nullptr) {
            *error_out = "battle_end source savestate id " + std::to_string(source_savestate_id)
                + " was not found in StateDB";
        }
        return false;
    }

    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::exists(worker_exe)) {
        if (error_out != nullptr) {
            *error_out = "SavorWorker.exe was not found next to SavorE2E: " + worker_exe.string();
        }
        return false;
    }

    DurableLogFile durable_log;
    if (!durable_log.Open(options, options.scenario, error_out)) {
        return false;
    }
    std::cout << "[durable-log] path=" << durable_log.path().string() << '\n';

    const auto runtime_root = WorkspaceRoot(options) / "workflow-runtime";
    auto registry_config =
        savor::db::execution::programdb::MakeProductionProgramKindRegistryConfig(
            runtime_root);
    registry_config.battle_end.working_dir_root =
        runtime_root / "battle-end-results";
    std::string error;
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
            &error)) {
        if (error_out != nullptr) {
            *error_out = "failed building production program registry: " + error;
        }
        return false;
    }

    const auto descriptor_is_coordinator_only = [&](const char* step_kind) {
        const auto* descriptor = registry.FindForStepKind(step_kind);
        return descriptor != nullptr
            && descriptor->job_persistence == nullptr
            && descriptor->graph_job_persistence != nullptr
            && registry.HasRequiredAdaptersForStepKind(step_kind);
    };
    if (!descriptor_is_coordinator_only(kCompletionStepKind)
        || !descriptor_is_coordinator_only(kSeedProbeStepKind)
        || !descriptor_is_coordinator_only(kResultsStepKind)
        || !registry.HasRequiredAdaptersForStepKind(kSeedProbeGridStepKind)
        || !registry.HasRequiredAdaptersForStepKind(kSeedProbeUniqueStepKind)
        || !registry.HasRequiredAdaptersForStepKind(kSeedProbeMaterializeStepKind)) {
        if (error_out != nullptr) {
            *error_out = "battle-end workflow descriptors are not registered with all required graph and dynamic adapters";
        }
        return false;
    }

    std::int64_t seed_probe_spec_id = 0;
    if (!SeedAuthoringSpec(
            db_service->AuthoringDb(),
            options,
            &seed_probe_spec_id,
            &error)
        || seed_probe_spec_id <= 0) {
        if (error_out != nullptr) *error_out = "failed creating battle-end seed-probe spec: " + error;
        return false;
    }
    std::int64_t workflow_instance_id = 0;
    BattleEndWorkflowLaunchOptions launch{};
    launch.source_victory_savestate_id = source_savestate_id;
    launch.seed_probe_spec_id = seed_probe_spec_id;
    if (options.battle_end_seed_selector == "seed_value") {
        launch.seed_selector = BattleEndSeedSelector::SeedValue;
    } else if (options.battle_end_seed_selector == "seed_delta") {
        launch.seed_selector = BattleEndSeedSelector::SeedDelta;
    }
    launch.seed_selector_value = options.battle_end_seed_value;
    if (!CreateBattleEndWorkflow(
            db_service->AuthoringDb(),
            db_service->ExecutionDb(),
            launch,
            &workflow_instance_id,
            &error)) {
        if (error_out != nullptr) *error_out = "failed creating battle-end/results workflow: " + error;
        return false;
    }

    std::cout << "[battle-end-setup] source_savestate_id=" << source_savestate_id
              << " workflow_instance_id=" << workflow_instance_id
              << " seed_probe_spec_id=" << seed_probe_spec_id
              << " seed_selector=" << SeedSelectorName(launch.seed_selector);
    if (launch.seed_selector_value.has_value()) {
        std::cout << " seed_selector_value=" << *launch.seed_selector_value;
    }
    std::cout << '\n';
    durable_log.AppendLine(
        "[battle-end-setup] source_savestate_id=" + std::to_string(source_savestate_id)
        + " workflow_instance_id=" + std::to_string(workflow_instance_id)
        + " seed_probe_spec_id=" + std::to_string(seed_probe_spec_id)
        + " seed_selector=" + SeedSelectorName(launch.seed_selector)
        + (launch.seed_selector_value.has_value()
            ? " seed_selector_value=" + std::to_string(*launch.seed_selector_value)
            : ""));

    auto coordinator = savor::runner::parallel::savordb::BuildDbBackedWorkflowCoordinator(
        db_service->ExecutionDb(),
        db_service->StateDb(),
        savor::runner::parallel::savordb::DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = static_cast<std::size_t>(options.worker_count),
            .controller_sleep_ms = static_cast<std::uint32_t>(options.poll_ms),
            .worker_exe_path = worker_exe.string(),
            .iso_path = options.iso_path.string(),
            .dolphin_base_dir = options.dolphin_base_dir.string(),
            .worker_dir_root = options.worker_dir_root.value_or(WorkspaceRoot(options) / ".workers").string(),
            .visual_workers = options.visual_worker,
            .auto_resume_visual_workers = false,
            .visual_screenshot_dir = options.visual_screenshot_dir.value_or(
                WorkspaceRoot(options) / "visual-screenshots").string(),
        },
        savor::runner::parallel::savordb::CoordinatorIntegrationConfig{
            .strict_smoke_terminal_on_failure = true,
        },
        &registry);

    std::mutex event_mutex;
    std::deque<std::string> pending_events;
    std::vector<std::string> all_events;
    const auto push_event = [&](std::string line) {
        durable_log.AppendLine(line);
        std::lock_guard<std::mutex> lock(event_mutex);
        all_events.push_back(line);
        pending_events.push_back(std::move(line));
    };
    const auto drain_events = [&]() {
        std::deque<std::string> out;
        std::lock_guard<std::mutex> lock(event_mutex);
        std::swap(out, pending_events);
        return out;
    };

    std::mutex progress_mutex;
    WorkerProgressById progress_by_worker;
    coordinator.SetResultCallback([&](const savor::PRResult& result) {
        std::ostringstream line;
        line << "[battle-end-worker-result] job=" << result.job_id
             << " worker=" << result.worker_id
             << " ok=" << (result.ps.ok ? "true" : "false")
             << " w_err=" << savor::WErrToString(result.ps.w_err)
             << '(' << static_cast<int>(result.ps.w_err) << ')';
        push_event(line.str());
    });
    coordinator.SetResultMapEventCallback([&](const std::string& line) {
        push_event(line);
    });
    coordinator.SetProgressCallback([&](const savor::PRProgress& progress) {
        std::lock_guard<std::mutex> lock(progress_mutex);
        progress_by_worker[progress.worker_id] = progress;
    });

    ScopedWorkflowCoordinatorService workflow_coordinator;
    if (!workflow_coordinator.Start(
            db_service->ExecutionDb(),
            db_service->AuthoringDb(),
            &registry,
            options,
            &error,
            [&](const std::string& line) {
                push_event(line);
            },
            true)) {
        if (error_out != nullptr) *error_out = error;
        return false;
    }

    coordinator.Start();
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(std::max<std::int64_t>(1, options.timeout_ms));
    const bool interactive_stdout = IsInteractiveStdout();
    MultiLineProgressRenderer progress_renderer;
    const auto refresh_cadence = std::chrono::milliseconds(100);
    std::optional<savor::db::execution::workflow::WorkflowGraphSnapshot> graph;
    bool completed = false;
    bool failed = false;
    std::size_t poll_count = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        ++poll_count;
        const auto event_lines = drain_events();
        graph = db_service->ExecutionDb()->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
        WorkerProgressById progress_snapshot;
        {
            std::lock_guard<std::mutex> lock(progress_mutex);
            progress_snapshot = progress_by_worker;
        }
        const auto telemetry = coordinator.SnapshotTelemetry();
        const auto workers = coordinator.SnapshotWorkers();
        RecordWorkerCoordinatorPerfSample(options, telemetry, workers);
        const auto progress_lines = BuildCoordinatorProgressLines(
            db_service->ExecutionDb(),
            telemetry,
            workers,
            graph,
            &progress_snapshot);
        if (interactive_stdout) {
            progress_renderer.SetLines(progress_lines);
            progress_renderer.WriteEventLines(std::cout, event_lines);
            progress_renderer.RenderIfDue(
                std::cout,
                std::chrono::steady_clock::now(),
                refresh_cadence);
        } else {
            for (const auto& line : event_lines) {
                std::cout << line << '\n';
            }
            if (poll_count == 1 || (poll_count % 10) == 0) {
                std::cout << "[battle-end] " << DescribeWorkflow(graph) << '\n';
            }
        }

        if (graph.has_value()) {
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
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max<std::int64_t>(1, options.poll_ms)));
    }

    coordinator.Stop();
    workflow_coordinator.Stop();
    for (const auto& line : drain_events()) {
        std::cout << line << '\n';
    }
    graph = db_service->ExecutionDb()->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);

    const auto final_description = DescribeWorkflow(graph);
    std::cout << "[battle-end-final] status="
              << (completed ? "success" : failed ? "failure" : "timeout") << '\n'
              << "  source_savestate_id=" << source_savestate_id << '\n'
              << "  workflow_instance_id=" << workflow_instance_id << '\n'
              << "  " << final_description << '\n';
    durable_log.AppendLine(
        "[battle-end-final] status="
        + std::string(completed ? "success" : failed ? "failure" : "timeout")
        + " source_savestate_id=" + std::to_string(source_savestate_id)
        + " workflow_instance_id=" + std::to_string(workflow_instance_id)
        + " " + final_description);

    if (!completed) {
        std::string last_failure_event;
        {
            std::lock_guard<std::mutex> lock(event_mutex);
            for (auto it = all_events.rbegin(); it != all_events.rend(); ++it) {
                if (it->find("ok=false") != std::string::npos
                    || it->find("error=") != std::string::npos
                    || it->find("failed=true") != std::string::npos) {
                    last_failure_event = *it;
                    break;
                }
            }
        }
        if (error_out != nullptr) {
            *error_out = failed
                ? "battle-end workflow failed: " + final_description
                : "battle-end workflow timed out before reaching a terminal workflow state: " + final_description;
            if (!last_failure_event.empty()) {
                *error_out += "; last coordinator error: " + last_failure_event;
            }
        }
        return false;
    }
    std::string verification_summary;
    std::string verification_error;
    if (!VerifyCompletedBattleEndWorkflow(
            db_service,
            workflow_instance_id,
            source_savestate_id,
            &verification_summary,
            &verification_error)) {
        const auto line = "[battle-end-verify] ok=false error=" + verification_error;
        std::cout << line << '\n';
        durable_log.AppendLine(line);
        if (error_out != nullptr) *error_out = verification_error;
        return false;
    }
    const auto verification_line = "[battle-end-verify] ok=true " + verification_summary;
    std::cout << verification_line << '\n';
    durable_log.AppendLine(verification_line);
    return true;
}

} // namespace savor::e2e
