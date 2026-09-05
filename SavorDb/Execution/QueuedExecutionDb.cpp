#include "QueuedExecutionDb.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

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

    std::optional<workflow::WorkflowStepSettlementSnapshot> GetStepSettlementSnapshotForJob(std::int64_t job_id) const override {
        return owner_->ExecuteRead<std::optional<workflow::WorkflowStepSettlementSnapshot>>(
            [this, job_id]() {
                auto* service = owner_->inner_ != nullptr ? owner_->inner_->WorkflowQueryService() : nullptr;
                return service != nullptr ? service->GetStepSettlementSnapshotForJob(job_id) : std::nullopt;
            },
            std::nullopt);
    }

    std::vector<workflow::WorkflowStepSettlementSnapshot> ListSettlementReadyStepSnapshots(std::size_t limit) const override {
        return owner_->ExecuteRead<std::vector<workflow::WorkflowStepSettlementSnapshot>>(
            [this, limit]() {
                auto* service = owner_->inner_ != nullptr ? owner_->inner_->WorkflowQueryService() : nullptr;
                return service != nullptr ? service->ListSettlementReadyStepSnapshots(limit) : std::vector<workflow::WorkflowStepSettlementSnapshot>{};
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

    std::optional<workflow::WorkflowTransitionActivationRecord>
    GetWorkflowTransitionActivation(
        std::int64_t workflow_instance_id,
        std::string_view activation_key) const override {
        return owner_->ExecuteRead<
            std::optional<workflow::WorkflowTransitionActivationRecord>>(
            [this, workflow_instance_id, key = std::string(activation_key)]() {
                auto* service = owner_->inner_ != nullptr
                    ? owner_->inner_->WorkflowQueryService() : nullptr;
                return service != nullptr
                    ? service->GetWorkflowTransitionActivation(
                        workflow_instance_id, key)
                    : std::nullopt;
            },
            std::nullopt);
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

    bool FailWorkflowInstance(const workflow::WorkflowFailInstanceCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->FailWorkflowInstance(cmd, err);
        });
    }

    bool InterruptWorkflowInstance(const workflow::WorkflowInterruptInstanceCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->InterruptWorkflowInstance(cmd, err);
        });
    }

    bool MarkStepMaterialized(const workflow::WorkflowMarkStepMaterializedCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->MarkStepMaterialized(cmd, err);
        });
    }

    bool CompleteWorkflowStep(const workflow::WorkflowCompleteStepCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->CompleteWorkflowStep(cmd, err);
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

    bool FreezeTransitionActivation(
        const workflow::WorkflowFreezeTransitionActivationCommand& command,
        workflow::WorkflowFreezeTransitionActivationReceipt* receipt_out,
        std::string* error_out) override {
        return owner_->ExecuteWrite<bool>(
            [this, command, receipt_out, error_out]() {
                auto* service = owner_->inner_ != nullptr
                    ? owner_->inner_->WorkflowCommandService() : nullptr;
                if (service == nullptr) {
                    SetError(error_out, kInnerUnavailableError);
                    return false;
                }
                return service->FreezeTransitionActivation(
                    command, receipt_out, error_out);
            },
            false,
            error_out);
    }

    bool ApplyTransitionActivation(
        const workflow::WorkflowApplyTransitionActivationCommand& command,
        std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->ApplyTransitionActivation(cmd, err);
        });
    }

    bool RecordTransitionActivationFailure(
        const workflow::WorkflowRecordTransitionActivationFailureCommand& command,
        std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->RecordTransitionActivationFailure(cmd, err);
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
    availability_stop_.store(false, std::memory_order_release);
    RefreshExecutionWorkAvailability();
    availability_thread_ =
        std::thread([this]() { AvailabilityWatcherLoop(); });
    return true;
}

void QueuedExecutionDb::Stop() {
    availability_stop_.store(true, std::memory_order_release);
    availability_cv_.notify_all();
    if (availability_thread_.joinable()) {
        availability_thread_.join();
    }
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
    snapshot.availability_watcher_reads =
        availability_watcher_reads_.load(std::memory_order_relaxed);
    snapshot.availability_signal_transitions =
        availability_signal_transitions_.load(
            std::memory_order_relaxed);
    snapshot.availability_callback_wakes =
        availability_callback_wakes_.load(std::memory_order_relaxed);
    snapshot.workset_waves = workset_waves_.load(std::memory_order_relaxed);
    snapshot.worksets_published_in_waves =
        worksets_published_in_waves_.load(std::memory_order_relaxed);
    snapshot.jobs_published_in_waves =
        jobs_published_in_waves_.load(std::memory_order_relaxed);
    snapshot.workset_wave_ready_transitions =
        workset_wave_ready_transitions_.load(std::memory_order_relaxed);
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

bool QueuedExecutionDb::PublishWorksetWave(
    const PublishWorksetWaveCommand& command,
    PublishWorksetWaveReceipt* receipt_out,
    std::string* error_out) {
    const bool published = ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->PublishWorksetWave(command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
    if (published) {
        ++workset_waves_;
        worksets_published_in_waves_.fetch_add(command.worksets.size());
        jobs_published_in_waves_.fetch_add(
            static_cast<std::uint64_t>(command.expected_job_count));
        if (receipt_out != nullptr
            && receipt_out->ready_workset_availability_changed) {
            ++workset_wave_ready_transitions_;
        }
        RefreshExecutionWorkAvailability();
    }
    return published;
}

std::vector<ClaimedPublishedWorkset>
QueuedExecutionDb::ClaimPublishedWorksetBatch(
    const ClaimPublishedWorksetBatchCommand& command,
    std::string* error_out) {
    auto claimed = ExecuteWrite<std::vector<ClaimedPublishedWorkset>>(
        [this, command, error_out]() {
            return inner_ != nullptr
                ? inner_->ClaimPublishedWorksetBatch(command, error_out)
                : std::vector<ClaimedPublishedWorkset>{};
        },
        {},
        error_out);
    RefreshExecutionWorkAvailability();
    return claimed;
}

std::vector<WorksetDispatchLeaseReceipt>
QueuedExecutionDb::RenewActiveWorksetLeases(
    const RenewActiveWorksetLeasesCommand& command,
    std::string* error_out) {
    return ExecuteWrite<std::vector<WorksetDispatchLeaseReceipt>>(
        [this, command, error_out]() {
            return inner_ != nullptr
                ? inner_->RenewActiveWorksetLeases(command, error_out)
                : std::vector<WorksetDispatchLeaseReceipt>{};
        },
        {},
        error_out);
}

std::optional<ExecutionWorkAvailabilitySnapshot>
QueuedExecutionDb::GetExecutionWorkAvailability(
    std::string* error_out) const {
    return ExecuteRead<
        std::optional<ExecutionWorkAvailabilitySnapshot>>(
        [this, error_out]() {
            return inner_ != nullptr
                ? inner_->GetExecutionWorkAvailability(error_out)
                : std::nullopt;
        },
        std::nullopt,
        error_out);
}

ExecutionWorkAvailabilitySubscription
QueuedExecutionDb::SubscribeExecutionWorkAvailability(
    ExecutionWorkAvailabilityCallback callback) {
    if (!callback) return 0;
    ExecutionWorkAvailabilitySubscription subscription = 0;
    std::optional<ExecutionWorkAvailabilitySnapshot> initial;
    ExecutionWorkAvailabilityCallback initial_callback;
    {
        std::lock_guard lock(availability_mutex_);
        subscription = next_availability_subscription_++;
        if (subscription == 0) subscription = next_availability_subscription_++;
        availability_callbacks_.insert_or_assign(subscription, callback);
        initial = last_execution_work_availability_;
        initial_callback = std::move(callback);
    }
    if (initial.has_value()) {
        try {
            initial_callback(*initial);
            ++availability_callback_wakes_;
        } catch (...) {
        }
    }
    return subscription;
}

void QueuedExecutionDb::UnsubscribeExecutionWorkAvailability(
    ExecutionWorkAvailabilitySubscription subscription) {
    std::lock_guard lock(availability_mutex_);
    availability_callbacks_.erase(subscription);
}

bool QueuedExecutionDb::MarkWorksetActive(
    const MarkWorksetActiveCommand& command,
    WorksetDispatchMutationReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->MarkWorksetActive(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
}

bool QueuedExecutionDb::MarkWorksetDraining(
    const MarkWorksetDrainingCommand& command,
    WorksetDispatchMutationReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->MarkWorksetDraining(
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
    const bool released = ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->ReleaseWorksetDispatch(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
    if (released) RefreshExecutionWorkAvailability();
    return released;
}

bool QueuedExecutionDb::PersistWorkerExecutionEventsBatch(
    const PersistWorkerExecutionEventsBatchCommand& command,
    PersistWorkerExecutionEventsBatchReceipt* receipt_out,
    std::string* error_out) {
    const bool applied = ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->PersistWorkerExecutionEventsBatch(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
    if (applied) RefreshExecutionWorkAvailability();
    return applied;
}

std::vector<ClaimedExecutionFinishedJob>
QueuedExecutionDb::ClaimExecutionFinishedJobsBatch(
    const ClaimExecutionFinishedJobsBatchCommand& command,
    std::string* error_out) {
    auto claimed = ExecuteWrite<std::vector<ClaimedExecutionFinishedJob>>(
        [this, command, error_out]() {
            return inner_ != nullptr
                ? inner_->ClaimExecutionFinishedJobsBatch(command, error_out)
                : std::vector<ClaimedExecutionFinishedJob>{};
        },
        {},
        error_out);
    RefreshExecutionWorkAvailability();
    return claimed;
}

std::vector<InterruptedResultProcessingJob>
QueuedExecutionDb::ListInterruptedResultProcessingJobs(
    std::string* error_out) {
    return ExecuteRead<std::vector<InterruptedResultProcessingJob>>(
        [this, error_out]() {
            return inner_ != nullptr
                ? inner_->ListInterruptedResultProcessingJobs(error_out)
                : std::vector<InterruptedResultProcessingJob>{};
        },
        {},
        error_out);
}

bool QueuedExecutionDb::ResetInterruptedResultProcessing(
    const ResetInterruptedResultProcessingCommand& command,
    ResultProcessingReceipt* receipt_out,
    std::string* error_out) {
    const bool applied = ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->ResetInterruptedResultProcessing(
                    command, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
    if (applied) RefreshExecutionWorkAvailability();
    return applied;
}

bool QueuedExecutionDb::CommitResultFinalizationsBatch(
    const CommitResultFinalizationsBatchCommand& command,
    std::vector<ResultProcessingReceipt>* receipts_out,
    std::string* error_out) {
    const bool applied = ExecuteWrite<bool>(
        [this, command, receipts_out, error_out]() {
            return inner_ != nullptr
                ? inner_->CommitResultFinalizationsBatch(
                    command, receipts_out, error_out)
                : false;
        },
        false,
        error_out);
    if (applied) RefreshExecutionWorkAvailability();
    return applied;
}

std::vector<CommittedJobCancellation>
QueuedExecutionDb::ListUnresolvedJobCancellations(
    std::string* error_out) {
    return ExecuteRead<std::vector<CommittedJobCancellation>>(
        [this, error_out]() {
            return inner_ != nullptr
                ? inner_->ListUnresolvedJobCancellations(error_out)
                : std::vector<CommittedJobCancellation>{};
        },
        {},
        error_out);
}

bool QueuedExecutionDb::MutateJobCancellationsBatch(
    const MutateJobCancellationsBatchCommand& command,
    std::vector<JobCancellationReceipt>* receipts_out,
    std::string* error_out) {
    const bool applied = ExecuteWrite<bool>(
        [this, command, receipts_out, error_out]() {
            return inner_ != nullptr
                ? inner_->MutateJobCancellationsBatch(
                    command, receipts_out, error_out)
                : false;
        },
        false,
        error_out);
    if (applied) RefreshExecutionWorkAvailability();
    return applied;
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

std::optional<ClaimedResultStagingCleanup>
QueuedExecutionDb::ClaimNextResultStagingCleanup(
    const ClaimResultStagingCleanupCommand& command,
    std::string* error_out) {
    return ExecuteWrite<std::optional<ClaimedResultStagingCleanup>>(
        [this, command, error_out]() {
            return inner_ != nullptr
                ? inner_->ClaimNextResultStagingCleanup(command, error_out)
                : std::nullopt;
        }, std::nullopt, error_out);
}

bool QueuedExecutionDb::CompleteResultStagingCleanup(
    const CompleteResultStagingCleanupCommand& command,
    ExecutionDbOperationDisposition* disposition_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, disposition_out, error_out]() {
            return inner_ != nullptr
                ? inner_->CompleteResultStagingCleanup(
                    command, disposition_out, error_out)
                : false;
        }, false, error_out);
}

bool QueuedExecutionDb::GetResultStagingCleanupCount(
    std::int64_t* count_out,
    std::string* error_out) const {
    return ExecuteRead<bool>(
        [this, count_out, error_out]() {
            return inner_ != nullptr
                && inner_->GetResultStagingCleanupCount(count_out, error_out);
        }, false, error_out);
}

bool QueuedExecutionDb::ClearResultStagingCleanupQueue(
    std::int64_t* rows_deleted_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, rows_deleted_out, error_out]() {
            return inner_ != nullptr
                && inner_->ClearResultStagingCleanupQueue(
                    rows_deleted_out, error_out);
        }, false, error_out);
}

bool QueuedExecutionDb::RecoverInterruptedWorksetDispatches(
    RecoverInterruptedWorksetDispatchesReceipt* receipt_out,
    std::string* error_out) {
    const bool recovered = ExecuteWrite<bool>(
        [this, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->RecoverInterruptedWorksetDispatches(
                    receipt_out, error_out)
                : false;
        },
        false,
        error_out);
    if (recovered) RefreshExecutionWorkAvailability();
    return recovered;
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
    const bool requeued = ExecuteWrite<bool>(
        [this, rows_requeued_out, error_out]() {
            return inner_ != nullptr ? inner_->RequeueExpiredExecutionLeases(rows_requeued_out, error_out) : false;
        },
        false,
        error_out);
    if (requeued) RefreshExecutionWorkAvailability();
    return requeued;
}

bool QueuedExecutionDb::RequeueExpiredClaimedExecutionJobs(
    int* rows_requeued_out,
    std::string* error_out) {
    const bool requeued = ExecuteWrite<bool>(
        [this, rows_requeued_out, error_out]() {
            return inner_ != nullptr ? inner_->RequeueExpiredClaimedExecutionJobs(rows_requeued_out, error_out) : false;
        },
        false,
        error_out);
    if (requeued) RefreshExecutionWorkAvailability();
    return requeued;
}

bool QueuedExecutionDb::RequeueClaimedExecutionJob(
    std::int64_t job_id,
    std::string_view claimed_by_token,
    std::string_view message,
    std::string* error_out) {
    const auto token = std::string(claimed_by_token);
    const auto message_value = std::string(message);
    const bool requeued = ExecuteWrite<bool>(
        [this, job_id, token, message_value, error_out]() {
            return inner_ != nullptr
                ? inner_->RequeueClaimedExecutionJob(job_id, token, message_value, error_out)
                : false;
        },
        false,
        error_out);
    if (requeued) RefreshExecutionWorkAvailability();
    return requeued;
}

bool QueuedExecutionDb::RequeueInterruptedExecutionJobs(
    int* rows_requeued_out,
    std::string* error_out) {
    const bool requeued = ExecuteWrite<bool>(
        [this, rows_requeued_out, error_out]() {
            return inner_ != nullptr ? inner_->RequeueInterruptedExecutionJobs(rows_requeued_out, error_out) : false;
        },
        false,
        error_out);
    if (requeued) RefreshExecutionWorkAvailability();
    return requeued;
}

std::optional<ExecutionJobRecord> QueuedExecutionDb::GetExecutionJob(std::int64_t job_id) const {
    return ExecuteRead<std::optional<ExecutionJobRecord>>(
        [this, job_id]() {
            return inner_ != nullptr ? inner_->GetExecutionJob(job_id) : std::nullopt;
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

std::optional<ExecutionJobSetProgressDetails> QueuedExecutionDb::GetJobSetProgress(std::int64_t job_set_id) const {
    return ExecuteRead<std::optional<ExecutionJobSetProgressDetails>>(
        [this, job_set_id]() {
            return inner_ != nullptr ? inner_->GetJobSetProgress(job_set_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<FailedWorkflowWorksetJobRecord>
QueuedExecutionDb::ListFailedWorkflowWorksetJobs(
    std::int64_t workflow_instance_id) const {
    return ExecuteRead<std::vector<FailedWorkflowWorksetJobRecord>>(
        [this, workflow_instance_id]() {
            return inner_ != nullptr
                ? inner_->ListFailedWorkflowWorksetJobs(workflow_instance_id)
                : std::vector<FailedWorkflowWorksetJobRecord>{};
        },
        {});
}

bool QueuedExecutionDb::ApplyWorksetJobReorganization(
    const WorksetJobReorganizationPlan& plan,
    WorksetJobReorganizationReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, &plan, receipt_out, error_out]() {
            return inner_ != nullptr
                ? inner_->ApplyWorksetJobReorganization(plan, receipt_out, error_out)
                : false;
        },
        false,
        error_out);
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

bool QueuedExecutionDb::MarkQueuedJobsSuperseded(
    std::int64_t job_set_id,
    std::int64_t except_job_id,
    std::string* error_out,
    int* rows_superseded_out) {
    const bool applied = ExecuteWrite<bool>(
        [this, job_set_id, except_job_id, error_out, rows_superseded_out]() {
            return inner_ != nullptr
                ? inner_->MarkQueuedJobsSuperseded(job_set_id, except_job_id, error_out, rows_superseded_out)
                : false;
        },
        false,
        error_out);
    if (applied) RefreshExecutionWorkAvailability();
    return applied;
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

void QueuedExecutionDb::AvailabilityWatcherLoop() {
    const auto interval = std::max(
        config_.availability_watch_interval,
        std::chrono::milliseconds(1));
    std::unique_lock lock(availability_mutex_);
    while (!availability_stop_.load(std::memory_order_acquire)) {
        if (availability_cv_.wait_for(
                lock,
                interval,
                [this]() {
                    return availability_stop_.load(
                        std::memory_order_acquire);
                })) {
            break;
        }
        lock.unlock();
        ++availability_watcher_reads_;
        RefreshExecutionWorkAvailability();
        lock.lock();
    }
}

void QueuedExecutionDb::RefreshExecutionWorkAvailability() {
    if (!IsRunning()) return;
    std::string error;
    const auto snapshot = GetExecutionWorkAvailability(&error);
    if (snapshot.has_value()) {
        PublishExecutionWorkAvailability(*snapshot);
    }
}

void QueuedExecutionDb::PublishExecutionWorkAvailability(
    const ExecutionWorkAvailabilitySnapshot& snapshot) {
    std::vector<ExecutionWorkAvailabilityCallback> callbacks;
    {
        std::lock_guard lock(availability_mutex_);
        if (last_execution_work_availability_.has_value()
            && last_execution_work_availability_->generation
                == snapshot.generation) {
            return;
        }
        last_execution_work_availability_ = snapshot;
        ++availability_signal_transitions_;
        callbacks.reserve(availability_callbacks_.size());
        for (const auto& [_, callback] : availability_callbacks_) {
            callbacks.push_back(callback);
        }
    }
    availability_callback_wakes_.fetch_add(
        callbacks.size(),
        std::memory_order_relaxed);
    for (const auto& callback : callbacks) {
        try {
            callback(snapshot);
        } catch (...) {
        }
    }
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
