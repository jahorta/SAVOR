#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../IExecutionDb.h"
#include "../ProgramDB/ProgramKindRegistry.h"
#include "WorkflowOrchestration.h"
#include "WorkflowStepSettlementGate.h"

namespace savor::db {
struct IAuthoringDb;
}

namespace savor::db::execution::workflow {

class CoordinatorItemCreditSource {
public:
    void Open(std::size_t total_credits) noexcept;
    void SetCapacity(std::size_t total_credits) noexcept;
    void SetExternalUsage(std::size_t used_credits) noexcept;
    [[nodiscard]] std::size_t AvailableCredits() const noexcept;
    [[nodiscard]] bool TryReserve(std::size_t credits = 1) noexcept;
    void ReleaseReservations(std::size_t credits = 1) noexcept;
    void Close() noexcept;
    [[nodiscard]] bool IsOpen() const noexcept;

private:
    std::atomic<bool> open_{ false };
    std::atomic<std::size_t> total_credits_{ 0 };
    std::atomic<std::size_t> external_usage_{ 0 };
    std::atomic<std::size_t> reservations_{ 0 };
};

struct WorkflowCoordinatorConfig {
    bool workflow_enabled = true;
    bool strict_smoke_terminal_on_failure = false;
    std::chrono::milliseconds poll_interval{ 50 };
    // Targeted commit notifications are the normal advancement path. This
    // slower scan remains the crash-recovery authority for notifications lost
    // across process boundaries or restarts.
    std::chrono::milliseconds settlement_repair_interval{ 250 };
    std::chrono::milliseconds descriptor_repair_interval{ 1000 };
    std::size_t ready_scan_limit = 2048;
    std::size_t settlement_scan_limit = 64;
    std::size_t max_active_materialized_workflows = 30;
    int successor_step_priority_boost = 10;
    std::shared_ptr<CoordinatorItemCreditSource> item_credit_source;
    // In-process hint only. The execution DB remains authoritative and the
    // claim scheduler retains its external-writer fallback poll.
};

struct SettledWorkflowStepNotification {
    std::uint64_t commit_sequence = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t job_id = 0;
};

struct WorkflowCoordinatorTelemetry {
    std::int64_t ready_scan_count = 0;
    std::int64_t ready_steps_seen = 0;
    std::int64_t workflow_created_seen_count = 0;
    std::int64_t last_ready_scan_latency_ms = 0;
    std::int64_t active_materialized_workflow_count = 0;
    std::int64_t materialization_throttle_count = 0;
    std::int64_t materialization_count = 0;
    std::int64_t materialization_failure_count = 0;
    std::int64_t last_materialization_latency_ms = 0;
    std::int64_t max_materialization_latency_ms = 0;
    std::int64_t continuation_count = 0;
    std::int64_t continuation_added_work_count = 0;
    std::int64_t continuation_failure_count = 0;
    std::int64_t settlement_scan_count = 0;
    std::int64_t settled_step_count = 0;
    std::int64_t settled_empty_step_count = 0;
    std::int64_t failed_step_count = 0;
    std::int64_t completed_step_count = 0;
    std::int64_t transition_advanced_count = 0;
    std::int64_t workflow_completed_count = 0;
    std::int64_t workflow_failed_count = 0;
    std::int64_t targeted_settlement_notification_count = 0;
    std::int64_t targeted_settlement_advancement_count = 0;
    std::int64_t descriptor_unavailable_count = 0;
    std::int64_t descriptor_resumed_count = 0;
};

class WorkflowCoordinatorService {
public:
    using EventLineCallback = std::function<void(const std::string&)>;

    WorkflowCoordinatorService(
        savor::db::IExecutionDb* execution_db,
        const savor::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
        WorkflowCoordinatorConfig config = {},
        EventLineCallback event_line_callback = {},
        StepSettlementGateService* step_completion_gate = nullptr,
        savor::db::IAuthoringDb* authoring_db = nullptr);
    ~WorkflowCoordinatorService();

    WorkflowCoordinatorService(const WorkflowCoordinatorService&) = delete;
    WorkflowCoordinatorService& operator=(const WorkflowCoordinatorService&) = delete;

    bool Start(std::string* error_out = nullptr);
    void Stop();
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] WorkflowCoordinatorTelemetry SnapshotTelemetry() const;
    bool PublishSettlementCommit(
        const SettledWorkflowStepNotification& notification);

private:
    void Loop();
    bool AdvanceAvailableWork();
    bool PollReadyStepsFromDb();
    bool ReconcileTargetedTerminalNotifications();
    bool ReconcileSettledWorkflowSteps();
    bool ReconcileDescriptorAvailability();
    bool AdvanceSettlementSnapshot(
        const WorkflowStepSettlementSnapshot& snapshot,
        const char* failure_stage);
    bool ProcessReadyWorkflowStep(const WorkflowReadyStepRecord& step);
    std::optional<programdb::ProgramJobMaterializationContext>
    BuildMaterializationContext(
        const WorkflowReadyStepRecord& step,
        std::string* error_out) const;
    std::optional<programdb::WorkflowStepScheduleResult> ScheduleReadyStep(
        const WorkflowReadyStepRecord& step,
        std::string* error_out) const;
    bool MaterializeWorkflowStep(const WorkflowReadyStepRecord& step);
    void EmitWorkflowFailureEvent(
        const WorkflowReadyStepRecord& step,
        const std::string& stage,
        const std::string& reason) const;
    void MaybeTerminalFailStepInStrictSmokeMode(
        const WorkflowReadyStepRecord& step,
        const std::string& requested_by,
        const std::string& failure_reason) const;
    void EmitEventLine(const std::string& line) const;

    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IAuthoringDb* authoring_db_ = nullptr;
    const savor::db::execution::programdb::ProgramKindRegistry* program_kind_registry_ = nullptr;
    WorkflowCoordinatorConfig config_{};
    EventLineCallback event_line_callback_;
    std::unique_ptr<StepSettlementGateService> owned_step_completion_gate_;
    StepSettlementGateService* step_completion_gate_ = nullptr;
    std::atomic<bool> stop_{ false };
    std::atomic<bool> running_{ false };
    std::thread worker_thread_;
    std::chrono::steady_clock::time_point next_settlement_repair_at_{};
    std::chrono::steady_clock::time_point next_descriptor_repair_at_{};
    std::uint64_t observed_registry_generation_ = 0;
    mutable std::mutex wait_mtx_;
    std::condition_variable wait_cv_;
    mutable std::mutex terminal_notification_mtx_;
    std::map<
        std::tuple<std::uint64_t, std::int64_t, std::int64_t>,
        SettledWorkflowStepNotification>
        terminal_notifications_;
    std::atomic<std::uint64_t> terminal_notification_generation_{ 0 };
    std::unordered_set<std::int64_t> seen_workflow_instance_ids_;

    std::atomic<std::int64_t> ready_scan_count_{ 0 };
    std::atomic<std::int64_t> ready_steps_seen_{ 0 };
    std::atomic<std::int64_t> workflow_created_seen_count_{ 0 };
    std::atomic<std::int64_t> last_ready_scan_latency_ms_{ 0 };
    std::atomic<std::int64_t> active_materialized_workflow_count_{ 0 };
    std::atomic<std::int64_t> materialization_throttle_count_{ 0 };
    std::atomic<std::int64_t> materialization_count_{ 0 };
    std::atomic<std::int64_t> materialization_failure_count_{ 0 };
    std::atomic<std::int64_t> last_materialization_latency_ms_{ 0 };
    std::atomic<std::int64_t> max_materialization_latency_ms_{ 0 };
    std::atomic<std::int64_t> continuation_count_{ 0 };
    std::atomic<std::int64_t> continuation_added_work_count_{ 0 };
    std::atomic<std::int64_t> continuation_failure_count_{ 0 };
    std::atomic<std::int64_t> settlement_scan_count_{ 0 };
    std::atomic<std::int64_t> settled_step_count_{ 0 };
    std::atomic<std::int64_t> settled_empty_step_count_{ 0 };
    std::atomic<std::int64_t> failed_step_count_{ 0 };
    std::atomic<std::int64_t> completed_step_count_{ 0 };
    std::atomic<std::int64_t> transition_advanced_count_{ 0 };
    std::atomic<std::int64_t> workflow_completed_count_{ 0 };
    std::atomic<std::int64_t> workflow_failed_count_{ 0 };
    std::atomic<std::int64_t> targeted_settlement_notification_count_{ 0 };
    std::atomic<std::int64_t> targeted_settlement_advancement_count_{ 0 };
    std::atomic<std::int64_t> descriptor_unavailable_count_{ 0 };
    std::atomic<std::int64_t> descriptor_resumed_count_{ 0 };
};

} // namespace savor::db::execution::workflow
