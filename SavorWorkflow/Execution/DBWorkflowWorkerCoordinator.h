#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Runner/Parallel/PRTypes.h"
#include "Runner/Runtime/RuntimeTypes.h"
#include "Runner/Runtime/Worksets/WorksetTypes.h"
#include "../Worker/ProcessWorker.h"
#include "../Worker/TSQueue.h"
#include "../Worker/WorkerStatusRegistry.h"
#include "Execution/IExecutionDb.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/Workflow/AdapterChainOrchestrator.h"
#include "Execution/Workflow/WorkflowCoordinatorService.h"
#include "Execution/Workflow/WorkflowOrchestration.h"
#include "State/IStateDb.h"
#include "WorkflowCoordinatorBridge.h"
#include "WorkflowIntegrationMode.h"
#include "WorkflowSchedulerAdapter.h"
#include "StepInputAggregationService.h"
#include "JobMaterializationService.h"

namespace savor::runner::parallel::savordb {

struct CoordinatorWorkerCapabilityPreflightResult {
    // The hook owns any launch/session negotiation needed to make this true.
    bool process_ready = false;
    savor::runtime::WorkerCapabilityMask capabilities = 0;
    std::optional<savor::runtime::WorkerRuntimeManifest> runtime_manifest;
    std::string error;
};

struct DBWorkflowWorkerCoordinatorConfig {
    using RuntimeSlotPreparer = std::function<bool(
        size_t,
        const DBWorkflowWorkerCoordinatorConfig&,
        std::filesystem::path*,
        std::string*)>;
    using WorkerCapabilityPreflight = std::function<CoordinatorWorkerCapabilityPreflightResult(
        size_t,
        const DBWorkflowWorkerCoordinatorConfig&,
        const std::shared_ptr<savor::ProcessWorker>&)>;
    using TerminalCommitCallback = std::function<void(
        const savor::db::execution::workflow::
            TerminalWorkflowStepNotification&)>;
    using ItemAuthorityLostCallback = std::function<void(
        std::int64_t,
        std::string_view,
        savor::db::ExecutionJobLeaseRenewalDisposition)>;
    using WorkerItemUsageProvider =
        std::function<CoordinatorItemCapacitySnapshot()>;
    using WorksetDefinitionBuilder = std::function<
        std::optional<savor::runtime::WorkerWorksetDefinition>(
            std::size_t,
            const std::vector<ClaimedJobRecord>&,
            const savor::runtime::WorkerRuntimeManifest&,
            std::string*)>;
    using WorksetTerminalDecoder = std::function<
        std::optional<savor::PRResult>(
            const ClaimedJobRecord&,
            const savor::wrms::WorksetItemTerminalPayload&,
            std::string*)>;

    size_t desired_workers = 1;
    uint32_t controller_sleep_ms = 5;
    uint32_t worker_start_timeout_ms = 20000;
    uint32_t worker_start_retry_backoff_ms = 5000;
    uint32_t max_worker_start_attempts = 3;
    uint32_t max_concurrent_worker_starts = 2;
    std::string worker_exe_path;
    std::string iso_path;
    std::string dolphin_base_dir;
    std::string worker_dir_root;
    std::string worker_binary_runtime_root;
    bool visual_workers = false;
    bool visual_debug_workers = false;
    bool auto_resume_visual_workers = false;
    std::string visual_screenshot_dir;
    // Slice 7 sets this to the canonical exact nine-module catalog hash.
    // Until then the production partial manifest cannot open the data plane.
    std::string expected_catalog_sha256;
    std::string expected_runtime_profile_sha256;
    std::string expected_dependency_manifest_sha256;
    RuntimeSlotPreparer runtime_slot_preparer;
    // Required for Start(). The coordinator does not touch DB-facing work
    // until one process-ready result advertises WorksetDispatch and the exact
    // configured production catalog.
    WorkerCapabilityPreflight worker_capability_preflight;
    TerminalCommitCallback terminal_commit_callback;
    ItemAuthorityLostCallback item_authority_lost_callback;
    WorkerItemUsageProvider worker_item_usage_provider;
    WorksetDefinitionBuilder workset_definition_builder;
    WorksetTerminalDecoder workset_terminal_decoder;
    std::size_t workset_lookahead_items = 64;
    std::size_t workset_lookahead_bytes = 64ull * 1024ull * 1024ull;
    std::shared_ptr<
        savor::db::execution::workflow::CoordinatorItemCreditSource>
        item_credit_source;
};

enum class CoordinatorStartStatus {
    NotStarted = 0,
    Started,
    CapabilityPreflightUnavailable,
    CapabilityPreflightFailed,
    WorksetDispatchUnavailable,
    WorksetCatalogUnavailable,
    InteractiveVisualDebugUnavailable,
};

struct CoordinatorStartResult {
    CoordinatorStartStatus status = CoordinatorStartStatus::NotStarted;
    size_t ready_workset_capable_workers = 0;
    bool non_retryable = false;
    std::string error;

    [[nodiscard]] bool started() const noexcept {
        return status == CoordinatorStartStatus::Started;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return started();
    }
};

struct WorkflowCoordinatorTelemetry {
    std::int64_t ready_scan_count = 0;
    std::int64_t ready_steps_enqueued = 0;
    std::int64_t last_ready_scan_latency_ms = 0;
    std::int64_t max_ready_queue_depth = 0;
    std::int64_t input_complete_count = 0;
    std::int64_t input_timeout_count = 0;
    std::int64_t terminal_input_failure_count = 0;
    std::int64_t last_input_latency_ms = 0;
    std::int64_t materialization_count = 0;
    std::int64_t last_materialization_latency_ms = 0;
    std::int64_t max_materialization_latency_ms = 0;
    std::int64_t stale_claim_count = 0;
    std::int64_t dispatch_attempt_count = 0;
    std::int64_t dispatch_success_count = 0;
    std::int64_t dispatch_miss_count = 0;
    std::int64_t dispatch_miss_rate_basis_points = 0;
    std::int64_t progress_batch_count = 0;
    std::int64_t max_progress_batch_size = 0;
    std::int64_t results_received_count = 0;
    std::int64_t workflow_created_signal_count = 0;
    std::int64_t materialization_failure_count = 0;
    std::int64_t payload_materialization_failure_count = 0;
    std::int64_t claim_attempt_count = 0;
    std::int64_t claimed_job_count = 0;
    std::int64_t clean_zero_claim_count = 0;
    std::int64_t claim_error_count = 0;
    std::int64_t partial_claim_count = 0;
    bool no_jobs_available = false;
    struct WorkerEfficiency {
        std::int64_t worker_id = 0;
        std::int64_t dispatch_success_count = 0;
        std::int64_t program_kind_switch_count = 0;
    };
    std::vector<WorkerEfficiency> workers;
};

struct CoordinatorWarningSnapshot {
    std::uint64_t sequence = 0;
    std::int64_t worker_id = 0;
    std::int64_t job_id = 0;
    std::int64_t observed_mono_ns = 0;
    std::string message;
    std::string detail;
};

enum class VisualReplayRuntimeState {
    Idle = 0,
    QueuedStartup,
    LaunchingWorker,
    AttachReady,
    Active,
    Stopping,
    Finished,
    Failed,
};

struct VisualDebugReplaySnapshot {
    bool active = false;
    std::uint64_t session_id = 0;
    std::int64_t job_id = 0;
    std::int64_t worker_id = 0;
    VisualReplayRuntimeState state = VisualReplayRuntimeState::Idle;
    std::string detail;
    bool controls_enabled = false;
};

class DBWorkflowWorkerCoordinator {
public:
    using ReadyStepPersistFn = std::function<void(const WorkflowReadyStep&, const ScheduledJobSet&)>;
    using ProgressCallback = std::function<void(const savor::PRProgress&)>;
    using ResultCallback = std::function<void(const savor::PRResult&)>;
    using ResultMapEventCallback = std::function<void(const std::string&)>;

    DBWorkflowWorkerCoordinator(
        savor::db::IExecutionDb* execution_db,
        DBWorkflowWorkerCoordinatorConfig worker_cfg,
        CoordinatorIntegrationConfig integration_cfg,
        const savor::db::execution::programdb::ProgramKindRegistry* program_kind_registry = nullptr,
        ReadyStepPersistFn persist_materialization_fn = {},
        savor::db::execution::workflow::StepCompletionGateService* step_completion_gate = nullptr,
        savor::db::IStateDb* state_db = nullptr);

    DBWorkflowWorkerCoordinator(
        savor::db::IExecutionDb* execution_db,
        DBWorkflowWorkerCoordinatorConfig worker_cfg,
        CoordinatorIntegrationConfig integration_cfg,
        WorkflowSchedulerAdapter::ScheduleFn workflow_schedule_fn,
        ReadyStepPersistFn persist_materialization_fn = {},
        const savor::db::execution::programdb::ProgramKindRegistry* program_kind_registry = nullptr,
        savor::db::execution::workflow::StepCompletionGateService* step_completion_gate = nullptr,
        savor::db::IStateDb* state_db = nullptr);

    ~DBWorkflowWorkerCoordinator();

    CoordinatorStartResult Start();
    void Stop();
    [[nodiscard]] CoordinatorStartResult SnapshotStartResult() const;
    [[nodiscard]] bool IsDataPlaneEnabled() const noexcept;
    [[nodiscard]] size_t ReadyWorksetCapableWorkerCount() const;
    [[nodiscard]] size_t ReadyItemCreditCapacity() const;
    [[nodiscard]] std::shared_ptr<
        savor::db::execution::workflow::CoordinatorItemCreditSource>
        ItemCreditSource() const;

    void SetPaused(bool paused);
    bool IsPaused() const;
    void SetDesiredWorkerCount(size_t desired_workers);

    void SetWorkflowMaterializationCallback(WorkflowCoordinatorBridge::MaterializationCallback callback);
    void SetWorkflowTerminalCallback(WorkflowCoordinatorBridge::TerminalCallback callback);
    void SetWorkflowCreatedCallback(WorkflowCoordinatorBridge::WorkflowCreatedCallback callback);

    // External path for terminal notifications coming from job/job_set execution.
    bool PublishTerminalJobSet(const TerminalJobSetSignal& signal);
    bool PublishWorkflowCreated(const WorkflowCreatedSignal& signal);

    // Manual injection hook for tests or explicit push-based materialization pipelines.
    void EnqueueReadyStep(const WorkflowReadyStep& step);
    std::optional<ScheduledJobSet> MaterializeWorkflowStep(const WorkflowReadyStep& step);
    bool SendJobToWorker(
        size_t worker_idx,
        uint64_t job_id,
        const savor::PSJob& job);
    size_t ActiveWorkerCount() const;
    void SetProgressCallback(ProgressCallback callback);
    void SetResultCallback(ResultCallback callback);
    void SetResultMapEventCallback(ResultMapEventCallback callback);
    void EnqueueProgressForTest(const savor::PRProgress& progress);
    void EnqueueResultForTest(const savor::PRResult& result);
    static bool MaterializeWorkerRuntimeForTest(
        size_t worker_idx,
        const DBWorkflowWorkerCoordinatorConfig& worker_cfg,
        std::filesystem::path* runtime_worker_exe_out,
        std::string* error_out = nullptr);

    PRStatus SnapshotStatus() const;
    WorkflowCoordinatorTelemetry SnapshotTelemetry() const;
    std::vector<CoordinatorWarningSnapshot> SnapshotWarnings() const;
    std::vector<WorkerSnapshot> SnapshotWorkers() const;
    bool SetWorkerVisualSurface(size_t worker_idx, uint64_t render_widget_handle, std::string host_events_pipe_name);
    bool StartVisualDebugReplay(
        std::int64_t job_id,
        uint64_t render_widget_handle,
        std::string host_events_pipe_name,
        std::string* error_out = nullptr);
    bool StopVisualDebugReplay();
    bool PauseVisualDebugReplayEmulation();
    bool ResumeVisualDebugReplayEmulation();
    bool StepVisualDebugReplayVm();
    VisualDebugReplaySnapshot SnapshotVisualDebugReplay() const;
    std::vector<std::string> TakeVisualDebugLogLines();

private:
    struct WorkerSlot;
    using WorkerSlotPtr = std::shared_ptr<WorkerSlot>;

    struct WorkerSlot {
        mutable std::mutex mtx;
        mutable std::mutex workset_submission_mtx;
        size_t id = 0;
        std::shared_ptr<savor::ProcessWorker> worker;
        // Changes whenever this logical slot receives a new child process.
        // Callback ingress from an older process is stale evidence and must
        // never mutate or fail-close the replacement.
        std::uint64_t process_generation = 0;
        std::atomic<bool> ready{ false };
        savor::runtime::WorkerCapabilityMask capabilities = 0;
        std::optional<savor::runtime::WorkerRuntimeManifest> runtime_manifest;
        bool start_attempted = false;
        bool startup_in_progress = false;
        uint32_t start_attempts = 0;
        bool start_retry_exhausted_logged = false;
        std::chrono::steady_clock::time_point next_start_after{};
        std::string last_start_error;
        std::thread startup_thread;
        std::optional<uint64_t> in_flight_job_id;
        std::optional<std::uint64_t> active_workset_id;
        std::optional<std::uint64_t> staged_workset_id;
        std::unordered_set<std::uint64_t> retained_workset_ids;
        std::unordered_map<std::uint64_t, std::size_t>
            retained_workset_item_counts;
        std::unordered_set<std::uint64_t> failed_closed_workset_ids;
        std::uint64_t last_workset_outbound_sequence = 0;
        std::uint64_t last_workset_terminal_order = 0;
        bool workset_admission_blocked = false;
        std::optional<std::int32_t> loaded_program_kind;
        std::optional<std::string> loaded_program_runtime_affinity_key;
        std::optional<std::string> loaded_savestate_affinity_key;
        std::optional<std::string> loaded_workset_execution_key;
        std::int64_t dispatch_success_count = 0;
        std::int64_t program_kind_switch_count = 0;
        std::chrono::steady_clock::time_point in_flight_started_at{};
        std::chrono::steady_clock::time_point last_worker_contact_at{};
        std::chrono::steady_clock::time_point dead_in_flight_observed_at{};
        uint64_t visual_render_widget_handle = 0;
        std::string visual_host_events_pipe_name;
    };
    struct WorkerVisualSurface {
        uint64_t render_widget_handle = 0;
        std::string host_events_pipe_name;
    };
    struct DispatchableWorkerInfo {
        size_t worker_idx = 0;
        std::optional<std::int32_t> loaded_program_kind;
        std::optional<std::string> loaded_program_runtime_affinity_key;
        std::optional<std::string> loaded_savestate_affinity_key;
        std::optional<std::string> loaded_workset_execution_key;
    };
    struct DispatchedJobContext {
        WorkflowReadyStep step;
        std::int64_t job_set_id = 0;
    };
    enum class WorksetEventKind {
        State = 0,
        ItemStarted,
        ItemProgress,
        ItemTerminal,
        Credits,
        Summary,
    };
    struct WorksetEventIngress {
        std::size_t worker_idx = 0;
        std::uint64_t worker_generation = 0;
        WorksetEventKind kind = WorksetEventKind::State;
        savor::wrms::WorksetStatePayload state;
        savor::wrms::WorksetItemStartedPayload started;
        savor::wrms::InvocationProgressPayload progress;
        savor::wrms::WorksetItemTerminalPayload terminal;
        savor::wrms::WorksetCreditsPayload credits;
        savor::wrms::WorksetSummaryPayload summary;
    };
    struct DispatchedWorksetItemContext {
        std::size_t worker_idx = 0;
        std::uint64_t workset_id = 0;
        std::uint64_t item_id = 0;
        std::uint32_t item_ordinal = 0;
        std::uint64_t invocation_id = 0;
        std::uint64_t attempt_id = 0;
        bool start_persisted = false;
        bool authority_lost = false;
        ClaimedJobRecord claimed;
    };

    void WorkerJobCoordinatorLoop();
    void WorkerLifecycleCoordinatorLoop();
    CoordinatorWorkerCapabilityPreflightResult RunWorkerCapabilityPreflightForSlot(
        const WorkerSlotPtr& slot);
    CoordinatorWorkerCapabilityPreflightResult PreflightWorkerSlot(
        const WorkerSlotPtr& slot);
    void DrainProgressLoop();
    void DrainResultsLoop();
    void RecoverDeadInFlightWorkers();
    void ProcessReadyWorkflowStep(const WorkflowReadyStep& step);
    void ReconcileWorkerPool();
    bool StartWorkerSlot(WorkerSlotPtr slot);
    void CompleteWorkerSlotStartup(
        size_t worker_idx,
        uint32_t attempt,
        bool ready,
        savor::runtime::WorkerCapabilityMask capabilities,
        std::optional<savor::runtime::WorkerRuntimeManifest> runtime_manifest,
        const std::string& error,
        bool retryable);
    std::shared_ptr<savor::ProcessWorker> ResetWorkerSlotRuntime(WorkerSlot& slot);
    void StopWorkerSlot(
        WorkerSlotPtr slot,
        bool preserve_workset_event_context = false);
    void EmitShutdownPhase(const std::string& phase, const std::string& detail = {}) const;
    void RecordWorkerContactLocked(WorkerSlot& slot, std::chrono::steady_clock::time_point observed_at);
    WorkerSlotPtr MakeWorkerSlot(size_t worker_idx);
    WorkerSlotPtr GetWorkerSlot(size_t worker_idx) const;
    std::vector<WorkerSlotPtr> CopyWorkerSlots() const;
    bool TryRecordWorkerContactFromProgress(std::size_t worker_id, std::uint64_t job_id, std::chrono::steady_clock::time_point observed_at);
    bool PrepareRuntimeSlotForWorker(size_t worker_idx, std::filesystem::path* runtime_worker_exe_out, std::string* error_out);
    std::vector<DispatchableWorkerInfo> CollectDispatchableWorkers();
    std::size_t CountActiveInFlightItems() const;
    std::size_t ReadyCoordinatorBufferCapacity() const;
    void RefreshSharedItemCredits();
    void ReleaseWorkerByResult(const savor::PRResult& result);
    void PollReadyStepsFromDb();
    void MaintainMaterializerClaims(std::chrono::steady_clock::time_point now);
    bool TryDequeueReadyStep(WorkflowReadyStep* step_out);
    std::string ReadyDedupKey(std::int64_t workflow_step_id) const;
    bool CompleteNoWorkWorkflowStep(const WorkflowReadyStep& step) const;
    std::optional<ScheduledJobSet> MaterializeWorkflowStepInternal(const WorkflowReadyStep& step);
    void HandlePayloadMaterializationFailures();
    void HandleItemAuthorityLost(
        const ClaimLeaseMaintenanceResult::LostAuthority& lost);
    bool DispatchClaimedWorksetToWorker(
        size_t worker_idx,
        const std::vector<ClaimedJobRecord>& claimed_jobs);
    bool ValidateClaimedWorksetAuthority(
        const std::vector<ClaimedJobRecord>& claimed_jobs);
    bool ValidateBuiltWorkset(
        const savor::runtime::WorkerWorksetDefinition& workset,
        const std::vector<ClaimedJobRecord>& claimed_jobs,
        const savor::runtime::WorkerRuntimeManifest& manifest,
        std::string* error_out) const;
    void ConfigureWorkerCallbacks(
        std::size_t worker_idx,
        std::uint64_t worker_generation,
        const std::shared_ptr<savor::ProcessWorker>& worker);
    void DrainWorksetEventsLoop();
    void HandleWorksetState(
        const WorksetEventIngress& event);
    void HandleWorksetItemStarted(
        const WorksetEventIngress& event);
    void HandleWorksetItemProgress(
        const WorksetEventIngress& event);
    void HandleWorksetItemTerminal(
        const WorksetEventIngress& event);
    void HandleWorksetCredits(
        const WorksetEventIngress& event);
    void HandleWorksetSummary(
        const WorksetEventIngress& event);
    bool ProcessDurableResultProjection(
        const savor::PRResult& result,
        const DispatchedJobContext& context,
        savor::db::execution::workflow::
            TerminalWorkflowStepNotification* notification_out = nullptr,
        std::vector<std::string>* post_ack_event_lines_out = nullptr);
    bool ProcessDurableResultProjectionLocked(
        const savor::PRResult& result,
        const DispatchedJobContext& context,
        savor::db::execution::workflow::
            TerminalWorkflowStepNotification* notification_out,
        std::vector<std::string>* post_ack_event_lines_out);
    void PublishTerminalCommitIsolated(
        const savor::db::execution::workflow::
            TerminalWorkflowStepNotification& notification);
    void FailClosedWorkset(
        std::size_t worker_idx,
        std::uint64_t workset_id,
        std::string_view reason);
    void FailClosedWorker(
        std::size_t worker_idx,
        std::string_view reason);
    bool AcceptWorksetEventSequence(
        std::size_t worker_idx,
        std::uint64_t workset_id,
        std::uint64_t outbound_sequence,
        std::string_view event_kind);
    bool DispatchNextEligibleForWorker(
        size_t worker_idx,
        const MaterializedJobSelectionAffinity& worker_affinity,
        std::chrono::steady_clock::time_point now);
    void EmitAdapterTraceEvent(
        const WorkflowReadyStep& step,
        const std::string& stage,
        const std::string& status,
        std::optional<std::int64_t> job_id,
        std::optional<std::int64_t> job_set_id,
        const std::optional<std::string>& message = std::nullopt) const;
    void MarkDeterministicFailure(
        const WorkflowReadyStep& step,
        std::optional<std::int64_t> job_id,
        std::optional<std::int64_t> job_set_id,
        const std::string& reason) const;
    void EmitWorkflowFailureEvents(
        const WorkflowReadyStep& step,
        const std::string& stage,
        const std::string& reason) const;
    void EmitDurableEventLine(const std::string& line) const;
    void MaybeTerminalFailStepInStrictSmokeMode(const WorkflowReadyStep& step, const std::string& requested_by) const;
    CoordinatorStartResult FailStart(
        CoordinatorStartStatus status,
        std::string error,
        std::vector<WorkerSlotPtr> slots_to_stop);

    void RegisterWorkerSlotTelemetry(const WorkerSlot& slot);
    void MarkWorkerError(const WorkerSlot& slot, const std::string& error);
    void RecordCoordinatorWarning(
        std::int64_t worker_id,
        std::int64_t job_id,
        std::string message,
        std::string detail);
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IStateDb* state_db_ = nullptr;
    DBWorkflowWorkerCoordinatorConfig worker_cfg_{};
    std::atomic<size_t> desired_worker_count_{ 1 };
    std::atomic<size_t> worker_slot_count_{ 0 };
    CoordinatorIntegrationConfig integration_cfg_{};
    std::function<ScheduledJobSet(const WorkflowReadyStep&)> schedule_ready_step_fn_;
    StepInputAggregationService input_aggregation_service_;
    ReadyStepPersistFn persist_materialization_fn_;
    JobMaterializationService job_materialization_service_;
    const savor::db::execution::programdb::ProgramKindRegistry* program_kind_registry_ = nullptr;
    std::unique_ptr<savor::db::execution::workflow::StepCompletionGateService> owned_step_completion_gate_;
    savor::db::execution::workflow::StepCompletionGateService* step_completion_gate_ = nullptr;
    std::unique_ptr<savor::db::execution::workflow::AdapterChainOrchestrator> adapter_chain_orchestrator_;
    WorkflowCoordinatorBridge workflow_bridge_;
    ProgressCallback progress_callback_;
    ResultCallback result_callback_;
    ResultMapEventCallback result_map_event_callback_;

    std::atomic<bool> stop_{ false };
    std::atomic<bool> stop_started_{ false };
    std::atomic<bool> paused_{ false };
    std::atomic<bool> data_plane_enabled_{ false };
    std::atomic<uint64_t> epoch_{ 1 };
    std::thread worker_job_thread_;
    std::thread worker_lifecycle_thread_;
    std::thread job_materializer_thread_;
    std::thread progress_drainer_thread_;
    std::thread results_drainer_thread_;
    std::thread workset_event_drainer_thread_;
    std::chrono::steady_clock::time_point last_claim_lease_maintenance_{};

    mutable std::mutex queue_mtx_;
    std::condition_variable queue_cv_;
    std::deque<WorkflowReadyStep> ready_queue_;
    std::unordered_set<std::string> seen_ready_step_ids_;
    std::unordered_set<std::int64_t> seen_workflow_instance_ids_;
    mutable std::mutex workers_mtx_;
    std::vector<WorkerSlotPtr> workers_;
    std::mutex runtime_preparation_mtx_;
    std::optional<std::filesystem::path> prepared_runtime_worker_exe_;
    std::unordered_map<std::uint64_t, DispatchedJobContext> dispatched_job_context_by_id_;
    std::unordered_map<std::string, DispatchedWorksetItemContext>
        dispatched_workset_items_;
    std::unordered_map<size_t, WorkerVisualSurface> worker_visual_surfaces_;
    size_t rr_worker_cursor_ = 0;
    TSQueue<savor::PRProgress> progress_q_;
    TSQueue<savor::PRResult> results_q_;
    TSQueue<WorksetEventIngress> workset_event_q_;
    std::mutex result_projection_mtx_;
    size_t materialized_count_ = 0;
    size_t terminal_published_count_ = 0;
    std::atomic<std::int64_t> ready_scan_count_{ 0 };
    std::atomic<std::int64_t> ready_steps_enqueued_{ 0 };
    std::atomic<std::int64_t> last_ready_scan_latency_ms_{ 0 };
    std::atomic<std::int64_t> max_ready_queue_depth_{ 0 };
    std::atomic<std::int64_t> input_complete_count_{ 0 };
    std::atomic<std::int64_t> input_timeout_count_{ 0 };
    std::atomic<std::int64_t> terminal_input_failure_count_{ 0 };
    std::atomic<std::int64_t> last_input_latency_ms_{ 0 };
    std::atomic<std::int64_t> materialization_count_{ 0 };
    std::atomic<std::int64_t> last_materialization_latency_ms_{ 0 };
    std::atomic<std::int64_t> max_materialization_latency_ms_{ 0 };
    std::atomic<std::int64_t> stale_claim_count_{ 0 };
    std::atomic<std::int64_t> dispatch_attempt_count_{ 0 };
    std::atomic<std::int64_t> dispatch_success_count_{ 0 };
    std::atomic<std::int64_t> dispatch_miss_count_{ 0 };
    std::atomic<std::int64_t> progress_batch_count_{ 0 };
    std::atomic<std::int64_t> max_progress_batch_size_{ 0 };
    std::atomic<std::int64_t> results_received_count_{ 0 };
    std::atomic<std::uint64_t> terminal_commit_sequence_{ 0 };
    std::atomic<std::int64_t> workflow_created_signal_count_{ 0 };
    std::atomic<std::int64_t> materialization_failure_count_{ 0 };
    std::atomic<std::int64_t> payload_materialization_failure_count_{ 0 };
    std::atomic<std::int64_t> claim_attempt_count_{ 0 };
    std::atomic<std::int64_t> claimed_job_count_{ 0 };
    std::atomic<std::int64_t> clean_zero_claim_count_{ 0 };
    std::atomic<std::int64_t> claim_error_count_{ 0 };
    std::atomic<std::int64_t> partial_claim_count_{ 0 };
    std::atomic<bool> no_jobs_available_{ false };
    std::atomic<std::int64_t> adapter_input_complete_invocations_{ 0 };
    std::atomic<std::int64_t> adapter_job_claimed_invocations_{ 0 };
    std::atomic<std::int64_t> adapter_job_terminal_invocations_{ 0 };
    mutable std::mutex coordinator_warning_mtx_;
    std::deque<CoordinatorWarningSnapshot> coordinator_warnings_;
    std::uint64_t next_coordinator_warning_sequence_ = 1;
    WorkerStatusRegistry worker_status_;
    mutable std::mutex lifecycle_mtx_;
    mutable std::mutex start_mtx_;
    bool start_attempted_ = false;
    CoordinatorStartResult last_start_result_{};
};

} // namespace savor::runner::parallel::savordb
