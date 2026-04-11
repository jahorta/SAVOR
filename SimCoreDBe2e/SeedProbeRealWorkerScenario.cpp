#include "SeedProbeRealWorkerScenario.h"

#include <chrono>
#include <cstdint>
#include <thread>

#include "Execution/Workflow/WorkflowModeProvider.h"
#include "Runner/Parallel/SimCoreDB/DBWorkflowWorkerCoordinator.h"

#include "Cli.h"
#include "DbSetup.h"

namespace simcore::e2e {

using simcore::db::execution::workflow::WorkflowExecutionMode;
using simcore::db::execution::workflow::StaticWorkflowModeProvider;
using simcore::runner::parallel::simcoredb::CoordinatorIntegrationConfig;
using simcore::runner::parallel::simcoredb::DBWorkflowWorkerCoordinator;
using simcore::runner::parallel::simcoredb::DBWorkflowWorkerCoordinatorConfig;
using simcore::runner::parallel::simcoredb::ScheduledJobSet;
using simcore::runner::parallel::simcoredb::WorkflowReadyStep;

namespace {

constexpr std::int64_t kSeedProbeAverageUniqueCountEstimate = 25;

std::int64_t ComputeSeedProbeTimeoutMs(std::int64_t baseline_timeout_ms) {
    const std::int64_t grid_probe_count = static_cast<std::int64_t>(kSeedProbeSamplesPerAxis) * 2 * 3;
    const std::int64_t multiplier = 1 + grid_probe_count + kSeedProbeAverageUniqueCountEstimate;
    return baseline_timeout_ms * multiplier;
}

} // namespace

bool RunSeedProbeRealWorkerSmoke(
    const CliOptions& options,
    const char* argv0,
    simcore::db::core::DBService* db_service,
    std::string* error_out) {
    if (db_service == nullptr) {
        if (error_out) *error_out = "db service is required";
        return false;
    }
    if (!db_service->IsRunning()) {
        if (error_out) *error_out = "db service must be started in main before running scenarios";
        return false;
    }

    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::exists(worker_exe)) {
        if (error_out) *error_out = "SimCoreWorker.exe was not found next to SimCoreDBe2e: " + worker_exe.string();
        return false;
    }
    std::string err;

    std::int64_t savestate_id = 0;
    if (!SeedStateSavestate(db_service->StateDb(), options.savestate_file, &savestate_id, &err)) {
        if (error_out) *error_out = "failed seeding StateDB savestate: " + err;
        return false;
    }

    std::int64_t seed_probe_spec_id = 0;
    if (!SeedAuthoringSpec(db_service->AuthoringDb(), &seed_probe_spec_id, &err)) {
        if (error_out) *error_out = "failed seeding AuthoringDB seedprobe spec: " + err;
        return false;
    }

    auto* execution_db = dynamic_cast<simcore::db::execution::workflow::SqliteExecutionDb*>(db_service->ExecutionDb());
    if (execution_db == nullptr) {
        if (error_out) *error_out = "DBService execution db is not sqlite-backed";
        return false;
    }

    std::int64_t workflow_instance_id = 0;
    if (!SeedExecutionWorkflow(
            db_service->AnalysisDb(),
            execution_db,
            savestate_id,
            seed_probe_spec_id,
            &workflow_instance_id,
            &err)) {
        if (error_out) *error_out = "failed seeding execution workflow rows: " + err;
        return false;
    }

    StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::Workflow, .source = "SimCoreDBe2e" });

    std::int64_t next_job_set_id = 30000;
    auto schedule = [&](const WorkflowReadyStep& step) {
        ++next_job_set_id;
        return ScheduledJobSet{ .job_set_id = next_job_set_id, .workflow_step_id = step.workflow_step_id };
    };

    DBWorkflowWorkerCoordinator coordinator(
        execution_db,
        &mode_provider,
        DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 1,
            .controller_sleep_ms = static_cast<uint32_t>(options.poll_ms),
            .worker_exe_path = worker_exe.string(),
            .iso_path = options.iso_path.string(),
            .dolphin_base_dir = options.dolphin_base_dir.string(),
            .worker_dir_root = options.worker_dir_root.value_or(
                std::filesystem::temp_directory_path() / "simcoredbe2e-workers").string(),
        },
        CoordinatorIntegrationConfig{},
        schedule);

    coordinator.Start();
    auto* ui_read_db = db_service->UiReadDb();
    if (ui_read_db == nullptr) {
        coordinator.Stop();
        if (error_out) *error_out = "DBService ui read db unavailable";
        return false;
    }

    const auto timeout_ms = ComputeSeedProbeTimeoutMs(options.timeout_ms);
    const auto started = std::chrono::steady_clock::now();
    std::size_t poll_count = 0;
    while (std::chrono::steady_clock::now() - started < std::chrono::milliseconds(timeout_ms)) {
        (void)ui_read_db->ListProjectionSubscriptions("Execution", "exec_outbox_message");
        ++poll_count;

        const auto telemetry = coordinator.SnapshotTelemetry();
        const auto graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
        if (graph.has_value()) {
            using simcore::db::execution::workflow::WorkflowInstanceState;
            if (graph->instance.state == WorkflowInstanceState::Completed) {
                break;
            }
            if (graph->instance.state == WorkflowInstanceState::Failed
                || graph->instance.state == WorkflowInstanceState::Canceled) {
                if (error_out) *error_out = "workflow did not complete successfully";
                coordinator.Stop();
                return false;
            }
        }
        if (telemetry.ready_scan_count == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
            continue;
        }
        if (graph.has_value() && graph->instance.state == simcore::db::execution::workflow::WorkflowInstanceState::Completed) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
    }

    coordinator.Stop();

    if (poll_count == 0) {
        if (error_out) *error_out = "UiReadDB polling loop did not execute";
        return false;
    }

    const auto final_graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    if (!final_graph.has_value()
        || final_graph->instance.state != simcore::db::execution::workflow::WorkflowInstanceState::Completed) {
        if (error_out) *error_out = "workflow did not reach COMPLETED state before timeout";
        return false;
    }

    return true;
}

} // namespace simcore::e2e
