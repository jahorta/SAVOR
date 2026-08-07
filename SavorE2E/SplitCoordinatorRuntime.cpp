#include "SplitCoordinatorRuntime.h"

#include <algorithm>
#include <utility>

#include "Authoring/IAuthoringDb.h"
#include "Execution/IExecutionDb.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/WorkerResultBlobStore.h"

namespace savor::e2e {

SplitCoordinatorRuntime::~SplitCoordinatorRuntime() {
    Stop(nullptr);
}

void SplitCoordinatorRuntime::SetExecutionPaused(bool paused) {
    execution_paused_ = paused;
    if (job_execution_coordinator_ != nullptr) {
        job_execution_coordinator_->SetPaused(paused);
    }
}

bool SplitCoordinatorRuntime::Start(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAuthoringDb* authoring_db,
    const savor::db::execution::programdb::ProgramKindRegistry*
        program_kind_registry,
    savor::runner::parallel::savordb::WorkerCoordinatorConfig worker_config,
    const std::filesystem::path& object_store_root,
    std::chrono::milliseconds poll_interval,
    savor::runtime::ArtifactCompatibilityToken state_compatibility,
    EventLineCallback event_line_callback,
    std::string* error_out) {
    if (execution_db == nullptr || authoring_db == nullptr
        || program_kind_registry == nullptr) {
        if (error_out != nullptr) {
            *error_out =
                "split coordinator runtime requires execution DB, Authoring "
                "DB, and program registry";
        }
        return false;
    }
    if (!state_compatibility.Complete()
        || state_compatibility.runtime_revision.empty()) {
        if (error_out != nullptr) {
            *error_out =
                "split coordinator runtime requires complete state "
                "compatibility";
        }
        return false;
    }
    if (started_) {
        return true;
    }

    const auto effective_poll =
        std::max(poll_interval, std::chrono::milliseconds(1));
    queued_execution_db_ =
        dynamic_cast<savor::db::execution::QueuedExecutionDb*>(execution_db);
    blob_store_ =
        std::make_unique<savor::db::execution::WorkerResultBlobStore>(
            object_store_root);

    savor::db::execution::workflow::WorkflowCoordinatorConfig
        workflow_config{};
    workflow_config.workflow_enabled = true;
    workflow_config.strict_smoke_terminal_on_failure = false;
    workflow_config.poll_interval = effective_poll;
    workflow_coordinator_ = std::make_unique<
        savor::db::execution::workflow::WorkflowCoordinatorService>(
            execution_db,
            program_kind_registry,
            workflow_config,
            event_line_callback,
            nullptr,
            authoring_db);

    blob_cleanup_ = std::make_unique<
        savor::db::execution::programdb::WorkerResultBlobCleanupService>(
            execution_db,
            blob_store_.get(),
            savor::db::execution::programdb::
                WorkerResultBlobCleanupConfig{
                    .enabled = true,
                    .poll_interval = effective_poll,
                });

    result_processor_ = std::make_unique<
        savor::db::execution::programdb::ProgramResultProcessor>(
            execution_db,
            program_kind_registry,
            blob_store_.get(),
            savor::db::execution::programdb::
                ProgramResultProcessorConfig{.enabled = true},
            [this](
                std::uint64_t commit_sequence,
                std::int64_t workflow_step_id,
                std::int64_t job_id) {
                if (workflow_coordinator_ != nullptr) {
                    (void)workflow_coordinator_->PublishTerminalCommit({
                        .commit_sequence = commit_sequence,
                        .workflow_step_id = workflow_step_id,
                        .job_id = job_id,
                    });
                }
                if (blob_cleanup_ != nullptr) {
                    blob_cleanup_->Wake();
                }
            },
            std::move(event_line_callback));

    worker_coordinator_ = std::make_unique<
        savor::runner::parallel::savordb::WorkerCoordinator>(
            std::move(worker_config));
    job_execution_coordinator_ = std::make_unique<
        savor::runner::parallel::savordb::JobExecutionCoordinator>(
            execution_db,
            program_kind_registry,
            worker_coordinator_.get(),
            blob_store_.get(),
            savor::runner::parallel::savordb::
                JobExecutionCoordinatorConfig{
                    .poll_interval = effective_poll,
                    .state_compatibility = std::move(state_compatibility),
                });
    job_execution_coordinator_->SetPaused(execution_paused_);
    result_processor_->SetCancellationCallbacks(
        [this](
            std::uint64_t hold_id,
            const std::vector<savor::db::ExecutionCancellationRequestSpec>&
                cancellations) {
            if (job_execution_coordinator_ != nullptr) {
                job_execution_coordinator_
                    ->RegisterCancellationCommitPending(
                        hold_id, cancellations);
            }
        },
        [this](
            std::uint64_t hold_id,
            const std::vector<savor::db::CommittedJobCancellation>&
                cancellations) {
            if (job_execution_coordinator_ != nullptr) {
                job_execution_coordinator_->RegisterCommittedCancellations(
                    hold_id, cancellations);
            }
        });

    std::string error;
    if (!workflow_coordinator_->Start(&error)) {
        return FailStart(
            "workflow coordinator startup failed: " + error, error_out);
    }
    workflow_started_ = true;
    if (!blob_cleanup_->Start(&error)) {
        return FailStart(
            "worker result blob cleanup startup failed: " + error,
            error_out);
    }
    cleanup_started_ = true;
    const auto worker_start = worker_coordinator_->Start();
    if (!worker_start.started()) {
        last_fleet_startup_snapshot_ =
            worker_coordinator_->SnapshotFleetStartup();
        return FailStart(
            "worker coordinator did not start: " + worker_start.diagnostic,
            error_out);
    }
    worker_started_ = true;
    if (!job_execution_coordinator_->Start(&error)) {
        return FailStart(
            "job execution coordinator startup failed: " + error,
            error_out);
    }
    job_execution_started_ = true;
    if (!result_processor_->Start(&error)) {
        return FailStart(
            "program result processor startup failed: " + error,
            error_out);
    }
    result_processor_started_ = true;
    job_execution_coordinator_->OpenCancellationAdmission();
    started_ = true;
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

bool SplitCoordinatorRuntime::Stop(std::string* error_out) {
    bool ok = true;
    std::string errors;
    const auto append_error = [&](std::string message) {
        ok = false;
        if (!errors.empty()) {
            errors += "; ";
        }
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
    if (error_out != nullptr) {
        *error_out = std::move(errors);
    }
    return ok;
}

SplitCoordinatorTelemetry SplitCoordinatorRuntime::SnapshotTelemetry() const {
    return {
        .workflow = workflow_coordinator_ != nullptr
            ? workflow_coordinator_->SnapshotTelemetry()
            : savor::db::execution::workflow::
                WorkflowCoordinatorTelemetry{},
        .execution = job_execution_coordinator_ != nullptr
            ? job_execution_coordinator_->SnapshotTelemetry()
            : savor::runner::parallel::savordb::
                JobExecutionCoordinatorTelemetry{},
        .worker = worker_coordinator_ != nullptr
            ? worker_coordinator_->SnapshotTelemetry()
            : savor::runner::parallel::savordb::WorkerCoordinatorTelemetry{},
        .results = result_processor_ != nullptr
            ? result_processor_->SnapshotTelemetry()
            : savor::db::execution::programdb::
                ProgramResultProcessorTelemetry{},
        .cleanup = blob_cleanup_ != nullptr
            ? blob_cleanup_->SnapshotTelemetry()
            : savor::db::execution::programdb::
                WorkerResultBlobCleanupTelemetry{},
        .lanes = job_execution_coordinator_ != nullptr
            ? job_execution_coordinator_->SnapshotWorkerLanes()
            : std::vector<savor::runner::parallel::savordb::
                JobExecutionWorkerLaneSnapshot>{},
        .execution_db_queue = queued_execution_db_ != nullptr
            ? std::optional(queued_execution_db_->GetTelemetrySnapshot())
            : std::nullopt,
    };
}

std::vector<WorkerSnapshot> SplitCoordinatorRuntime::SnapshotWorkers() const {
    return worker_coordinator_ != nullptr
        ? worker_coordinator_->SnapshotWorkers()
        : std::vector<WorkerSnapshot>{};
}

std::vector<savor::runner::parallel::savordb::
    ReadyWorkerCompatibilitySnapshot>
SplitCoordinatorRuntime::SnapshotReadyWorkers() const {
    return worker_coordinator_ != nullptr
        ? worker_coordinator_->SnapshotReadyWorkers()
        : std::vector<savor::runner::parallel::savordb::
            ReadyWorkerCompatibilitySnapshot>{};
}

std::vector<savor::runner::parallel::savordb::
    JobExecutionCoordinatorWarning>
SplitCoordinatorRuntime::SnapshotExecutionWarnings() const {
    return job_execution_coordinator_ != nullptr
        ? job_execution_coordinator_->SnapshotWarnings()
        : std::vector<savor::runner::parallel::savordb::
            JobExecutionCoordinatorWarning>{};
}

savor::runner::parallel::savordb::FleetStartupSnapshot
SplitCoordinatorRuntime::SnapshotFleetStartup() const {
    return worker_coordinator_ != nullptr
        ? worker_coordinator_->SnapshotFleetStartup()
        : last_fleet_startup_snapshot_;
}

savor::runner::parallel::savordb::WorkerCoordinatorStartResult
SplitCoordinatorRuntime::SnapshotWorkerStartResult() const {
    return worker_coordinator_ != nullptr
        ? worker_coordinator_->SnapshotStartResult()
        : savor::runner::parallel::savordb::
            WorkerCoordinatorStartResult{};
}

bool SplitCoordinatorRuntime::FailStart(
    std::string message, std::string* error_out) {
    std::string shutdown_error;
    (void)Stop(&shutdown_error);
    if (!shutdown_error.empty()) {
        message += "; shutdown: " + shutdown_error;
    }
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
    return false;
}

} // namespace savor::e2e
