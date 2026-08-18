#include "CoordinatorRuntime.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "Authoring/IAuthoringDb.h"
#include "Execution/IExecutionDb.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/WorkerResultBlobStore.h"

namespace savor::runner::parallel::savordb {

CoordinatorRuntime::~CoordinatorRuntime() {
    Stop(nullptr);
}

bool CoordinatorRuntime::Start(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAuthoringDb* authoring_db,
    const savor::db::execution::programdb::ProgramKindRegistry*
        program_kind_registry,
    CoordinatorRuntimeConfig config, std::string* error_out) {
    if (started_) {
        if (error_out != nullptr)
            error_out->clear();
        return true;
    }
    if (execution_db == nullptr || authoring_db == nullptr ||
        program_kind_registry == nullptr) {
        if (error_out != nullptr) {
            *error_out =
                "coordinator runtime requires execution DB, Authoring DB, "
                "and program registry";
        }
        return false;
    }
    if (!config.state_compatibility.Complete() ||
        config.state_compatibility.runtime_revision.empty() ||
        config.object_store_root.empty()) {
        if (error_out != nullptr) {
            *error_out =
                "coordinator runtime requires complete state compatibility "
                "and an object-store root";
        }
        return false;
    }

    try {
        execution_paused_ = config.initially_paused;
        const auto effective_poll =
            std::max(config.poll_interval, std::chrono::milliseconds(1));
        queued_execution_db_ =
            dynamic_cast<savor::db::execution::QueuedExecutionDb*>(
                execution_db);
        blob_store_ =
            std::make_unique<savor::db::execution::WorkerResultBlobStore>(
                config.object_store_root);

        savor::db::execution::workflow::WorkflowCoordinatorConfig
            workflow_config{};
        workflow_config.workflow_enabled = true;
        workflow_config.strict_smoke_terminal_on_failure = false;
        workflow_config.poll_interval = effective_poll;
        workflow_coordinator_ = std::make_unique<
            savor::db::execution::workflow::WorkflowCoordinatorService>(
            execution_db, program_kind_registry, workflow_config,
            config.event_line_callback, nullptr, authoring_db);

        blob_cleanup_ = std::make_unique<
            savor::db::execution::programdb::WorkerResultBlobCleanupService>(
            execution_db, blob_store_.get(),
            savor::db::execution::programdb::WorkerResultBlobCleanupConfig{
                .enabled = true,
                .poll_interval = effective_poll,
            });

        result_processor_ = std::make_unique<
            savor::db::execution::programdb::ProgramResultProcessor>(
            execution_db, program_kind_registry, blob_store_.get(),
            savor::db::execution::programdb::ProgramResultProcessorConfig{
                .enabled = true},
            [this](std::uint64_t commit_sequence, std::int64_t workflow_step_id,
                   std::int64_t job_id) {
                if (workflow_coordinator_ != nullptr) {
                    (void)workflow_coordinator_->PublishTerminalCommit({
                        .commit_sequence = commit_sequence,
                        .workflow_step_id = workflow_step_id,
                        .job_id = job_id,
                    });
                }
                if (blob_cleanup_ != nullptr)
                    blob_cleanup_->Wake();
            },
            std::move(config.event_line_callback));

        worker_coordinator_ =
            std::make_unique<WorkerCoordinator>(std::move(config.worker));
        for (auto& surface : config.visual_surfaces) {
            (void)worker_coordinator_->SetWorkerVisualSurface(
                surface.worker_id, surface.render_widget_handle,
                std::move(surface.host_events_pipe_name));
        }

        job_execution_coordinator_ = std::make_unique<JobExecutionCoordinator>(
            execution_db, program_kind_registry, worker_coordinator_.get(),
            blob_store_.get(),
            JobExecutionCoordinatorConfig{
                .poll_interval = effective_poll,
                .state_compatibility = std::move(config.state_compatibility),
            });
        job_execution_coordinator_->SetPaused(execution_paused_);

        // These callbacks must be installed before ProgramResultProcessor
        // starts; its callback configuration is intentionally immutable while
        // running.
        result_processor_->SetCancellationCallbacks(
            [this](
                std::uint64_t hold_id,
                const std::vector<savor::db::ExecutionCancellationRequestSpec>&
                    cancellations) {
                if (job_execution_coordinator_ != nullptr) {
                    job_execution_coordinator_
                        ->RegisterCancellationCommitPending(hold_id,
                                                            cancellations);
                }
            },
            [this](std::uint64_t hold_id,
                   const std::vector<savor::db::CommittedJobCancellation>&
                       cancellations) {
                if (job_execution_coordinator_ != nullptr) {
                    job_execution_coordinator_->RegisterCommittedCancellations(
                        hold_id, cancellations);
                }
            });

        std::string error;
        if (!workflow_coordinator_->Start(&error)) {
            return FailStart("workflow coordinator startup failed: " + error,
                             error_out);
        }
        workflow_started_ = true;
        if (!blob_cleanup_->Start(&error)) {
            return FailStart("worker result blob cleanup startup failed: " +
                                 error,
                             error_out);
        }
        cleanup_started_ = true;
        const auto worker_start = worker_coordinator_->Start();
        if (!worker_start.started()) {
            last_fleet_startup_snapshot_ =
                worker_coordinator_->SnapshotFleetStartup();
            return FailStart("worker coordinator did not start: " +
                                 worker_start.diagnostic,
                             error_out);
        }
        worker_started_ = true;
        // WorkerCoordinator::Start resets its local admission state, so apply
        // the runtime's authoritative initial pause only after startup commits.
        worker_coordinator_->SetPaused(execution_paused_);
        if (!job_execution_coordinator_->Start(&error)) {
            return FailStart("job execution coordinator startup failed: " +
                                 error,
                             error_out);
        }
        job_execution_started_ = true;
        if (!result_processor_->Start(&error)) {
            return FailStart(
                "program result processor startup failed: " + error, error_out);
        }
        result_processor_started_ = true;

        // This is the final startup commit. Dispatch is intentionally
        // impossible until cancellation recovery and both callback directions
        // are live.
        job_execution_coordinator_->OpenCancellationAdmission();
        started_ = true;
        if (error_out != nullptr)
            error_out->clear();
        return true;
    } catch (const std::exception& ex) {
        return FailStart(std::string("coordinator runtime startup threw: ") +
                             ex.what(),
                         error_out);
    } catch (...) {
        return FailStart(
            "coordinator runtime startup threw an unknown exception",
            error_out);
    }
}

bool CoordinatorRuntime::Stop(std::string* error_out) {
    bool ok = true;
    std::string errors;
    const auto append_error = [&](std::string message) {
        ok = false;
        if (!errors.empty())
            errors += "; ";
        errors += std::move(message);
    };

    if (job_execution_started_ && job_execution_coordinator_ != nullptr) {
        job_execution_coordinator_->Quiesce();
        std::string error;
        if (!job_execution_coordinator_->ReleaseBufferedClaims(&error)) {
            append_error("release buffered claims failed: " + error);
        }
    }
    if (worker_started_ && worker_coordinator_ != nullptr) {
        worker_coordinator_->Stop();
        worker_started_ = false;
    }
    if (job_execution_started_ && job_execution_coordinator_ != nullptr) {
        std::string error;
        if (!job_execution_coordinator_->RecoverAfterWorkersStopped(&error)) {
            append_error("post-worker dispatch recovery failed: " + error);
        }
        job_execution_coordinator_->Stop();
        job_execution_started_ = false;
    }
    if (result_processor_started_ && result_processor_ != nullptr) {
        result_processor_->Stop();
        result_processor_started_ = false;
    }
    if (cleanup_started_ && blob_cleanup_ != nullptr) {
        blob_cleanup_->Stop();
        cleanup_started_ = false;
    }
    if (workflow_started_ && workflow_coordinator_ != nullptr) {
        workflow_coordinator_->Stop();
        workflow_started_ = false;
    }

    started_ = false;
    job_execution_coordinator_.reset();
    worker_coordinator_.reset();
    result_processor_.reset();
    blob_cleanup_.reset();
    workflow_coordinator_.reset();
    blob_store_.reset();
    queued_execution_db_ = nullptr;
    if (error_out != nullptr)
        *error_out = std::move(errors);
    return ok;
}

void CoordinatorRuntime::SetExecutionPaused(bool paused) {
    execution_paused_ = paused;
    if (worker_coordinator_ != nullptr) {
        worker_coordinator_->SetPaused(paused);
    }
    if (job_execution_coordinator_ != nullptr) {
        job_execution_coordinator_->SetPaused(paused);
    }
}

void CoordinatorRuntime::SetDesiredWorkerCount(std::size_t desired_workers) {
    if (worker_coordinator_ != nullptr) {
        worker_coordinator_->SetDesiredWorkerCount(desired_workers);
    }
}

bool CoordinatorRuntime::IsStarted() const noexcept {
    return started_;
}

bool CoordinatorRuntime::IsExecutionPaused() const noexcept {
    return execution_paused_;
}

CoordinatorRuntimeTelemetry CoordinatorRuntime::SnapshotTelemetry() const {
    return {
        .workflow = workflow_coordinator_ != nullptr
                        ? workflow_coordinator_->SnapshotTelemetry()
                        : savor::db::execution::workflow::
                              WorkflowCoordinatorTelemetry{},
        .execution = job_execution_coordinator_ != nullptr
                         ? job_execution_coordinator_->SnapshotTelemetry()
                         : JobExecutionCoordinatorTelemetry{},
        .worker = worker_coordinator_ != nullptr
                      ? worker_coordinator_->SnapshotTelemetry()
                      : WorkerCoordinatorTelemetry{},
        .results = result_processor_ != nullptr
                       ? result_processor_->SnapshotTelemetry()
                       : savor::db::execution::programdb::
                             ProgramResultProcessorTelemetry{},
        .cleanup = blob_cleanup_ != nullptr
                       ? blob_cleanup_->SnapshotTelemetry()
                       : savor::db::execution::programdb::
                             WorkerResultBlobCleanupTelemetry{},
        .worker_admission_paused =
            worker_coordinator_ != nullptr && worker_coordinator_->IsPaused(),
        .lanes = job_execution_coordinator_ != nullptr
                     ? job_execution_coordinator_->SnapshotWorkerLanes()
                     : std::vector<JobExecutionWorkerLaneSnapshot>{},
        .execution_db_queue =
            queued_execution_db_ != nullptr
                ? std::optional(queued_execution_db_->GetTelemetrySnapshot())
                : std::nullopt,
    };
}

std::vector<WorkerSnapshot> CoordinatorRuntime::SnapshotWorkers() const {
    return worker_coordinator_ != nullptr
               ? worker_coordinator_->SnapshotWorkers()
               : std::vector<WorkerSnapshot>{};
}

std::vector<ReadyWorkerDispatchSnapshot>
CoordinatorRuntime::SnapshotReadyWorkers() const {
    return worker_coordinator_ != nullptr
               ? worker_coordinator_->SnapshotReadyWorkers()
               : std::vector<ReadyWorkerDispatchSnapshot>{};
}

std::vector<JobExecutionCoordinatorWarning>
CoordinatorRuntime::SnapshotExecutionWarnings() const {
    return job_execution_coordinator_ != nullptr
               ? job_execution_coordinator_->SnapshotWarnings()
               : std::vector<JobExecutionCoordinatorWarning>{};
}

FleetStartupSnapshot CoordinatorRuntime::SnapshotFleetStartup() const {
    return worker_coordinator_ != nullptr
               ? worker_coordinator_->SnapshotFleetStartup()
               : last_fleet_startup_snapshot_;
}

WorkerCoordinatorStartResult
CoordinatorRuntime::SnapshotWorkerStartResult() const {
    return worker_coordinator_ != nullptr
               ? worker_coordinator_->SnapshotStartResult()
               : WorkerCoordinatorStartResult{};
}

bool CoordinatorRuntime::FailStart(std::string message,
                                   std::string* error_out) {
    std::string shutdown_error;
    (void)Stop(&shutdown_error);
    if (!shutdown_error.empty()) {
        message += "; shutdown: " + shutdown_error;
    }
    if (error_out != nullptr)
        *error_out = std::move(message);
    return false;
}

} // namespace savor::runner::parallel::savordb
