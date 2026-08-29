#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Execution/JobExecutionCoordinator.h"
#include "Execution/ProgramDB/ProgramResultProcessor.h"
#include "Execution/ProgramDB/ResultStagingCleanupService.h"
#include "Execution/ProgramDB/WorkerResultBlobCleanupService.h"
#include "Execution/QueuedExecutionDb.h"
#include "Execution/WorkerCoordinator.h"
#include "Execution/Workflow/WorkflowCoordinatorService.h"

namespace savor::db {
struct IAuthoringDb;
struct IExecutionDb;
} // namespace savor::db

namespace savor::db::execution {
class WorkerResultBlobStore;
}

namespace savor::db::execution::programdb {
class ProgramKindRegistry;
}

namespace savor::runner::parallel::savordb {

struct CoordinatorWorkerVisualSurface {
    std::size_t worker_id = 0;
    std::uint64_t render_widget_handle = 0;
    std::uint64_t surface_generation = 0;
    std::uint32_t owner_process_id = 0;
    std::string host_events_pipe_name;
};

struct CoordinatorRuntimeConfig {
    WorkerCoordinatorConfig worker;
    std::chrono::milliseconds poll_interval{100};
    savor::runtime::ArtifactCompatibilityToken state_compatibility;
    bool initially_paused = false;
    std::filesystem::path object_store_root;
    savor::db::execution::workflow::WorkflowCoordinatorService::
        EventLineCallback event_line_callback;
    std::vector<CoordinatorWorkerVisualSurface> visual_surfaces;
};

struct CoordinatorRuntimeTelemetry {
    savor::db::execution::workflow::WorkflowCoordinatorTelemetry workflow;
    JobExecutionCoordinatorTelemetry execution;
    WorkerCoordinatorTelemetry worker;
    savor::db::execution::programdb::ProgramResultProcessorTelemetry results;
    savor::db::execution::programdb::WorkerResultBlobCleanupTelemetry cleanup;
    savor::db::execution::programdb::ResultStagingCleanupTelemetry
        result_staging_cleanup;
    bool worker_admission_paused = false;
    std::vector<JobExecutionWorkerLaneSnapshot> lanes;
    std::optional<savor::db::execution::ExecutionQueueTelemetrySnapshot>
        execution_db_queue;
};

// Owns the complete production coordination stack. Applications must use this
// composition boundary so cancellation admission, service ordering, and
// shutdown recovery cannot be partially initialized by individual callers.
class CoordinatorRuntime final {
  public:
    CoordinatorRuntime() = default;
    ~CoordinatorRuntime();

    CoordinatorRuntime(const CoordinatorRuntime&) = delete;
    CoordinatorRuntime& operator=(const CoordinatorRuntime&) = delete;

    bool Start(savor::db::IExecutionDb* execution_db,
               savor::db::IAuthoringDb* authoring_db,
               const savor::db::execution::programdb::ProgramKindRegistry*
                   program_kind_registry,
               CoordinatorRuntimeConfig config,
               std::string* error_out = nullptr);
    bool Stop(std::string* error_out = nullptr);

    void SetExecutionPaused(bool paused);
    bool SetDesiredWorkerCount(
        std::size_t desired_workers,
        std::string* error_out = nullptr);
    bool ResizeVisualWorkerPool(
        std::size_t desired_workers,
        std::vector<CoordinatorWorkerVisualSurface> surfaces,
        std::string* error_out = nullptr);
    bool SetWorkerVisualSurface(
        CoordinatorWorkerVisualSurface surface,
        std::string* error_out = nullptr);
    void InvalidateWorkerVisualSurface(
        std::size_t worker_id,
        std::uint64_t surface_generation);

    [[nodiscard]] bool IsStarted() const noexcept;
    [[nodiscard]] bool IsExecutionPaused() const noexcept;
    [[nodiscard]] CoordinatorRuntimeTelemetry SnapshotTelemetry() const;
    [[nodiscard]] std::vector<WorkerSnapshot> SnapshotWorkers() const;
    [[nodiscard]] std::vector<ReadyWorkerDispatchSnapshot>
    SnapshotReadyWorkers() const;
    [[nodiscard]] std::vector<JobExecutionCoordinatorWarning>
    SnapshotExecutionWarnings() const;
    [[nodiscard]] FleetStartupSnapshot SnapshotFleetStartup() const;
    [[nodiscard]] WorkerCoordinatorStartResult
    SnapshotWorkerStartResult() const;

  private:
    bool FailStart(std::string message, std::string* error_out);

    std::unique_ptr<savor::db::execution::WorkerResultBlobStore> blob_store_;
    savor::db::execution::QueuedExecutionDb* queued_execution_db_ = nullptr;
    std::unique_ptr<savor::db::execution::workflow::WorkflowCoordinatorService>
        workflow_coordinator_;
    std::unique_ptr<
        savor::db::execution::programdb::WorkerResultBlobCleanupService>
        blob_cleanup_;
    std::unique_ptr<savor::db::execution::programdb::ProgramResultProcessor>
        result_processor_;
    std::unique_ptr<
        savor::db::execution::programdb::ResultStagingCleanupService>
        result_staging_cleanup_;
    std::unique_ptr<WorkerCoordinator> worker_coordinator_;
    std::unique_ptr<JobExecutionCoordinator> job_execution_coordinator_;
    bool workflow_started_ = false;
    bool cleanup_started_ = false;
    bool result_processor_started_ = false;
    bool result_staging_cleanup_started_ = false;
    bool worker_started_ = false;
    bool job_execution_started_ = false;
    bool started_ = false;
    bool execution_paused_ = false;
    FleetStartupSnapshot last_fleet_startup_snapshot_;
};

} // namespace savor::runner::parallel::savordb
