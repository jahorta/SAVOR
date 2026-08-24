#include "WorkflowUnitRealWorkerScenario.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Authoring/IAuthoringDb.h"
#include "Authoring/AuthoringRecipeMaterializer.h"
#include "Common/DbService.h"
#include "Common/Types/UtcTimestamp.h"
#include "DurableLogFile.h"
#include "Execution/ProgramDB/ProductionProgramKindRegistry.h"
#include "Execution/Workflow/WorkflowComposition.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "Execution/Workflow/WorkflowUnitActivationFactory.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "ScenarioAssessment.h"
#include "Execution/CoordinatorRuntime.h"
#include "Utils/Hash.h"
#include "WorkerStartupBarrier.h"

namespace savor::e2e {
namespace {

using savor::db::execution::workflow::WorkflowInstanceState;
using savor::db::execution::workflow::WorkflowStepState;

constexpr std::string_view kNodeKey = "unit_1";
constexpr std::string_view kCorrelation = "savor-e2e.workflow-unit-v1";

bool Fail(std::string message, std::string* error_out) {
    if (error_out != nullptr) *error_out = std::move(message);
    return false;
}

struct LaunchContract {
    savor::db::execution::workflow::WorkflowUnitDefinition unit;
    savor::db::execution::workflow::WorkflowPortDefinition input;
    savor::db::execution::workflow::WorkflowUnitStepTemplate step;
};

struct SourceIdentity {
    std::int64_t workflow_graph_revision_id = 0;
    std::string node_key;
    std::string input_key;
    std::string data_kind;
    std::string ref_kind;
    std::int64_t ref_id = 0;
    std::string source_kind;

    friend bool operator==(const SourceIdentity&, const SourceIdentity&) =
        default;
};

std::optional<LaunchContract> ResolveLaunchContract(
    const CliOptions& options,
    std::string* error_out) {
    if (!options.workflow_unit || options.workflow_unit->empty()) {
        Fail("workflow_unit has no selected unit kind", error_out);
        return std::nullopt;
    }
    const auto registry = savor::db::execution::workflow::
        BuildDefaultWorkflowUnitRegistry();
    const auto* unit = registry.Find(*options.workflow_unit);
    if (unit == nullptr) {
        Fail("workflow_unit is not registered: " + *options.workflow_unit,
             error_out);
        return std::nullopt;
    }
    if (unit->required_inputs.size() != 1 ||
        !unit->required_inputs.front().required) {
        Fail("workflow_unit requires a unit with exactly one required input port",
             error_out);
        return std::nullopt;
    }
    if (unit->step_templates.size() != 1) {
        Fail("workflow_unit requires a unit with exactly one static step template",
             error_out);
        return std::nullopt;
    }
    if (!unit->authored_refs.empty()) {
        Fail("workflow_unit does not support units requiring authored references",
             error_out);
        return std::nullopt;
    }
    if (unit->required_inputs.front().key.empty() ||
        unit->required_inputs.front().data_kind.empty() ||
        unit->step_templates.front().step_kind.empty()) {
        Fail("workflow_unit selected an incomplete static unit contract",
             error_out);
        return std::nullopt;
    }
    if (std::ranges::find(unit->internal_step_kinds,
                          unit->step_templates.front().step_kind) ==
        unit->internal_step_kinds.end()) {
        Fail("workflow_unit static step is absent from its internal-step contract",
             error_out);
        return std::nullopt;
    }
    return LaunchContract{*unit, unit->required_inputs.front(),
                          unit->step_templates.front()};
}

bool SeedWorkflow(
    savor::db::IAuthoringDb* authoring_db,
    savor::db::IExecutionDb* execution_db,
    const LaunchContract& contract,
    const CliOptions& options,
    std::string_view run_identity,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    if (authoring_db == nullptr || execution_db == nullptr ||
        workflow_instance_id_out == nullptr || !options.source_ref_kind ||
        options.source_ref_kind->empty() || !options.source_ref_id ||
        *options.source_ref_id <= 0) {
        return Fail("workflow_unit source binding is incomplete", error_out);
    }

    const auto now = savor::db::types::UtcNow();
    savor::db::SaveWorkflowGraphResult saved{};
    const savor::db::authoring::AuthoringRecipeMaterializer materializer(
        authoring_db);
    if (!materializer.SaveWorkflowGraph({
            .symbol = "workflow_unit",
            .name = "SavorE2E workflow unit " + std::string(run_identity),
            .description = "One exact registered workflow unit launched from an existing durable reference",
            .hidden = true,
            .nodes = {{
                .node_key = std::string(kNodeKey),
                .unit = {contract.unit.unit_kind},
                .display_name = contract.unit.display_name,
            }},
            .external_inputs = {{
                std::string(kNodeKey), {contract.input.key},
            }},
        }, {}, &saved, error_out)) {
        return false;
    }

    const auto registry = savor::db::execution::workflow::
        BuildDefaultWorkflowUnitRegistry();
    std::string activation_error;
    auto activation = savor::db::execution::workflow::
        BuildUnitActivationSpecFromDefinition(
            registry, std::string(kNodeKey), std::string(kNodeKey),
            contract.unit.unit_kind, contract.unit.display_name,
            std::nullopt, std::nullopt, {}, &activation_error);
    if (!activation || activation->steps.size() != 1 ||
        activation->steps.front().step_kind != contract.step.step_kind) {
        return Fail("workflow_unit activation does not match its static contract: " +
                        activation_error,
                    error_out);
    }

    savor::db::execution::workflow::WorkflowCreateInstanceCommand command{};
    command.workflow_kind = "workflow_graph";
    command.root_scope_kind = "manual";
    command.workflow_graph_revision_id = saved.workflow_graph_revision_id;
    command.created_by = "savor-e2e";
    command.created_at_utc = now.time_since_epoch().count();
    command.unit_activations.push_back(std::move(*activation));
    command.input_bindings.push_back({
        .node_key = std::string(kNodeKey),
        .input_key = contract.input.key,
        .data_kind = contract.input.data_kind,
        .ref_kind = *options.source_ref_kind,
        .ref_id = *options.source_ref_id,
        .source_kind = "scenario",
    });
    return execution_db->CreateWorkflowInstance(
        command, workflow_instance_id_out, error_out);
}

std::optional<SourceIdentity> ReadSourceIdentity(
    const savor::db::execution::workflow::WorkflowGraphSnapshot& graph,
    const LaunchContract& contract,
    std::string* error_out) {
    const auto matches = std::ranges::count_if(
        graph.input_bindings, [&](const auto& binding) {
            return binding.node_key == kNodeKey &&
                binding.input_key == contract.input.key;
        });
    if (matches != 1) {
        Fail("workflow_unit source binding is missing or ambiguous", error_out);
        return std::nullopt;
    }
    const auto binding = std::ranges::find_if(
        graph.input_bindings, [&](const auto& value) {
            return value.node_key == kNodeKey &&
                value.input_key == contract.input.key;
        });
    return SourceIdentity{binding->workflow_graph_revision_id,
                          binding->node_key, binding->input_key,
                          binding->data_kind, binding->ref_kind,
                          binding->ref_id, binding->source_kind};
}

bool ValidateStaticGraphAndTerminalEvidence(
    savor::db::core::DBService* db_service,
    const savor::db::execution::workflow::WorkflowGraphSnapshot& graph,
    const LaunchContract& contract,
    const SourceIdentity& source,
    std::string* error_out) {
    if (db_service == nullptr || db_service->AuthoringDb() == nullptr ||
        db_service->ExecutionDb() == nullptr ||
        !graph.instance.workflow_graph_revision_id) {
        return Fail("workflow_unit graph evidence is unavailable", error_out);
    }
    if (graph.instance.root_scope_kind != "manual" ||
        graph.instance.root_scope_id.has_value()) {
        return Fail("workflow_unit graph does not use the canonical manual root scope",
                    error_out);
    }
    const auto authored = db_service->AuthoringDb()->GetWorkflowGraphRevision(
        *graph.instance.workflow_graph_revision_id);
    if (!authored || authored->nodes.size() != 1 ||
        !authored->edges.empty()) {
        return Fail("workflow_unit authored graph is not an exact one-node graph",
                    error_out);
    }
    const auto& node = authored->nodes.front();
    if (node.node_key != kNodeKey ||
        node.unit_kind != contract.unit.unit_kind ||
        node.inputs.size() != 1 ||
        node.inputs.front().input_key != contract.input.key ||
        node.inputs.front().data_kind != contract.input.data_kind ||
        !node.inputs.front().required) {
        return Fail("workflow_unit authored node drifted from the static registry",
                    error_out);
    }

    if (node.possible_outputs.size() != contract.unit.possible_outputs.size()) {
        return Fail("workflow_unit authored output catalog drifted", error_out);
    }
    for (const auto& expected : contract.unit.possible_outputs) {
        const auto matches = std::ranges::count_if(
            node.possible_outputs, [&](const auto& output) {
                return output.output_key == expected.key &&
                    output.data_kind == expected.data_kind;
            });
        if (matches != 1) {
            return Fail("workflow_unit authored output catalog is not exact",
                        error_out);
        }
    }

    const auto activation_count = std::ranges::count_if(
        graph.unit_activations, [&](const auto& activation) {
            return activation.graph_node_key == kNodeKey &&
                activation.unit_kind == contract.unit.unit_kind;
        });
    if (activation_count != 1) {
        return Fail("workflow_unit root activation lineage is missing or ambiguous",
                    error_out);
    }
    const auto activation = std::ranges::find_if(
        graph.unit_activations, [&](const auto& value) {
            return value.graph_node_key == kNodeKey &&
                value.unit_kind == contract.unit.unit_kind;
        });
    const auto root_steps = std::ranges::count_if(
        graph.steps, [&](const auto& step) {
            return step.graph_node_key == kNodeKey &&
                step.step_kind == contract.step.step_kind;
        });
    if (root_steps != 1) {
        return Fail("workflow_unit root step lineage is missing or ambiguous",
                    error_out);
    }
    const auto root_step = std::ranges::find_if(
        graph.steps, [&](const auto& step) {
            return step.graph_node_key == kNodeKey &&
                step.step_kind == contract.step.step_kind;
        });
    if (root_step->workflow_unit_activation_id !=
            std::optional<std::int64_t>(
                activation->workflow_unit_activation_id) ||
        root_step->step_key != kNodeKey ||
        root_step->state != WorkflowStepState::Completed ||
        !root_step->job_set_id || *root_step->job_set_id <= 0 ||
        !root_step->input_ref_kind || root_step->input_ref_kind->empty() ||
        !root_step->input_ref_id || *root_step->input_ref_id <= 0 ||
        root_step->priority != contract.step.priority ||
        root_step->max_attempts != contract.step.max_attempts) {
        return Fail("workflow_unit root step terminal lineage is inconsistent",
                    error_out);
    }
    const auto root_jobs = db_service->ExecutionDb()->ListJobsInJobSet(
        *root_step->job_set_id);
    if (root_jobs.empty()) {
        return Fail("workflow_unit root step has no durable execution jobs",
                    error_out);
    }
    for (const auto& root_job : root_jobs) {
        const auto job = db_service->ExecutionDb()->GetExecutionJob(
            root_job.job_id);
        if (!job || job->job_set_id != *root_step->job_set_id ||
            job->program_ref_kind != *root_step->input_ref_kind ||
            job->program_ref_id != *root_step->input_ref_id) {
            return Fail("workflow_unit materialized domain reference does not match its jobs",
                        error_out);
        }
    }

    const auto current_source = ReadSourceIdentity(graph, contract, error_out);
    if (!current_source || *current_source != source) {
        return Fail("workflow_unit external source identity changed during execution",
                    error_out);
    }

    const auto outputs = db_service->ExecutionDb()->WorkflowQueryService()
        ->ListStepOutputs(graph.instance.workflow_instance_id);
    std::unordered_set<std::string> observed_root_keys;
    for (const auto& output : outputs) {
        if (output.workflow_instance_id !=
                graph.instance.workflow_instance_id ||
            output.workflow_step_id <= 0 || output.output_key.empty() ||
            output.data_kind.empty() || output.ref_kind.empty() ||
            output.ref_id <= 0) {
            return Fail("workflow_unit observed malformed durable output evidence",
                        error_out);
        }
        if (output.workflow_step_id != root_step->workflow_step_id) continue;
        const auto expected = std::ranges::find_if(
            contract.unit.possible_outputs, [&](const auto& value) {
                return value.key == output.output_key &&
                    value.data_kind == output.data_kind;
            });
        if (expected == contract.unit.possible_outputs.end() ||
            !observed_root_keys.insert(output.output_key).second) {
            return Fail("workflow_unit root published an undeclared or duplicate output",
                        error_out);
        }
    }
    if (root_step->output_ref_kind || root_step->output_ref_id) {
        if (!root_step->output_ref_kind || !root_step->output_ref_id ||
            *root_step->output_ref_id <= 0) {
            return Fail("workflow_unit root terminal output reference is partial",
                        error_out);
        }
        const auto exact = std::ranges::count_if(
            outputs, [&](const auto& output) {
                return output.workflow_step_id == root_step->workflow_step_id &&
                    output.ref_kind == *root_step->output_ref_kind &&
                    output.ref_id == *root_step->output_ref_id;
            });
        if (exact != 1) {
            return Fail("workflow_unit root terminal reference lacks one exact named output",
                        error_out);
        }
    }
    return true;
}

} // namespace

bool RunWorkflowUnitRealWorkerScenario(
    const CliOptions& options,
    const ResolvedE2eScenarioEntry& entry,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out) {
    if (entry.source !=
            E2eScenarioEntrySource::ExistingWorkspaceReference ||
        db_service == nullptr || !db_service->IsRunning() ||
        db_service->ExecutionDb() == nullptr ||
        db_service->StateDb() == nullptr ||
        db_service->AnalysisDb() == nullptr ||
        db_service->AuthoringDb() == nullptr) {
        return Fail("workflow_unit requires a running complete existing workspace",
                    error_out);
    }
    const auto contract = ResolveLaunchContract(options, error_out);
    if (!contract) return false;

    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::is_regular_file(worker_exe)) {
        return Fail("SavorWorker.exe was not found next to SavorE2E: " +
                        worker_exe.string(),
                    error_out);
    }

    std::string error;
    const auto workspace_root = *options.workspace_root;
    auto registry_config = savor::db::execution::programdb::
        MakeProductionProgramKindRegistryConfig(
            workspace_root / "workflow-runtime");
    savor::db::execution::programdb::ProgramKindRegistry program_registry;
    if (!savor::db::execution::programdb::BuildProductionProgramKindRegistry({
            .execution_db = db_service->ExecutionDb(),
            .state_db = db_service->StateDb(),
            .analysis_db = db_service->AnalysisDb(),
            .authoring_db = db_service->AuthoringDb(),
        }, std::move(registry_config), &program_registry, &error)) {
        return Fail("failed building production workflow_unit registry: " +
                        error,
                    error_out);
    }
    const auto* production_descriptor =
        program_registry.FindForStepKind(contract->step.step_kind);
    if (production_descriptor == nullptr ||
        !program_registry.HasRequiredAdaptersForStepKind(
            contract->step.step_kind) ||
        !production_descriptor->supports_workflow_orchestration) {
        return Fail("workflow_unit static step has no complete production orchestration descriptor: " +
                        contract->step.step_kind,
                    error_out);
    }

    std::string iso_sha256;
    try {
        iso_sha256 = hash::sha256_of_file(options.iso_path.string());
    } catch (const std::exception& exception) {
        return Fail("failed hashing workflow_unit ISO: " +
                        std::string(exception.what()),
                    error_out);
    }
    savor::runtime::ArtifactCompatibilityToken compatibility{
        .game_id = std::string(savor::runtime::program::capabilities::
            kSupportedGameId),
        .iso_sha256 = std::move(iso_sha256),
        .emulator_build = "dolphin-2506a",
        .runtime_revision = "worker-runtime-slice4",
    };
    savor::runner::parallel::savordb::WorkerCoordinatorConfig worker_config{
        .desired_workers = static_cast<std::size_t>(
            std::max<std::int64_t>(1, options.worker_count)),
        .controller_sleep_ms = static_cast<std::uint32_t>(
            std::max<std::int64_t>(1, options.poll_ms)),
        .worker_start_timeout_ms = 60'000,
        .worker_exe_path = worker_exe.string(),
        .iso_path = options.iso_path.string(),
        .dolphin_base_dir = options.dolphin_base_dir.string(),
        .worker_dir_root = options.worker_dir_root.value_or(
            workspace_root / ".workers").string(),
        .worker_binary_runtime_root =
            (workspace_root / "worker-runtime").string(),
        .worker_mode = options.visual_worker
            ? savor::runtime::WorkerMode::Visual
            : savor::runtime::WorkerMode::Headless,
        .runtime_artifact_root =
            (workspace_root / "runtime-artifacts").string(),
    };

    DurableLogFile durable_log;
    if (!durable_log.Open(options, options.scenario, error_out)) return false;
    std::mutex output_mutex;
    const auto event_sink = [&](const std::string& line) {
        durable_log.AppendLine(line);
        std::lock_guard lock(output_mutex);
        std::cout << line << '\n';
    };
    savor::runner::parallel::savordb::CoordinatorRuntime coordinators;
    ArmInitialWorkerPoolBarrier(
        options.wait_for_workers_ready,
        [&](bool paused) { coordinators.SetExecutionPaused(paused); },
        event_sink);
    savor::runner::parallel::savordb::CoordinatorRuntimeConfig
        coordinator_config{
            .worker = std::move(worker_config),
            .poll_interval = std::chrono::milliseconds(
                std::max<std::int64_t>(1, options.poll_ms)),
            .state_compatibility = std::move(compatibility),
            .initially_paused = options.wait_for_workers_ready,
            .object_store_root = workspace_root / "object_store",
            .event_line_callback = event_sink,
        };
    if (!coordinators.Start(
            db_service->ExecutionDb(), db_service->AuthoringDb(),
            &program_registry, std::move(coordinator_config), &error)) {
        return Fail("workflow_unit coordinator startup failed: " + error,
                    error_out);
    }
    const auto barrier = WaitForInitialWorkerPool(
        options.wait_for_workers_ready,
        std::chrono::milliseconds(
            std::max<std::int64_t>(1, options.poll_ms)),
        [&]() { return coordinators.SnapshotFleetStartup(); },
        [&](bool paused) { coordinators.SetExecutionPaused(paused); },
        event_sink);
    if (!barrier.satisfied) {
        std::string stop_error;
        (void)coordinators.Stop(&stop_error);
        return Fail(barrier.diagnostic +
                (stop_error.empty() ? "" : "; shutdown: " + stop_error),
            error_out);
    }

    std::int64_t workflow_instance_id = 0;
    if (!SeedWorkflow(db_service->AuthoringDb(), db_service->ExecutionDb(),
                      *contract, options, entry.run_identity,
                      &workflow_instance_id, &error)) {
        std::string stop_error;
        (void)coordinators.Stop(&stop_error);
        return Fail("failed seeding workflow_unit graph: " + error +
                (stop_error.empty() ? "" : "; shutdown: " + stop_error),
            error_out);
    }
    auto initial_graph = db_service->ExecutionDb()->WorkflowQueryService()
        ->GetWorkflowGraph(workflow_instance_id);
    const auto initial_source = initial_graph
        ? ReadSourceIdentity(*initial_graph, *contract, &error)
        : std::nullopt;
    if (!initial_graph || !initial_source ||
        !initial_graph->instance.workflow_graph_revision_id ||
        initial_source->workflow_graph_revision_id !=
            *initial_graph->instance.workflow_graph_revision_id ||
        initial_source->node_key != kNodeKey ||
        initial_source->input_key != contract->input.key ||
        initial_source->data_kind != contract->input.data_kind ||
        initial_source->ref_kind != *options.source_ref_kind ||
        initial_source->ref_id != *options.source_ref_id ||
        initial_source->source_kind != "scenario") {
        std::string cancel_error;
        (void)db_service->ExecutionDb()->WorkflowCommandService()
            ->CancelWorkflowInstance({
                .workflow_instance_id = workflow_instance_id,
                .requested_by = "savor-e2e-workflow-unit-preflight",
            }, &cancel_error);
        std::string stop_error;
        (void)coordinators.Stop(&stop_error);
        return Fail(error.empty()
                ? "workflow_unit persisted a different initial source identity"
                : error,
            error_out);
    }
    event_sink("[workflow-unit-source] unit=" + contract->unit.unit_kind +
        " data_kind=" + initial_source->data_kind + " ref=" +
        initial_source->ref_kind + ':' +
        std::to_string(initial_source->ref_id) + " workflow=" +
        std::to_string(workflow_instance_id));

    std::optional<savor::db::execution::workflow::WorkflowGraphSnapshot>
        final_graph;
    for (;;) {
        final_graph = db_service->ExecutionDb()->WorkflowQueryService()
            ->GetWorkflowGraph(workflow_instance_id);
        if (final_graph &&
            (final_graph->instance.state == WorkflowInstanceState::Completed ||
             final_graph->instance.state == WorkflowInstanceState::Failed ||
             final_graph->instance.state == WorkflowInstanceState::Canceled)) {
            break;
        }
        const auto telemetry = coordinators.SnapshotTelemetry();
        if (telemetry.execution.invariant_paused) {
            error = telemetry.execution.last_error.empty()
                ? "workflow_unit JobExecutionCoordinator entered an invariant pause"
                : telemetry.execution.last_error;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(
            std::max<std::int64_t>(1, options.poll_ms)));
    }

    const auto final_telemetry = coordinators.SnapshotTelemetry();
    const auto final_ready_workers = coordinators.SnapshotReadyWorkers();
    const auto final_warnings = coordinators.SnapshotExecutionWarnings();
    std::string shutdown_error;
    const bool clean_shutdown = coordinators.Stop(&shutdown_error);

    ScenarioAssessment assessment;
    assessment.Require(clean_shutdown,
        "workflow_unit coordinator shutdown failed: " + shutdown_error);
    assessment.Require(error.empty(),
        error.empty() ? "workflow_unit coordinator failed" : error);
    assessment.Require(final_graph.has_value(),
        "workflow_unit workflow snapshot is unavailable");
    const bool workflow_completed = final_graph &&
        final_graph->instance.state == WorkflowInstanceState::Completed;
    assessment.Require(workflow_completed,
        "workflow_unit workflow did not reach COMPLETED");
    std::string invariant_error;
    if (workflow_completed) {
        assessment.Require(
            ValidateStaticGraphAndTerminalEvidence(
                db_service, *final_graph, *contract, *initial_source,
                &invariant_error),
            invariant_error.empty()
                ? "workflow_unit durable contract assessment failed"
                : invariant_error);
    }
    std::vector<savor::db::execution::workflow::WorkflowGraphSnapshot>
        workflows;
    if (final_graph) workflows.push_back(*final_graph);
    AssessCommonScenarioExecution(
        db_service->ExecutionDb(), workflows, final_telemetry,
        final_ready_workers, &assessment);
    for (const auto& warning : final_warnings) {
        assessment.Warn("coordinator warning " +
            std::to_string(warning.sequence) + ": " + warning.message +
            (warning.detail.empty() ? "" : " (" + warning.detail + ")"));
    }
    ReportCommonScenarioTrajectory(
        db_service, workflows, "workflow_unit", event_sink, &assessment);

    const auto reloaded_graph = db_service->ExecutionDb()
        ->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    if (reloaded_graph) {
        std::string recheck_error;
        const auto rechecked = ReadSourceIdentity(
            *reloaded_graph, *contract, &recheck_error);
        assessment.Require(rechecked && *rechecked == *initial_source,
            recheck_error.empty()
                ? "workflow_unit source identity re-read changed"
                : recheck_error);
        if (rechecked) {
            event_sink("[workflow-unit-source-recheck] unit=" +
                contract->unit.unit_kind + " data_kind=" +
                rechecked->data_kind + " ref=" + rechecked->ref_kind + ':' +
                std::to_string(rechecked->ref_id) + " unchanged=" +
                (*rechecked == *initial_source ? "1" : "0"));
        }
    } else {
        assessment.Require(false,
            "workflow_unit source identity could not be re-read after execution");
    }
    EmitScenarioAssessment("workflow_unit", assessment, event_sink);
    if (!assessment.Passed()) {
        return Fail(assessment.FailureSummary("workflow_unit"), error_out);
    }

    const auto outputs = db_service->ExecutionDb()->WorkflowQueryService()
        ->ListStepOutputs(workflow_instance_id);
    event_sink("[workflow-unit-summary] workflow=" +
        std::to_string(workflow_instance_id) + " unit=" +
        contract->unit.unit_kind + " root_step=" + contract->step.step_kind +
        " observed_outputs=" + std::to_string(outputs.size()));
    return true;
}

} // namespace savor::e2e
