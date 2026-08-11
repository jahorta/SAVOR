#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Execution/ProgramDB/ProgramResultProcessor.h"
#include "Execution/ProgramDB/WorkerResultBlobCleanupService.h"
#include "Execution/QueuedExecutionDb.h"
#include "Execution/Workflow/WorkflowCoordinatorService.h"
#include "Execution/JobExecutionCoordinator.h"
#include "Execution/WorkerCoordinator.h"

namespace savor::db {
struct IAuthoringDb;
struct IExecutionDb;
}

namespace savor::db::execution {
class WorkerResultBlobStore;
}

namespace savor::db::execution::programdb {
class ProgramKindRegistry;
}

namespace savor::e2e {

struct SplitCoordinatorTelemetry {
    savor::db::execution::workflow::WorkflowCoordinatorTelemetry workflow;
    savor::runner::parallel::savordb::JobExecutionCoordinatorTelemetry
        execution;
    savor::runner::parallel::savordb::WorkerCoordinatorTelemetry worker;
    savor::db::execution::programdb::ProgramResultProcessorTelemetry results;
    savor::db::execution::programdb::WorkerResultBlobCleanupTelemetry cleanup;
    std::vector<
        savor::runner::parallel::savordb::JobExecutionWorkerLaneSnapshot>
        lanes;
    std::optional<savor::db::execution::ExecutionQueueTelemetrySnapshot>
        execution_db_queue;
};

// Shared E2E composition for the current split workflow, execution, worker,
// result-processing, and blob-cleanup subsystems. E2E scenarios must use this
// surface instead of the retired monolithic DB workflow worker coordinator.
class SplitCoordinatorRuntime {
public:
    using EventLineCallback =
        savor::db::execution::workflow::WorkflowCoordinatorService::
            EventLineCallback;

    SplitCoordinatorRuntime() = default;
    ~SplitCoordinatorRuntime();

    SplitCoordinatorRuntime(const SplitCoordinatorRuntime&) = delete;
    SplitCoordinatorRuntime& operator=(const SplitCoordinatorRuntime&) =
        delete;

    void SetExecutionPaused(bool paused);

    bool Start(
        savor::db::IExecutionDb* execution_db,
        savor::db::IAuthoringDb* authoring_db,
        const savor::db::execution::programdb::ProgramKindRegistry*
            program_kind_registry,
        savor::runner::parallel::savordb::WorkerCoordinatorConfig
            worker_config,
        const std::filesystem::path& object_store_root,
        std::chrono::milliseconds poll_interval,
        savor::runtime::ArtifactCompatibilityToken state_compatibility,
        EventLineCallback event_line_callback,
        std::string* error_out);

    bool Stop(std::string* error_out);

    [[nodiscard]] SplitCoordinatorTelemetry SnapshotTelemetry() const;
    [[nodiscard]] std::vector<WorkerSnapshot> SnapshotWorkers() const;
    [[nodiscard]] std::vector<
        savor::runner::parallel::savordb::ReadyWorkerDispatchSnapshot>
        SnapshotReadyWorkers() const;
    [[nodiscard]] std::vector<
        savor::runner::parallel::savordb::JobExecutionCoordinatorWarning>
        SnapshotExecutionWarnings() const;
    [[nodiscard]] savor::runner::parallel::savordb::FleetStartupSnapshot
        SnapshotFleetStartup() const;
    [[nodiscard]] savor::runner::parallel::savordb::
        WorkerCoordinatorStartResult SnapshotWorkerStartResult() const;

private:
    bool FailStart(std::string message, std::string* error_out);

    std::unique_ptr<savor::db::execution::WorkerResultBlobStore> blob_store_;
    savor::db::execution::QueuedExecutionDb* queued_execution_db_ = nullptr;
    std::unique_ptr<
        savor::db::execution::workflow::WorkflowCoordinatorService>
        workflow_coordinator_;
    std::unique_ptr<
        savor::db::execution::programdb::WorkerResultBlobCleanupService>
        blob_cleanup_;
    std::unique_ptr<
        savor::db::execution::programdb::ProgramResultProcessor>
        result_processor_;
    std::unique_ptr<savor::runner::parallel::savordb::WorkerCoordinator>
        worker_coordinator_;
    std::unique_ptr<
        savor::runner::parallel::savordb::JobExecutionCoordinator>
        job_execution_coordinator_;
    bool workflow_started_ = false;
    bool cleanup_started_ = false;
    bool result_processor_started_ = false;
    bool worker_started_ = false;
    bool job_execution_started_ = false;
    bool started_ = false;
    bool execution_paused_ = false;
    savor::runner::parallel::savordb::FleetStartupSnapshot
        last_fleet_startup_snapshot_;
};

} // namespace savor::e2e
