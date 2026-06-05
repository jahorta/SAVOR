#include "QueuedExecutionDb.h"

#include <utility>

namespace simcore::db::execution {
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

    bool AppendStepInputEvent(const workflow::WorkflowAppendStepInputEventCommand& command, std::string* error_out) override {
        return Execute(command, error_out, [](auto* service, const auto& cmd, auto* err) {
            return service->AppendStepInputEvent(cmd, err);
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
    simcore::db::IExecutionDb* inner,
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
    if (write_lane_) {
        const auto lane = write_lane_->GetTelemetrySnapshot();
        snapshot.write_depth = lane.depth;
        snapshot.write_enqueued = lane.enqueued;
        snapshot.write_rejected = lane.rejected;
        snapshot.write_completed = lane.completed;
        snapshot.write_failed = lane.failed;
    }
    if (read_lane_) {
        const auto lane = read_lane_->GetTelemetrySnapshot();
        snapshot.read_depth = lane.depth;
        snapshot.read_enqueued = lane.enqueued;
        snapshot.read_rejected = lane.rejected;
        snapshot.read_completed = lane.completed;
        snapshot.read_failed = lane.failed;
    }
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
    return ExecuteWrite<bool>(
        [this, command, workflow_instance_id_out, error_out]() {
            return inner_ != nullptr
                ? inner_->CreateWorkflowInstance(command, workflow_instance_id_out, error_out)
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

std::optional<ExecutionJobRecord> QueuedExecutionDb::GetJob(std::int64_t job_id) const {
    return ExecuteRead<std::optional<ExecutionJobRecord>>(
        [this, job_id]() {
            return inner_ != nullptr ? inner_->GetJob(job_id) : std::nullopt;
        },
        std::nullopt);
}

std::optional<ExecutionJobSetProgressDetails> QueuedExecutionDb::GetJobSetProgress(std::int64_t job_set_id) const {
    return ExecuteRead<std::optional<ExecutionJobSetProgressDetails>>(
        [this, job_set_id]() {
            return inner_ != nullptr ? inner_->GetJobSetProgress(job_set_id) : std::nullopt;
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
Result QueuedExecutionDb::ExecuteRead(Fn&& fn, Result fallback, std::string* error_out) const {
    return core::QueuedDbExecutor::ExecuteQueued<Result>(
        *read_lane_,
        sqlite_call_mtx_,
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

template <typename Result, typename Fn>
Result QueuedExecutionDb::ExecuteWrite(Fn&& fn, Result fallback, std::string* error_out) const {
    return core::QueuedDbExecutor::ExecuteQueued<Result>(
        *write_lane_,
        sqlite_call_mtx_,
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

} // namespace simcore::db::execution
