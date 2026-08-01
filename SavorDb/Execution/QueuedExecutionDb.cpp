#include "QueuedExecutionDb.h"

#include <memory>
#include <utility>

namespace savor::db::execution {
namespace {

constexpr const char* kInnerUnavailableError = "execution db inner service unavailable";

void SetError(std::string* error_out, std::string message) {
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
}

} // namespace

class QueuedExecutionDb::QueuedWorkflowQueryService final : public workflow::IWorkflowOrchestrationQueryService {
public:
    explicit QueuedWorkflowQueryService(const QueuedExecutionDb* owner)
        : owner_(owner) {
    }

    std::vector<workflow::WorkflowInstanceRecord> ListWorkflowInstances(
        workflow::WorkflowInstanceState state,
        std::int64_t created_at_utc_start,
        std::int64_t created_at_utc_end) const override {
        return owner_->ExecuteRead<std::vector<workflow::WorkflowInstanceRecord>>(
            [this, state, created_at_utc_start, created_at_utc_end]() {
                auto* service = owner_->inner_ != nullptr ? owner_->inner_->WorkflowQueryService() : nullptr;
                return service != nullptr
                    ? service->ListWorkflowInstances(state, created_at_utc_start, created_at_utc_end)
                    : std::vector<workflow::WorkflowInstanceRecord>{};
            },
            {});
    }

    std::vector<workflow::WorkflowReadyStepRecord> ListReadySteps(std::size_t limit) const override {
        return owner_->ExecuteRead<std::vector<workflow::WorkflowReadyStepRecord>>(
            [this, limit]() {
                auto* service = owner_->inner_ != nullptr ? owner_->inner_->WorkflowQueryService() : nullptr;
                return service != nullptr ? service->ListReadySteps(limit) : std::vector<workflow::WorkflowReadyStepRecord>{};
            },
            {});
    }

    std::int64_t CountActiveMaterializedWorkflows() const override {
        return owner_->ExecuteRead<std::int64_t>(
            [this]() {
                auto* service = owner_->inner_ != nullptr ? owner_->inner_->WorkflowQueryService() : nullptr;
                return service != nullptr ? service->CountActiveMaterializedWorkflows() : 0;
            },
            0);
    }

    std::optional<workflow::WorkflowGraphSnapshot> GetWorkflowGraph(std::int64_t workflow_instance_id) const override {
        return owner_->ExecuteRead<std::optional<workflow::WorkflowGraphSnapshot>>(
            [this, workflow_instance_id]() {
                auto* service = owner_->inner_ != nullptr ? owner_->inner_->WorkflowQueryService() : nullptr;
                return service != nullptr ? service->GetWorkflowGraph(workflow_instance_id) : std::nullopt;
            },
            std::nullopt);
    }

    std::optional<workflow::WorkflowStepTerminalSnapshot> GetStepTerminalSnapshotForJob(std::int64_t job_id) const override {
        return owner_->ExecuteRead<std::optional<workflow::WorkflowStepTerminalSnapshot>>(
            [this, job_id]() {
                auto* service = owner_->inner_ != nullptr ? owner_->inner_->WorkflowQueryService() : nullptr;
                return service != nullptr ? service->GetStepTerminalSnapshotForJob(job_id) : std::nullopt;
            },
            std::nullopt);
    }

    std::vector<workflow::WorkflowStepTerminalSnapshot> ListTerminalReadyStepSnapshots(std::size_t limit) const override {
        return owner_->ExecuteRead<std::vector<workflow::WorkflowStepTerminalSnapshot>>(
            [this, limit]() {
                auto* service = owner_->inner_ != nullptr ? owner_->inner_->WorkflowQueryService() : nullptr;
                return service != nullptr ? service->ListTerminalReadyStepSnapshots(limit) : std::vector<workflow::WorkflowStepTerminalSnapshot>{};
            },
            {});
    }

    std::vector<workflow::WorkflowStepRecord> ListBlockedSteps(std::int64_t workflow_instance_id) const override {
        return owner_->ExecuteRead<std::vector<workflow::WorkflowStepRecord>>(
            [this, workflow_instance_id]() {
                auto* service = owner_->inner_ != nullptr ? owner_->inner_->WorkflowQueryService() : nullptr;
                return service != nullptr ? service->ListBlockedSteps(workflow_instance_id) : std::vector<workflow::WorkflowStepRecord>{};
            },
            {});
    }

    std::vector<std::pair<std::int64_t, std::int64_t>> GetStepToJobSetMap(std::int64_t workflow_instance_id) const override {
        return owner_->ExecuteRead<std::vector<std::pair<std::int64_t, std::int64_t>>>(
            [this, workflow_instance_id]() {
                auto* service = owner_->inner_ != nullptr ? owner_->inner_->WorkflowQueryService() : nullptr;
                return service != nullptr
                    ? service->GetStepToJobSetMap(workflow_instance_id)
                    : std::vector<std::pair<std::int64_t, std::int64_t>>{};
            },
            {});
    }

    std::vector<workflow::WorkflowStepOutputRecord> ListStepOutputs(std::int64_t workflow_instance_id) const override {
        return owner_->ExecuteRead<std::vector<workflow::WorkflowStepOutputRecord>>(
            [this, workflow_instance_id]() {
                auto* service = owner_->inner_ != nullptr ? owner_->inner_->WorkflowQueryService() : nullptr;
                return service != nullptr
                    ? service->ListStepOutputs(workflow_instance_id)
                    : std::vector<workflow::WorkflowStepOutputRecord>{};
            },
            {});
    }

private:
    const QueuedExecutionDb* owner_ = nullptr;
};

class QueuedExecutionDb::QueuedWorkflowCommandService final : public workflow::IWorkflowOrchestrationCommandService {
public:
    explicit QueuedWorkflowCommandService(QueuedExecutionDb* owner)
        : owner_(owner) {
    }

    bool CreateWorkflowInstance(
        const workflow::WorkflowCreateInstanceCommand& command,
        std::int64_t* workflow_instance_id_out,
        std::string* error_out) override {
        return owner_->CreateWorkflowInstance(command, workflow_instance_id_out, error_out);
    }

    bool RetryFailedStep(const workflow::WorkflowRetryStepCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->RetryFailedStep(cmd, err);
        });
    }

    bool SkipStep(const workflow::WorkflowSkipStepCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->SkipStep(cmd, err);
        });
    }

    bool CancelWorkflowInstance(const workflow::WorkflowCancelInstanceCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->CancelWorkflowInstance(cmd, err);
        });
    }

    bool ResumeWorkflowInstance(const workflow::WorkflowResumeInstanceCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->ResumeWorkflowInstance(cmd, err);
        });
    }

    bool CompleteWorkflowInstance(const workflow::WorkflowCompleteInstanceCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->CompleteWorkflowInstance(cmd, err);
        });
    }

    bool PauseWorkflowInstance(const workflow::WorkflowPauseInstanceCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->PauseWorkflowInstance(cmd, err);
        });
    }

    bool TerminalFailWorkflowInstance(const workflow::WorkflowTerminalFailInstanceCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->TerminalFailWorkflowInstance(cmd, err);
        });
    }

    bool MarkStepMaterialized(const workflow::WorkflowMarkStepMaterializedCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->MarkStepMaterialized(cmd, err);
        });
    }

    bool MarkStepTerminal(const workflow::WorkflowMarkStepTerminalCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->MarkStepTerminal(cmd, err);
        });
    }

    bool RecordStepOutput(const workflow::WorkflowRecordStepOutputCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->RecordStepOutput(cmd, err);
        });
    }

    bool RecordInputBinding(const workflow::WorkflowRecordInputBindingCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->RecordInputBinding(cmd, err);
        });
    }

    bool MarkStepBlocked(const workflow::WorkflowMarkStepBlockedCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->MarkStepBlocked(cmd, err);
        });
    }

    bool MarkStepReady(const workflow::WorkflowMarkStepReadyCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->MarkStepReady(cmd, err);
        });
    }

    bool AppendDynamicSteps(const workflow::WorkflowAppendDynamicStepsCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->AppendDynamicSteps(cmd, err);
        });
    }

    bool ScheduleUnitActivation(const workflow::WorkflowScheduleUnitActivationCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->ScheduleUnitActivation(cmd, err);
        });
    }

    bool AppendLifecycleEvent(const workflow::WorkflowAppendLifecycleEventCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->AppendLifecycleEvent(cmd, err);
        });
    }

private:
    template <typename Command, typename Fn>
    bool Execute(const Command& command, std::string* error_out, Fn&& fn) {
        return owner_->ExecuteWrite<bool>(
            [this, command, fn = std::forward<Fn>(fn), error_out]() mutable {
                auto* service = owner_->inner_ != nullptr ? owner_->inner_->WorkflowCommandService() : nullptr;
                if (service == nullptr) {
                    SetError(error_out, kInnerUnavailableError);
                    return false;
                }
                return fn(service, command, error_out);
            },
            false,
            error_out);
    }

    QueuedExecutionDb* owner_ = nullptr;
};

class QueuedExecutionDb::QueuedJobCommandService final : public jobs::IJobEventCommandService {
public:
    explicit QueuedJobCommandService(QueuedExecutionDb* owner)
        : owner_(owner) {
    }

    bool AppendLifecycleEvent(const jobs::JobLifecycleEventCommand& command, std::string* error_out) override {
        return owner_->ExecuteWrite<bool>(
            [this, command, error_out]() {
                auto* service = owner_->inner_ != nullptr ? owner_->inner_->JobCommandService() : nullptr;
                if (service == nullptr) {
                    SetError(error_out, kInnerUnavailableError);
                    return false;
                }
                return service->AppendLifecycleEvent(command, error_out);
            },
            false,
            error_out);
    }

private:
    QueuedExecutionDb* owner_ = nullptr;
};

QueuedExecutionDb::QueuedExecutionDb(
    savor::db::IExecutionDb* inner,
    ExecutionQueueConfig config)
    : inner_(inner)
    , config_(config)
    , read_lane_(std::make_unique<core::QueuedDbLane>("execution-read", config_.read_capacity))
    , write_lane_(std::make_unique<core::QueuedDbLane>("execution-write", config_.write_capacity))
    , workflow_query_service_(std::make_unique<QueuedWorkflowQueryService>(this))
    , workflow_command_service_(std::make_unique<QueuedWorkflowCommandService>(this))
    , job_command_service_(std::make_unique<QueuedJobCommandService>(this)) {
}

QueuedExecutionDb::~QueuedExecutionDb() {
    Stop();
}

bool QueuedExecutionDb::Start(std::string* error_out) {
    if (inner_ == nullptr) {
        SetError(error_out, "execution db inner database is null");
        return false;
    }
    if (IsRunning()) {
        return true;
    }
    if (!write_lane_->Start(error_out)) {
        return false;
    }
    if (!read_lane_->Start(error_out)) {
        write_lane_->Stop();
        return false;
    }
    return true;
}

void QueuedExecutionDb::Stop() {
    if (read_lane_) {
        read_lane_->Stop();
    }
    if (write_lane_) {
        write_lane_->Stop();
    }
}

bool QueuedExecutionDb::IsRunning() const {
    return read_lane_ != nullptr
        && write_lane_ != nullptr
        && read_lane_->IsRunning()
        && write_lane_->IsRunning();
}

ExecutionQueueTelemetrySnapshot QueuedExecutionDb::GetTelemetrySnapshot() const {
    ExecutionQueueTelemetrySnapshot snapshot{};
    snapshot.queued = core::BuildQueuedDbTelemetrySnapshot(write_lane_.get(), read_lane_.get());
    snapshot.write_depth = snapshot.queued.write_depth;
    snapshot.read_depth = snapshot.queued.read_depth;
    snapshot.write_capacity = snapshot.queued.write_capacity;
    snapshot.read_capacity = snapshot.queued.read_capacity;
    snapshot.write_high_water_depth = snapshot.queued.write_high_water_depth;
    snapshot.read_high_water_depth = snapshot.queued.read_high_water_depth;
    snapshot.write_oldest_queued_age_ms = snapshot.queued.write_oldest_queued_age_ms;
    snapshot.read_oldest_queued_age_ms = snapshot.queued.read_oldest_queued_age_ms;
    snapshot.write_enqueued = snapshot.queued.write_enqueued;
    snapshot.read_enqueued = snapshot.queued.read_enqueued;
    snapshot.write_rejected = snapshot.queued.write_rejected;
    snapshot.read_rejected = snapshot.queued.read_rejected;
    snapshot.write_completed = snapshot.queued.write_completed;
    snapshot.read_completed = snapshot.queued.read_completed;
    snapshot.write_failed = snapshot.queued.write_failed;
    snapshot.read_failed = snapshot.queued.read_failed;
    snapshot.sqlite_busy = snapshot.queued.sqlite_busy;
    snapshot.sqlite_locked = snapshot.queued.sqlite_locked;
    return snapshot;
}

workflow::IWorkflowOrchestrationQueryService* QueuedExecutionDb::WorkflowQueryService() {
    return workflow_query_service_.get();
}

workflow::IWorkflowOrchestrationCommandService* QueuedExecutionDb::WorkflowCommandService() {
    return workflow_command_service_.get();
}

jobs::IJobEventCommandService* QueuedExecutionDb::JobCommandService() {
    return job_command_service_.get();
}

bool QueuedExecutionDb::CreateWorkflowInstance(
    const workflow::WorkflowCreateInstanceCommand& command,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out) {
    auto command_copy = std::make_shared<workflow::WorkflowCreateInstanceCommand>(command);
    return ExecuteWrite<bool>(
        [this, command_copy, workflow_instance_id_out, error_out]() {
            return inner_ != nullptr
                ? inner_->CreateWorkflowInstance(*command_copy, workflow_instance_id_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::CreateJobSet(const CreateJobSetCommand& command, std::int64_t* job_set_id_out, std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, job_set_id_out, error_out]() {
            return inner_ != nullptr ? inner_->CreateJobSet(command, job_set_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::EnqueueJob(const EnqueueJobCommand& command, std::int64_t* job_id_out, std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, job_id_out, error_out]() {
            return inner_ != nullptr ? inner_->EnqueueJob(command, job_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::EnsureMaterializingJobSet(
    const EnsureMaterializingJobSetCommand& command,
    EnsureMaterializingJobSetReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->EnsureMaterializingJobSet(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::CreatePendingJob(
    const CreatePendingJobCommand& command,
    CreatePendingJobReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->CreatePendingJob(command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::SealJobPopulation(
    const SealJobPopulationCommand& command,
    SealJobPopulationReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->SealJobPopulation(command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::PublishWorkset(
    const PublishWorksetCommand& command,
    PublishWorksetReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->PublishWorkset(command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::CompleteWorksetPublication(
    const CompleteWorksetPublicationCommand& command,
    CompleteWorksetPublicationReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->CompleteWorksetPublication(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

std::vector<ClaimedPublishedWorkset>
QueuedExecutionDb::ClaimPublishedWorksetBatch(
    const ClaimPublishedWorksetBatchCommand& command,
    std::string* error_out) {
    return ExecuteWrite<std::vector<ClaimedPublishedWorkset>>(
        [this, command, error_out]() {
            return inner_ != nullptr
                ? inner_->ClaimPublishedWorksetBatch(command, error_out)
                : std::vector<ClaimedPublishedWorkset>{};
        },
        {},
        error_out);
}

bool QueuedExecutionDb::RenewWorksetDispatchLease(
    const RenewWorksetDispatchLeaseCommand& command,
    WorksetDispatchLeaseReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->RenewWorksetDispatchLease(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::MarkWorksetDispatched(
    const MarkWorksetDispatchedCommand& command,
    WorksetDispatchMutationReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->MarkWorksetDispatched(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::ReleaseWorksetDispatch(
    const ReleaseWorksetDispatchCommand& command,
    WorksetDispatchMutationReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->ReleaseWorksetDispatch(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::MarkWorksetJobStarted(
    const MarkWorksetJobStartedCommand& command,
    WorksetJobStartReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->MarkWorksetJobStarted(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::StageWorkerTerminal(
    const StageWorkerTerminalCommand& command,
    StageWorkerTerminalReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->StageWorkerTerminal(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

std::optional<ClaimedExecutionFinishedJob>
QueuedExecutionDb::ClaimNextExecutionFinishedJob(
    const ClaimExecutionFinishedJobCommand& command,
    std::string* error_out) {
    return ExecuteWrite<std::optional<ClaimedExecutionFinishedJob>>(
        [this, command, error_out]() {
            return inner_ != nullptr
                ? inner_->ClaimNextExecutionFinishedJob(command, error_out)
                : std::nullopt;
        },
        std::nullopt,
        error_out);
}

bool QueuedExecutionDb::RenewResultProcessingLease(
    const RenewResultProcessingLeaseCommand& command,
    ResultProcessingReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->RenewResultProcessingLease(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::ParkResultProcessing(
    const ParkResultProcessingCommand& command,
    ResultProcessingReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->ParkResultProcessing(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::CommitResultFinalization(
    const CommitResultFinalizationCommand& command,
    ResultProcessingReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->CommitResultFinalization(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::RequestJobCancellation(
    const RequestJobCancellationCommand& command,
    JobCancellationReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->RequestJobCancellation(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

std::optional<ClaimedJobCancellation>
QueuedExecutionDb::ClaimNextJobCancellation(
    const ClaimJobCancellationCommand& command,
    std::string* error_out) {
    return ExecuteWrite<std::optional<ClaimedJobCancellation>>(
        [this, command, error_out]() {
            return inner_ != nullptr
                ? inner_->ClaimNextJobCancellation(command, error_out)
                : std::nullopt;
        },
        std::nullopt,
        error_out);
}

bool QueuedExecutionDb::MarkJobCancellationDelivered(
    const MarkJobCancellationDeliveredCommand& command,
    JobCancellationReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->MarkJobCancellationDelivered(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::ResolveJobCancellation(
    const ResolveJobCancellationCommand& command,
    JobCancellationReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->ResolveJobCancellation(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::IsTempBlobTracked(
    std::string_view relative_path,
    bool* tracked_out,
    std::string* error_out) const {
    const auto path = std::string(relative_path);
    return ExecuteRead<bool>(
        [this, path, tracked_out, error_out]() {
            if (inner_ == nullptr) {
                SetError(error_out, kInnerUnavailableError);
                return false;
            }
            return inner_->IsTempBlobTracked(
                path,
                tracked_out,
                error_out);
        },
        false,
        error_out);
}

std::optional<ClaimedTempBlobCleanup>
QueuedExecutionDb::ClaimNextTempBlobCleanup(
    const ClaimTempBlobCleanupCommand& command,
    std::string* error_out) {
    return ExecuteWrite<std::optional<ClaimedTempBlobCleanup>>(
        [this, command, error_out]() {
            return inner_ != nullptr
                ? inner_->ClaimNextTempBlobCleanup(command, error_out)
                : std::nullopt;
        },
        std::nullopt,
        error_out);
}

bool QueuedExecutionDb::CompleteTempBlobCleanup(
    const CompleteTempBlobCleanupCommand& command,
    ExecutionDbOperationDisposition* disposition_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, disposition_out, error_out]() {
            return inner_ != nullptr
                ? inner_->CompleteTempBlobCleanup(
                    command, disposition_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::RecoverExpiredWorksetDispatches(
    int max_dispatches,
    int* dispatches_recovered_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, max_dispatches, dispatches_recovered_out, error_out]() {
            return inner_ != nullptr
                ? inner_->RecoverExpiredWorksetDispatches(
                    max_dispatches, dispatches_recovered_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::RecoverExpiredResultProcessingLeases(
    int max_jobs,
    int* jobs_recovered_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, max_jobs, jobs_recovered_out, error_out]() {
            return inner_ != nullptr
                ? inner_->RecoverExpiredResultProcessingLeases(
                    max_jobs, jobs_recovered_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::RecoverExpiredCancellationDeliveryLeases(
    int max_requests,
    int* requests_recovered_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, max_requests, requests_recovered_out, error_out]() {
            return inner_ != nullptr
                ? inner_->RecoverExpiredCancellationDeliveryLeases(
                    max_requests, requests_recovered_out, error_out)
                : false;
        },
        false,
        error_out);
}

std::optional<ClaimedExecutionJob> QueuedExecutionDb::ClaimNextReadyExecutionJob(
    std::string_view claimed_by_token,
    std::int64_t lease_duration_ms,
    std::string* error_out) {
    const auto token = std::string(claimed_by_token);
    return ExecuteWrite<std::optional<ClaimedExecutionJob>>(
        [this, token, lease_duration_ms, error_out]() {
            return inner_ != nullptr ? inner_->ClaimNextReadyExecutionJob(token, lease_duration_ms, error_out) : std::nullopt;
        },
        std::nullopt,
        error_out);
}

std::vector<ClaimedExecutionJob> QueuedExecutionDb::ClaimBatchReadyExecutionJobs(
    std::string_view claimed_by_token,
    int requested_jobs,
    std::int64_t lease_duration_ms,
    std::string* error_out) {
    const auto token = std::string(claimed_by_token);
    return ExecuteWrite<std::vector<ClaimedExecutionJob>>(
        [this, token, requested_jobs, lease_duration_ms, error_out]() {
            return inner_ != nullptr
                ? inner_->ClaimBatchReadyExecutionJobs(token, requested_jobs, lease_duration_ms, error_out)
                : std::vector<ClaimedExecutionJob>{};
        },
        {},
        error_out);
}

bool QueuedExecutionDb::RenewExecutionJobLease(
    std::int64_t job_id,
    std::string_view claimed_by_token,
    std::int64_t lease_duration_ms,
    bool* renewed_out,
    std::string* error_out) {
    const auto token = std::string(claimed_by_token);
    return ExecuteWrite<bool>(
        [this, job_id, token, lease_duration_ms, renewed_out, error_out]() {
            return inner_ != nullptr
                ? inner_->RenewExecutionJobLease(job_id, token, lease_duration_ms, renewed_out, error_out)
                : false;
        },
        false,
        error_out);
}

std::vector<ExecutionJobLeaseRenewalReceipt> QueuedExecutionDb::RenewExecutionJobLeases(
    const std::vector<ExecutionJobLeaseRequest>& requests,
    std::int64_t lease_duration_ms,
    std::string* error_out) {
    return ExecuteWrite<std::vector<ExecutionJobLeaseRenewalReceipt>>(
        [this, requests, lease_duration_ms, error_out]() {
            return inner_ != nullptr
                ? inner_->RenewExecutionJobLeases(requests, lease_duration_ms, error_out)
                : std::vector<ExecutionJobLeaseRenewalReceipt>{};
        },
        {},
        error_out);
}

bool QueuedExecutionDb::MarkExecutionJobStarted(
    std::int64_t job_id,
    std::string_view claimed_by_token,
    std::string_view requested_by,
    ExecutionJobStartReceipt* receipt_out,
    std::string* error_out) {
    const auto token = std::string(claimed_by_token);
    const auto requester = std::string(requested_by);
    return ExecuteWrite<bool>(
        [this, job_id, token, requester, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->MarkExecutionJobStarted(
                    job_id,
                    token,
                    requester,
                    receipt_out,
                    error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::ValidateExecutionJobStartAuthoritySet(
    const std::vector<ExecutionJobLeaseRequest>& requests,
    ExecutionJobStartAuthoritySetReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, requests, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->ValidateExecutionJobStartAuthoritySet(
                    requests,
                    receipt_out,
                    error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::ConfirmExecutionJobTerminalAuthority(
    std::int64_t job_id,
    std::string_view claimed_by_token,
    std::uint64_t durable_attempt_id,
    std::int64_t lease_duration_ms,
    ExecutionJobTerminalAuthorityReceipt* receipt_out,
    std::string* error_out) {
    const auto token = std::string(claimed_by_token);
    return ExecuteWrite<bool>(
        [this,
         job_id,
         token,
         durable_attempt_id,
         lease_duration_ms,
         receipt_out,
         error_out]() {
            return inner_ != nullptr
                ? inner_->ConfirmExecutionJobTerminalAuthority(
                    job_id,
                    token,
                    durable_attempt_id,
                    lease_duration_ms,
                    receipt_out,
                    error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::RecoverExecutionJobAfterWorkerLoss(
    std::int64_t job_id,
    std::string_view claimed_by_token,
    std::uint64_t durable_attempt_id,
    std::string_view message,
    ExecutionJobWorkerLossRecoveryReceipt* receipt_out,
    std::string* error_out) {
    const auto token = std::string(claimed_by_token);
    const auto message_value = std::string(message);
    return ExecuteWrite<bool>(
        [this,
         job_id,
         token,
         durable_attempt_id,
         message_value,
         receipt_out,
         error_out]() {
            return inner_ != nullptr
                ? inner_->RecoverExecutionJobAfterWorkerLoss(
                    job_id,
                    token,
                    durable_attempt_id,
                    message_value,
                    receipt_out,
                    error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::RequeueExpiredExecutionLeases(
    int* rows_requeued_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, rows_requeued_out, error_out]() {
            return inner_ != nullptr ? inner_->RequeueExpiredExecutionLeases(rows_requeued_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::RequeueExpiredClaimedExecutionJobs(
    int* rows_requeued_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, rows_requeued_out, error_out]() {
            return inner_ != nullptr ? inner_->RequeueExpiredClaimedExecutionJobs(rows_requeued_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::RequeueClaimedExecutionJob(
    std::int64_t job_id,
    std::string_view claimed_by_token,
    std::string_view message,
    std::string* error_out) {
    const auto token = std::string(claimed_by_token);
    const auto message_value = std::string(message);
    return ExecuteWrite<bool>(
        [this, job_id, token, message_value, error_out]() {
            return inner_ != nullptr
                ? inner_->RequeueClaimedExecutionJob(job_id, token, message_value, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::RequeueInterruptedExecutionJobs(
    int* rows_requeued_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, rows_requeued_out, error_out]() {
            return inner_ != nullptr ? inner_->RequeueInterruptedExecutionJobs(rows_requeued_out, error_out) : false;
        },
        false,
        error_out);
}

std::optional<ExecutionJobRecord> QueuedExecutionDb::GetJob(std::int64_t job_id) const {
    return ExecuteRead<std::optional<ExecutionJobRecord>>(
        [this, job_id]() {
            return inner_ != nullptr ? inner_->GetJob(job_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<ExecutionJobEventRecord> QueuedExecutionDb::ListJobEvents(std::int64_t job_id, int limit) const {
    return ExecuteRead<std::vector<ExecutionJobEventRecord>>(
        [this, job_id, limit]() {
            return inner_ != nullptr ? inner_->ListJobEvents(job_id, limit) : std::vector<ExecutionJobEventRecord>{};
        },
        {});
}

bool QueuedExecutionDb::RecordJobOutput(const RecordExecutionJobOutputCommand& command, std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, error_out]() {
            return inner_ != nullptr ? inner_->RecordJobOutput(command, error_out) : false;
        },
        false,
        error_out);
}

std::vector<ExecutionJobOutputRecord> QueuedExecutionDb::ListJobOutputsForWorkflowStep(std::int64_t workflow_step_id) const {
    return ExecuteRead<std::vector<ExecutionJobOutputRecord>>(
        [this, workflow_step_id]() {
            return inner_ != nullptr
                ? inner_->ListJobOutputsForWorkflowStep(workflow_step_id)
                : std::vector<ExecutionJobOutputRecord>{};
        },
        {});
}

std::optional<std::string> QueuedExecutionDb::GetJobInputIni(std::int64_t job_id, std::string* error_out) const {
    return ExecuteRead<std::optional<std::string>>(
        [this, job_id, error_out]() {
            return inner_ != nullptr ? inner_->GetJobInputIni(job_id, error_out) : std::nullopt;
        },
        std::nullopt,
        error_out);
}

bool QueuedExecutionDb::RequeueJob(std::int64_t job_id, std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, job_id, error_out]() {
            return inner_ != nullptr ? inner_->RequeueJob(job_id, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::RestartFailedJob(std::int64_t job_id, std::optional<std::string> input_ini_override, std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, job_id, input_ini_override, error_out]() {
            return inner_ != nullptr ? inner_->RestartFailedJob(job_id, input_ini_override, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::CancelQueuedOrClaimedJob(std::int64_t job_id, std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, job_id, error_out]() {
            return inner_ != nullptr ? inner_->CancelQueuedOrClaimedJob(job_id, error_out) : false;
        },
        false,
        error_out);
}

std::optional<ExecutionJobSetProgressDetails> QueuedExecutionDb::GetJobSetProgress(std::int64_t job_set_id) const {
    return ExecuteRead<std::optional<ExecutionJobSetProgressDetails>>(
        [this, job_set_id]() {
            return inner_ != nullptr ? inner_->GetJobSetProgress(job_set_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<ExecutionJobSetJobRecord> QueuedExecutionDb::ListJobsInJobSet(
    std::int64_t job_set_id) const {
    return ExecuteRead<std::vector<ExecutionJobSetJobRecord>>(
        [this, job_set_id]() {
            return inner_ != nullptr
                ? inner_->ListJobsInJobSet(job_set_id)
                : std::vector<ExecutionJobSetJobRecord>{};
        },
        {});
}

std::vector<ExecutionJobSetJobRecord>
QueuedExecutionDb::ListJobsByProgramReference(
    std::int32_t program_kind,
    std::string_view program_ref_kind,
    std::int64_t program_ref_id) const {
    return ExecuteRead<std::vector<ExecutionJobSetJobRecord>>(
        [this, program_kind, ref_kind = std::string(program_ref_kind),
         program_ref_id]() {
            return inner_ != nullptr
                ? inner_->ListJobsByProgramReference(
                    program_kind, ref_kind, program_ref_id)
                : std::vector<ExecutionJobSetJobRecord>{};
        },
        {});
}

std::optional<ExecutionJobSetMaterializationRecord>
QueuedExecutionDb::GetJobSetByMaterializationKey(
    std::string_view materialization_key) const {
    return ExecuteRead<
        std::optional<ExecutionJobSetMaterializationRecord>>(
        [this, materialization_key =
                   std::string(materialization_key)]() {
            return inner_ != nullptr
                ? inner_->GetJobSetByMaterializationKey(
                      materialization_key)
                : std::nullopt;
        },
        std::nullopt);
}

std::vector<ExecutionChildJobSetProgressDetails> QueuedExecutionDb::GetChildJobSetProgress(std::int64_t parent_job_set_id) const {
    return ExecuteRead<std::vector<ExecutionChildJobSetProgressDetails>>(
        [this, parent_job_set_id]() {
            return inner_ != nullptr ? inner_->GetChildJobSetProgress(parent_job_set_id) : std::vector<ExecutionChildJobSetProgressDetails>{};
        },
        {});
}

bool QueuedExecutionDb::MarkQueuedJobsSuperseded(
    std::int64_t job_set_id,
    std::int64_t except_job_id,
    std::string* error_out,
    int* rows_superseded_out) {
    return ExecuteWrite<bool>(
        [this, job_set_id, except_job_id, error_out, rows_superseded_out]() {
            return inner_ != nullptr
                ? inner_->MarkQueuedJobsSuperseded(job_set_id, except_job_id, error_out, rows_superseded_out)
                : false;
        },
        false,
        error_out);
}

retention::OutboxRetentionPreview QueuedExecutionDb::PreviewOutboxRetention(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy) const {
    return ExecuteRead<retention::OutboxRetentionPreview>(
        [this, subscriptions, now_utc, policy]() {
            return inner_ != nullptr
                ? inner_->PreviewOutboxRetention(subscriptions, now_utc, policy)
                : retention::OutboxRetentionPreview{};
        },
        {});
}

bool QueuedExecutionDb::PurgeOutboxThroughRetentionFloor(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy,
    int max_rows,
    int* rows_deleted_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, subscriptions, now_utc, policy, max_rows, rows_deleted_out, error_out]() {
            return inner_ != nullptr
                ? inner_->PurgeOutboxThroughRetentionFloor(subscriptions, now_utc, policy, max_rows, rows_deleted_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::PurgeWorkflowHandlerDedupeOlderThan(
    std::int64_t last_seen_at_utc_exclusive,
    int max_rows,
    int* rows_deleted_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, last_seen_at_utc_exclusive, max_rows, rows_deleted_out, error_out]() {
            return inner_ != nullptr
                ? inner_->PurgeWorkflowHandlerDedupeOlderThan(last_seen_at_utc_exclusive, max_rows, rows_deleted_out, error_out)
                : false;
        },
        false,
        error_out);
}

std::optional<events::ExecutionWorkflowJobPayloadView> QueuedExecutionDb::ResolveExecutionWorkflowJobPayload(
    const events::EventEnvelope& envelope) const {
    return ExecuteRead<std::optional<events::ExecutionWorkflowJobPayloadView>>(
        [this, envelope]() {
            return inner_ != nullptr ? inner_->ResolveExecutionWorkflowJobPayload(envelope) : std::nullopt;
        },
        std::nullopt);
}

std::optional<events::ExecutionWorkflowJobPayloadView> QueuedExecutionDb::ResolveExecutionWorkflowJobPayload(
    std::string_view event_type,
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    const auto event_type_copy = std::string(event_type);
    const auto payload_ref_kind_copy = std::string(payload_ref_kind);
    return ExecuteRead<std::optional<events::ExecutionWorkflowJobPayloadView>>(
        [this, event_type_copy, event_version, payload_ref_kind_copy, payload_ref_id]() {
            return inner_ != nullptr
                ? inner_->ResolveExecutionWorkflowJobPayload(event_type_copy, event_version, payload_ref_kind_copy, payload_ref_id)
                : std::nullopt;
        },
        std::nullopt);
}

template <typename Result, typename Fn>
Result QueuedExecutionDb::ExecuteRead(
    Fn&& fn,
    Result fallback,
    std::string* error_out,
    const std::source_location& location) const {
    return core::QueuedDbExecutor::ExecuteQueued<Result>(
        *read_lane_,
        sqlite_call_mtx_,
        core::MakeQueuedDbOperationName("Execution", location),
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

template <typename Result, typename Fn>
Result QueuedExecutionDb::ExecuteWrite(
    Fn&& fn,
    Result fallback,
    std::string* error_out,
    const std::source_location& location) const {
    return core::QueuedDbExecutor::ExecuteQueued<Result>(
        *write_lane_,
        sqlite_call_mtx_,
        core::MakeQueuedDbOperationName("Execution", location),
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

} // namespace savor::db::execution
