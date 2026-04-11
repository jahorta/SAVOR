#include "SeedProbeRealWorkerScenario.h"

#include <chrono>
#include <thread>

#include "Common/DbService.h"
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

bool RunSeedProbeRealWorkerSmoke(const CliOptions& options, const char* argv0, std::string* error_out) {
    using namespace simcore::db::core;
    using namespace simcore::db::migrations;

    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::exists(worker_exe)) {
        if (error_out) *error_out = "SimCoreWorker.exe was not found next to SimCoreDBe2e: " + worker_exe.string();
        return false;
    }

    const auto migration_root = ResolveMigrationRoot(options.migration_root);
    const auto db_paths = BuildDbPaths(options);
    DBService service(
        db_paths,
        MigrationSourceOptions{
            .source_kind = MigrationSourceKind::Filesystem,
            .filesystem_root = migration_root,
        });

    std::string err;
    if (!service.Start(&err)) {
        if (error_out) *error_out = "failed starting DBService: " + err;
        return false;
    }

    auto stop_service = [&]() { service.Stop(); };

    std::int64_t savestate_id = 0;
    if (!SeedStateSavestate(service.StateDb(), options.savestate_file, &savestate_id, &err)) {
        if (error_out) *error_out = "failed seeding StateDB savestate: " + err;
        stop_service();
        return false;
    }

    std::int64_t seed_probe_spec_id = 0;
    if (!SeedAuthoringSpec(service.AuthoringDb(), &seed_probe_spec_id, &err)) {
        if (error_out) *error_out = "failed seeding AuthoringDB seedprobe spec: " + err;
        stop_service();
        return false;
    }

    auto* execution_db = dynamic_cast<simcore::db::execution::workflow::SqliteExecutionDb*>(service.ExecutionDb());
    if (execution_db == nullptr) {
        if (error_out) *error_out = "DBService execution db is not sqlite-backed";
        stop_service();
        return false;
    }

    if (!SeedExecutionWorkflow(service.AnalysisDb(), execution_db, savestate_id, seed_probe_spec_id, &err)) {
        if (error_out) *error_out = "failed seeding execution workflow rows: " + err;
        stop_service();
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
            .worker_dir_root = options.worker_dir_root.value_or(db_paths.execution_db_path.parent_path() / "workers").string(),
        },
        CoordinatorIntegrationConfig{},
        schedule);

    coordinator.Start();
    auto* ui_read_db = service.UiReadDb();
    if (ui_read_db == nullptr) {
        coordinator.Stop();
        stop_service();
        if (error_out) *error_out = "DBService ui read db unavailable";
        return false;
    }

    const auto started = std::chrono::steady_clock::now();
    std::size_t poll_count = 0;
    while (std::chrono::steady_clock::now() - started < std::chrono::milliseconds(options.timeout_ms)) {
        (void)ui_read_db->ListProjectionSubscriptions("Execution", "exec_outbox_message");
        ++poll_count;

        const auto telemetry = coordinator.SnapshotTelemetry();
        if (telemetry.ready_scan_count > 0) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
    }

    coordinator.Stop();
    stop_service();

    if (poll_count == 0) {
        if (error_out) *error_out = "UiReadDB polling loop did not execute";
        return false;
    }

    return true;
}

} // namespace simcore::e2e
