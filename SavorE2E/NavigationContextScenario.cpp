#include "NavigationContextScenario.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Common/DbService.h"
#include "Common/Types/UtcTimestamp.h"
#include "CoordinatorProgress.h"
#include "Core/Memory/Soa/Navigation/NavigationContextCodec.h"
#include "DbSetup.h"
#include "DurableLogFile.h"
#include "Execution/DBWorkflowCoordinatorFactory.h"
#include "Execution/DBWorkflowWorkerCoordinator.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/ProgramDB/ProductionProgramKindRegistry.h"
#include "Execution/Workflow/WorkflowComposition.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Execution/Workflow/WorkflowUnitActivationFactory.h"
#include "Phases/Programs/NavigationContext/NavigationContextResult.h"
#include "Runner/Breakpoints/BpRegistry.h"
#include "Runner/IPC/Wire.h"
#include "Runner/Script/CtxRegistry.h"
#include "Utils/Hash.h"
#include "WorkerCoordinatorPerf.h"

namespace savor::e2e {
namespace {

constexpr const char* kUnitKind = "navigation.context_probe";
constexpr const char* kStepKind = "navigation.context_probe";
constexpr const char* kNodeKey = "navigation_context_1";
constexpr const char* kOutputKey = "navigation_context";
constexpr const char* kOutputDataKind = "state_artifact.navigation_context_id";
constexpr const char* kOutputRefKind = "state_artifact";
constexpr const char* kDerivationMethod = "navigation_context_capture";
constexpr const char* kDerivationContextKind = "state_artifact";
constexpr std::uint32_t kCapturePc = 0x80111770u;
std::filesystem::path WorkspaceRoot(const CliOptions& options)
{
    return options.workspace_root.value_or(
        std::filesystem::temp_directory_path() / "savor-e2e-default");
}

const char* WorkflowStateName(
    savor::db::execution::workflow::WorkflowInstanceState state)
{
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

std::string FormatHex(std::uint32_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << value << std::dec;
    return out.str();
}

bool ReadBinaryFile(
    const std::filesystem::path& path,
    std::string* bytes_out,
    std::string* error_out)
{
    if (bytes_out == nullptr) {
        if (error_out != nullptr) *error_out = "navigation context byte destination is null";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error_out != nullptr) {
            *error_out = "failed opening materialized navigation context: " + path.string();
        }
        return false;
    }
    bytes_out->assign(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        if (error_out != nullptr) {
            *error_out = "failed reading materialized navigation context: " + path.string();
        }
        return false;
    }
    return true;
}

bool CreateNavigationContextWorkflow(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    std::int64_t source_savestate_id,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out)
{
    if (authoring_db == nullptr || execution_db == nullptr
        || workflow_instance_id_out == nullptr || source_savestate_id <= 0) {
        if (error_out != nullptr) *error_out = "invalid navigation context workflow request";
        return false;
    }

    savor::db::SaveWorkflowGraphResult saved{};
    if (!authoring_db->SaveWorkflowGraph(
            {
                .name = "SavorE2E Navigation Context",
                .description = "Coordinator-routed standalone navigation context capture.",
                .hidden = true,
                .graph_version = 1,
                .graph_hash = "savor-e2e.workflow_graph.navigation_context.v1",
                .nodes = {{
                    .node_key = kNodeKey,
                    .unit_kind = kUnitKind,
                    .display_name = "Navigation Context",
                    .inputs = {{
                        .input_key = "entry_savestate",
                        .data_kind = "state.savestate_id",
                        .display_name = "Entry savestate",
                    }},
                    .possible_outputs = {{
                        .output_key = kOutputKey,
                        .data_kind = kOutputDataKind,
                        .display_name = "Navigation context",
                    }},
                }},
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = "savor-e2e.navigation-context",
                .causation_id = "savestate-" + std::to_string(source_savestate_id),
            },
            &saved,
            error_out)) {
        return false;
    }

    const auto units =
        savor::db::execution::workflow::BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto activation =
        savor::db::execution::workflow::BuildUnitActivationSpecFromDefinition(
            units,
            kNodeKey,
            kNodeKey,
            kUnitKind,
            "Navigation Context",
            std::nullopt,
            std::nullopt,
            {},
            &activation_error);
    if (!activation.has_value()) {
        if (error_out != nullptr) {
            *error_out = "failed building Navigation Context activation: "
                + activation_error;
        }
        return false;
    }

    savor::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.root_scope_id = source_savestate_id;
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "savor-e2e";
    command.created_at_utc =
        savor::db::types::UtcNow().time_since_epoch().count();
    command.unit_activations.push_back(std::move(*activation));
    command.input_bindings.push_back({
        .node_key = kNodeKey,
        .input_key = "entry_savestate",
        .data_kind = "state.savestate_id",
        .ref_kind = "state.savestate",
        .ref_id = source_savestate_id,
        .source_kind = "external",
    });
    return execution_db->CreateWorkflowInstance(
        command,
        workflow_instance_id_out,
        error_out);
}

struct WorkerObservation {
    bool seen = false;
    std::uint64_t job_id = 0;
    bool worker_ok = false;
    std::uint32_t entry_pc = 0;
    std::uint32_t hit_pc = 0;
    std::uint32_t hit_key = 0;
    std::uint32_t run_outcome = 0;
    std::uint32_t vi_delta = 0;
    std::uint32_t phase_outcome = 0;
    std::uint32_t failure = 0;
    std::string diagnostic;
};

WorkerObservation Observe(const savor::PRResult& result)
{
    WorkerObservation out{};
    out.seen = true;
    out.job_id = result.job_id;
    out.worker_ok = result.ps.ok;
    result.ps.ctx.get(
        savor::context::key::navigation::ENTRY_PC,
        out.entry_pc);
    result.ps.ctx.get(savor::context::key::core::RUN_HIT_PC, out.hit_pc);
    result.ps.ctx.get(savor::context::key::core::RUN_HIT_BP_KEY, out.hit_key);
    result.ps.ctx.get(
        savor::context::key::core::DW_RUN_OUTCOME_CODE,
        out.run_outcome);
    result.ps.ctx.get(savor::context::key::core::VI_DELTA, out.vi_delta);
    result.ps.ctx.get(
        savor::context::key::navigation::OUTCOME,
        out.phase_outcome);
    result.ps.ctx.get(
        savor::context::key::navigation::FAILURE,
        out.failure);
    result.ps.ctx.get(
        savor::context::key::navigation::DIAGNOSTIC,
        out.diagnostic);
    return out;
}

bool VerifyCompletedWorkflow(
    const CliOptions& options,
    savor::db::core::DBService* db_service,
    std::int64_t workflow_instance_id,
    std::int64_t source_savestate_id,
    const WorkerObservation& observation,
    std::string* summary_out,
    std::string* error_out)
{
    using savor::db::execution::workflow::WorkflowInstanceState;
    if (db_service == nullptr || db_service->ExecutionDb() == nullptr
        || db_service->StateDb() == nullptr
        || db_service->ExecutionDb()->WorkflowQueryService() == nullptr) {
        if (error_out != nullptr) *error_out = "Navigation Context verification databases are unavailable";
        return false;
    }

    const auto graph =
        db_service->ExecutionDb()->WorkflowQueryService()->GetWorkflowGraph(
            workflow_instance_id);
    const auto outputs =
        db_service->ExecutionDb()->WorkflowQueryService()->ListStepOutputs(
            workflow_instance_id);
    if (!graph.has_value()
        || graph->instance.state != WorkflowInstanceState::Completed
        || graph->unit_activations.size() != 1
        || graph->steps.size() != 1
        || outputs.size() != 1) {
        if (error_out != nullptr) {
            *error_out = "completed Navigation Context workflow is not an isolated one-step capture";
        }
        return false;
    }

    const auto& output = outputs.front();
    if (output.graph_node_key != kNodeKey
        || output.output_key != kOutputKey
        || output.data_kind != kOutputDataKind
        || output.ref_kind != kOutputRefKind
        || output.ref_id <= 0) {
        if (error_out != nullptr) *error_out = "Navigation Context workflow output is invalid";
        return false;
    }

    const auto derivations =
        db_service->StateDb()->ListSavestateDerivationsBySourceContext(
            kDerivationContextKind,
            output.ref_id);
    const auto matching_derivation = std::max_element(
        derivations.begin(),
        derivations.end(),
        [&](const auto& left, const auto& right) {
            const auto left_matches =
                left.from_savestate_id == source_savestate_id
                && left.method_kind == kDerivationMethod
                && left.source_context_kind == kDerivationContextKind
                && left.source_context_id == output.ref_id;
            const auto right_matches =
                right.from_savestate_id == source_savestate_id
                && right.method_kind == kDerivationMethod
                && right.source_context_kind == kDerivationContextKind
                && right.source_context_id == output.ref_id;
            if (left_matches != right_matches) return !left_matches;
            return left.derivation_id < right.derivation_id;
        });
    if (matching_derivation == derivations.end()
        || matching_derivation->from_savestate_id != source_savestate_id
        || matching_derivation->method_kind != kDerivationMethod
        || matching_derivation->source_context_kind
            != kDerivationContextKind
        || matching_derivation->source_context_id != output.ref_id) {
        if (error_out != nullptr) *error_out = "Navigation Context savestate derivation for this source is missing";
        return false;
    }

    const auto output_state =
        db_service->StateDb()->GetSavestate(
            matching_derivation->to_savestate_id);
    if (!output_state.has_value() || !output_state->is_complete
        || output_state->savestate_type != "NAVIGATION_CONTEXT") {
        if (error_out != nullptr) *error_out = "Navigation Context output savestate is absent or incomplete";
        return false;
    }

    const auto capture_dir =
        WorkspaceRoot(options) / "navigation-context-verification";
    std::error_code ec;
    std::filesystem::create_directories(capture_dir, ec);
    if (ec) {
        if (error_out != nullptr) {
            *error_out = "failed creating Navigation Context verification directory: "
                + ec.message();
        }
        return false;
    }
    const auto context_path =
        capture_dir / ("navigation-context-" + std::to_string(output.ref_id)
            + std::string(soa::navigation::ctx::codec::ext));
    std::string materialize_error;
    if (!db_service->StateDb()->MaterializeArtifactToPath(
            output.ref_id,
            context_path.string(),
            &materialize_error).has_value()) {
        if (error_out != nullptr) {
            *error_out = "failed materializing Navigation Context artifact: "
                + materialize_error;
        }
        return false;
    }

    std::string bytes;
    if (!ReadBinaryFile(context_path, &bytes, error_out)) {
        return false;
    }
    soa::navigation::ctx::NavigationContext context{};
    if (!soa::navigation::ctx::codec::decode(bytes, context)
        || context.capture_pc != kCapturePc
        || context.player_worksheet == 0
        || context.motion_state != 1) {
        if (error_out != nullptr) *error_out = "materialized NCTX artifact failed capture-contract validation";
        return false;
    }

    if (!observation.seen
        || !observation.worker_ok
        || observation.hit_pc != kCapturePc
        || observation.hit_key
            != bp::navigation::NavigationContextInitialPlayerInputReady
        || observation.phase_outcome
            != static_cast<std::uint32_t>(
                phase::navigation::ctx::Outcome::Completed)
        || observation.failure
            != static_cast<std::uint32_t>(
                phase::navigation::ctx::FailureCode::None)) {
        if (error_out != nullptr) {
            *error_out = "worker result did not preserve the successful Navigation Context capture diagnostics";
        }
        return false;
    }

    if (summary_out != nullptr) {
        *summary_out =
            "job_id=" + std::to_string(observation.job_id)
            + " entry_pc=" + FormatHex(observation.entry_pc)
            + " capture_pc=" + FormatHex(context.capture_pc)
            + " vi_delta=" + std::to_string(observation.vi_delta)
            + " has_ground="
                + std::to_string(context.has_ground ? 1 : 0)
            + " ground_tbl_id="
                + std::to_string(context.ground_tbl_id)
            + " context_artifact_id=" + std::to_string(output.ref_id)
            + " context_sha256="
                + hash::sha256_of_file(context_path.string())
            + " output_savestate_id="
                + std::to_string(output_state->savestate_id)
            + " output_savestate_sha256=" + output_state->artifact_sha256
            + " successor=none";
    }
    if (error_out != nullptr) error_out->clear();
    return true;
}

} // namespace

bool RunNavigationContextScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out)
{
    if (db_service == nullptr || !db_service->IsRunning()
        || db_service->ExecutionDb() == nullptr
        || db_service->StateDb() == nullptr
        || db_service->AuthoringDb() == nullptr) {
        if (error_out != nullptr) *error_out = "Navigation Context requires a running DBService";
        return false;
    }
    if (!options.source_savestate_id.has_value()
        || *options.source_savestate_id <= 0) {
        if (error_out != nullptr) {
            *error_out = "navigation_context requires --source-savestate-id with a positive StateDB savestate id";
        }
        return false;
    }
    const auto source_savestate_id = *options.source_savestate_id;
    const auto source_state =
        db_service->StateDb()->GetSavestate(source_savestate_id);
    if (!source_state.has_value() || !source_state->is_complete) {
        if (error_out != nullptr) {
            *error_out = "navigation_context source savestate id "
                + std::to_string(source_savestate_id)
                + " was not found as a complete StateDB savestate";
        }
        return false;
    }

    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::exists(worker_exe)) {
        if (error_out != nullptr) {
            *error_out = "SavorWorker.exe was not found next to SavorE2E: "
                + worker_exe.string();
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
    registry_config.navigation_context.working_dir_root =
        runtime_root / "navigation-context";
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
    const auto* descriptor = registry.FindForStepKind(kStepKind);
    if (descriptor == nullptr
        || descriptor->job_persistence != nullptr
        || descriptor->graph_job_persistence == nullptr
        || !registry.HasRequiredAdaptersForStepKind(kStepKind)) {
        if (error_out != nullptr) {
            *error_out = "Navigation Context descriptor is not registered as coordinator-only";
        }
        return false;
    }

    std::int64_t workflow_instance_id = 0;
    if (!CreateNavigationContextWorkflow(
            db_service->AuthoringDb(),
            db_service->ExecutionDb(),
            source_savestate_id,
            &workflow_instance_id,
            &error)) {
        if (error_out != nullptr) {
            *error_out = "failed creating Navigation Context workflow: " + error;
        }
        return false;
    }

    const auto setup_line =
        "[navigation-context-setup] source_savestate_id="
        + std::to_string(source_savestate_id)
        + " source_sha256=" + source_state->artifact_sha256
        + " workflow_instance_id=" + std::to_string(workflow_instance_id);
    std::cout << setup_line << '\n';
    durable_log.AppendLine(setup_line);

    auto coordinator =
        savor::runner::parallel::savordb::BuildDbBackedWorkflowCoordinator(
            db_service->ExecutionDb(),
            db_service->StateDb(),
            savor::runner::parallel::savordb::
                DBWorkflowWorkerCoordinatorConfig{
                    .desired_workers =
                        static_cast<std::size_t>(options.worker_count),
                    .controller_sleep_ms =
                        static_cast<std::uint32_t>(options.poll_ms),
                    .worker_exe_path = worker_exe.string(),
                    .iso_path = options.iso_path.string(),
                    .dolphin_base_dir =
                        options.dolphin_base_dir.string(),
                    .worker_dir_root =
                        options.worker_dir_root.value_or(
                            WorkspaceRoot(options) / ".workers").string(),
                    .visual_workers = options.visual_worker,
                    .auto_resume_visual_workers = false,
                    .visual_screenshot_dir =
                        options.visual_screenshot_dir.value_or(
                            WorkspaceRoot(options)
                                / "visual-screenshots").string(),
                },
            savor::runner::parallel::savordb::
                CoordinatorIntegrationConfig{
                    .strict_smoke_terminal_on_failure = true,
                },
            &registry);

    std::mutex observation_mutex;
    WorkerObservation observation{};
    coordinator.SetResultCallback([&](const savor::PRResult& result) {
        const auto observed = Observe(result);
        {
            std::lock_guard<std::mutex> lock(observation_mutex);
            observation = observed;
        }
        const auto line =
            "[navigation-context-worker-result] job="
            + std::to_string(result.job_id)
            + " ok=" + (result.ps.ok ? std::string("true") : "false")
            + " entry_pc=" + FormatHex(observed.entry_pc)
            + " hit_pc=" + FormatHex(observed.hit_pc)
            + " vi_delta=" + std::to_string(observed.vi_delta)
            + " failure=" + std::to_string(observed.failure)
            + " diagnostic=\"" + observed.diagnostic + "\"";
        std::cout << line << '\n';
        durable_log.AppendLine(line);
    });
    coordinator.SetResultMapEventCallback([&](const std::string& line) {
        std::cout << line << '\n';
        durable_log.AppendLine(line);
    });

    ScopedWorkflowCoordinatorService workflow_coordinator;
    if (!workflow_coordinator.Start(
            db_service->ExecutionDb(),
            db_service->AuthoringDb(),
            &registry,
            options,
            &error,
            [&](const std::string& line) {
                std::cout << line << '\n';
                durable_log.AppendLine(line);
            },
            true,
            coordinator.ItemCreditSource())) {
        if (error_out != nullptr) *error_out = error;
        return false;
    }

    coordinator.Start();
    std::optional<
        savor::db::execution::workflow::WorkflowGraphSnapshot> graph;
    bool completed = false;
    bool failed = false;
    std::size_t poll_count = 0;
    while (true) {
        ++poll_count;
        graph =
            db_service->ExecutionDb()->WorkflowQueryService()
                ->GetWorkflowGraph(workflow_instance_id);
        const auto telemetry = coordinator.SnapshotTelemetry();
        const auto workers = coordinator.SnapshotWorkers();
        RecordWorkerCoordinatorPerfSample(options, telemetry, workers);
        if (poll_count == 1 || poll_count % 10 == 0) {
            const auto state = graph.has_value()
                ? WorkflowStateName(graph->instance.state)
                : "UNAVAILABLE";
            std::cout << "[navigation-context] workflow=" << state << '\n';
        }
        if (graph.has_value()) {
            using savor::db::execution::workflow::WorkflowInstanceState;
            if (graph->instance.state == WorkflowInstanceState::Completed) {
                completed = true;
                break;
            }
            if (graph->instance.state == WorkflowInstanceState::Failed
                || graph->instance.state
                    == WorkflowInstanceState::Canceled) {
                failed = true;
                break;
            }
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(
                std::max<std::int64_t>(1, options.poll_ms)));
    }

    coordinator.Stop();
    workflow_coordinator.Stop();
    if (!completed) {
        if (error_out != nullptr) {
            *error_out = failed
                ? "Navigation Context workflow failed"
                : "Navigation Context workflow stopped before completion";
        }
        return false;
    }

    WorkerObservation observation_snapshot{};
    {
        std::lock_guard<std::mutex> lock(observation_mutex);
        observation_snapshot = observation;
    }
    std::string summary;
    if (!VerifyCompletedWorkflow(
            options,
            db_service,
            workflow_instance_id,
            source_savestate_id,
            observation_snapshot,
            &summary,
            &error)) {
        const auto line =
            "[navigation-context-verify] ok=false error=" + error;
        std::cout << line << '\n';
        durable_log.AppendLine(line);
        if (error_out != nullptr) *error_out = error;
        return false;
    }
    const auto line =
        "[navigation-context-verify] ok=true " + summary;
    std::cout << line << '\n';
    durable_log.AppendLine(line);
    if (error_out != nullptr) error_out->clear();
    return true;
}

} // namespace savor::e2e
