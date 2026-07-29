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
#include "AdapterChainOrchestrator.h"
#include "WorkflowOrchestration.h"

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
    std::chrono::milliseconds terminal_repair_interval{ 250 };
    std::size_t ready_scan_limit = 2048;
    std::size_t terminal_scan_limit = 64;
    std::size_t max_active_materialized_workflows = 30;
    int successor_step_priority_boost = 10;
    std::chrono::milliseconds input_timeout{ 2000 };
    int input_timeout_retries = 1;
    // Production supplies worker item credits. A null provider retains the
    // legacy active-workflow throttle for focused compatibility tests.
    std::function<std::size_t()> available_item_credits;
    // The production coordinator pair shares this closed-by-default source.
    // It opens only after exact worker/catalog negotiation succeeds.
    std::shared_ptr<CoordinatorItemCreditSource> item_credit_source;
};

struct TerminalWorkflowStepNotification {
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
    std::int64_t input_complete_count = 0;
    std::int64_t input_timeout_count = 0;
    std::int64_t terminal_input_failure_count = 0;
    std::int64_t last_input_latency_ms = 0;
    std::int64_t materialization_count = 0;
    std::int64_t materialization_failure_count = 0;
    std::int64_t last_materialization_latency_ms = 0;
    std::int64_t max_materialization_latency_ms = 0;
    std::int64_t terminal_scan_count = 0;
    std::int64_t terminal_step_count = 0;
    std::int64_t terminal_empty_step_count = 0;
    std::int64_t terminal_failed_step_count = 0;
    std::int64_t terminal_completed_step_count = 0;
    std::int64_t transition_advanced_count = 0;
    std::int64_t workflow_completed_count = 0;
    std::int64_t workflow_failed_count = 0;
    std::int64_t targeted_terminal_notification_count = 0;
    std::int64_t targeted_terminal_advancement_count = 0;
};

class WorkflowCoordinatorService {
public:
    using EventLineCallback = std::function<void(const std::string&)>;

    WorkflowCoordinatorService(
        savor::db::IExecutionDb* execution_db,
        const savor::db::execution::programdb::ProgramKindRegistry* program_kind_registry,
        WorkflowCoordinatorConfig config = {},
        EventLineCallback event_line_callback = {},
        StepCompletionGateService* step_completion_gate = nullptr,
        savor::db::IAuthoringDb* authoring_db = nullptr);
    ~WorkflowCoordinatorService();

    WorkflowCoordinatorService(const WorkflowCoordinatorService&) = delete;
    WorkflowCoordinatorService& operator=(const WorkflowCoordinatorService&) = delete;

    bool Start(std::string* error_out = nullptr);
    void Stop();
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] WorkflowCoordinatorTelemetry SnapshotTelemetry() const;
    bool PublishTerminalCommit(
        const TerminalWorkflowStepNotification& notification);

private:
    struct StepInputAggregationStatus {
        bool input_complete = false;
        bool terminal_failure_ready = false;
        bool timed_out = false;
        std::optional<std::int64_t> input_latency_ms;
    };

    struct StepAssemblyContext {
        std::int64_t workflow_instance_id = 0;
        std::string step_key;
        std::vector<std::string> required_inputs;
        std::unordered_map<std::string, std::string> pending_request_ids_by_source;
        std::unordered_set<std::string> ready_sources;
        std::chrono::steady_clock::time_point started_at{};
        std::chrono::steady_clock::time_point deadline{};
        int timeout_retries = 0;
        bool input_complete_emitted = false;
        bool async_fragment_simulated = false;
    };

    void Loop();
    bool AdvanceAvailableWork();
    bool PollReadyStepsFromDb();
    bool ReconcileTargetedTerminalNotifications();
    bool ReconcileTerminalWorkflowSteps();
    bool AdvanceTerminalSnapshot(
        const WorkflowStepTerminalSnapshot& snapshot,
        const char* failure_stage);
    bool ProcessReadyWorkflowStep(const WorkflowReadyStepRecord& step);
    bool CompleteNoWorkWorkflowStep(const WorkflowReadyStepRecord& step);
    std::optional<programdb::WorkflowStepScheduleResult> ScheduleReadyStep(const WorkflowReadyStepRecord& step) const;
    bool MaterializeWorkflowStep(const WorkflowReadyStepRecord& step);
    StepInputAggregationStatus EvaluateInputAggregation(
        const WorkflowReadyStepRecord& step,
        std::chrono::steady_clock::time_point now);
    bool SubmitInputFragment(
        const WorkflowReadyStepRecord& step,
        const std::string& source_key,
        const std::optional<std::string>& request_id,
        std::chrono::steady_clock::time_point now);
    std::string InputContextKey(const WorkflowReadyStepRecord& step) const;
    bool IsSeedProbePilotStep(const WorkflowReadyStepRecord& step) const;
    std::vector<std::string> RequiredInputsFor(const WorkflowReadyStepRecord& step) const;
    bool EmitInputEvent(
        const WorkflowReadyStepRecord& step,
        const std::string& event_kind,
        const std::optional<std::string>& source_key,
        const std::optional<std::string>& request_id,
        const std::optional<std::string>& message);
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
    std::unique_ptr<StepCompletionGateService> owned_step_completion_gate_;
    StepCompletionGateService* step_completion_gate_ = nullptr;
    AdapterChainOrchestrator adapter_chain_orchestrator_;

    std::atomic<bool> stop_{ false };
    std::atomic<bool> running_{ false };
    std::thread worker_thread_;
    std::chrono::steady_clock::time_point next_terminal_repair_at_{};
    mutable std::mutex wait_mtx_;
    std::condition_variable wait_cv_;
    mutable std::mutex terminal_notification_mtx_;
    std::map<
        std::tuple<std::uint64_t, std::int64_t, std::int64_t>,
        TerminalWorkflowStepNotification>
        terminal_notifications_;
    std::atomic<std::uint64_t> terminal_notification_generation_{ 0 };
    std::unordered_set<std::int64_t> seen_workflow_instance_ids_;
    std::unordered_map<std::string, StepAssemblyContext> input_contexts_;

    std::atomic<std::int64_t> ready_scan_count_{ 0 };
    std::atomic<std::int64_t> ready_steps_seen_{ 0 };
    std::atomic<std::int64_t> workflow_created_seen_count_{ 0 };
    std::atomic<std::int64_t> last_ready_scan_latency_ms_{ 0 };
    std::atomic<std::int64_t> active_materialized_workflow_count_{ 0 };
    std::atomic<std::int64_t> materialization_throttle_count_{ 0 };
    std::atomic<std::int64_t> input_complete_count_{ 0 };
    std::atomic<std::int64_t> input_timeout_count_{ 0 };
    std::atomic<std::int64_t> terminal_input_failure_count_{ 0 };
    std::atomic<std::int64_t> last_input_latency_ms_{ 0 };
    std::atomic<std::int64_t> materialization_count_{ 0 };
    std::atomic<std::int64_t> materialization_failure_count_{ 0 };
    std::atomic<std::int64_t> last_materialization_latency_ms_{ 0 };
    std::atomic<std::int64_t> max_materialization_latency_ms_{ 0 };
    std::atomic<std::int64_t> terminal_scan_count_{ 0 };
    std::atomic<std::int64_t> terminal_step_count_{ 0 };
    std::atomic<std::int64_t> terminal_empty_step_count_{ 0 };
    std::atomic<std::int64_t> terminal_failed_step_count_{ 0 };
    std::atomic<std::int64_t> terminal_completed_step_count_{ 0 };
    std::atomic<std::int64_t> transition_advanced_count_{ 0 };
    std::atomic<std::int64_t> workflow_completed_count_{ 0 };
    std::atomic<std::int64_t> workflow_failed_count_{ 0 };
    std::atomic<std::int64_t> targeted_terminal_notification_count_{ 0 };
    std::atomic<std::int64_t> targeted_terminal_advancement_count_{ 0 };
};

} // namespace savor::db::execution::workflow
