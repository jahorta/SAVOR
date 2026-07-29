#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

#include "Runner/IPC/WrmsProtocol.h"
#include "Runner/Runtime/IProgramRuntimePort.h"
#include "Runner/Runtime/RuntimeTypes.h"
#include "Runner/Runtime/Worksets/WorksetTypes.h"
#include "Runner/Script/PhaseScriptProgram.h"
#include "Runner/Parallel/PRTypes.h"
#include "TSQueue.h"
#include "WorkerTelemetry.h"

namespace savor {

inline constexpr const char* kDisconnectedWorkerApiDiagnostic =
    "legacy worker execution is disconnected during the hard-cutover refactor; "
    "canonical ProgramRuntime is not available";
inline constexpr std::chrono::milliseconds kProcessWorkerDefaultStopGrace{
    5000};
inline constexpr std::chrono::milliseconds
    kProcessWorkerDefaultCallbackCleanupGrace{250};

struct ProcessWorkerTestHooks {
    std::optional<std::chrono::milliseconds> stop_grace;
    std::optional<std::chrono::milliseconds> callback_cleanup_grace;
    std::function<void()> stop_acceptance_closed;
    std::function<void(
        wrms::MessageKind,
        std::span<const std::uint8_t>)> observe_writer_frame;
    std::function<void(wrms::MessageKind)> before_writer_write;
    std::function<void(wrms::MessageKind, bool)> after_writer_write;
    std::function<void()> writer_cancel_requested;
    std::function<void()> before_stdin_close;
    std::function<std::optional<bool>()> process_alive_override;
};

struct ProcessWorkerStopSnapshot {
    bool already_stopping{ false };
    bool was_running{ false };
    std::uint32_t stop_grace_ms{
        static_cast<std::uint32_t>(
            kProcessWorkerDefaultStopGrace.count())};
    bool deadline_expired{ false };
    bool shutdown_frame_attempted{ false };
    bool shutdown_frame_succeeded{ false };
    bool shutdown_result_received{ false };
    std::uint64_t shutdown_request_id{ 0 };
    bool graceful{ false };
    bool forced{ false };
    bool stdin_close_attempted{ false };
    bool stdin_close_succeeded{ false };
    std::uint32_t stdin_close_error{ 0 };
    bool cancel_writer_attempted{ false };
    bool cancel_writer_succeeded{ false };
    std::uint32_t cancel_writer_error{ 0 };
    bool writer_joined{ false };
    bool termination_attempted{ false };
    bool termination_succeeded{ false };
    std::uint32_t termination_error{ 0 };
    std::string termination_method;
    bool cancel_pipe_attempted{ false };
    bool cancel_pipe_succeeded{ false };
    std::uint32_t cancel_pipe_error{ 0 };
    bool cancel_reader_attempted{ false };
    bool cancel_reader_succeeded{ false };
    std::uint32_t cancel_reader_error{ 0 };
    bool reader_joined{ false };
    bool callback_dispatcher_joined{ false };
    bool callback_cleanup_timed_out{ false };
    std::uint32_t cooperative_wait_result{ WAIT_FAILED };
    std::uint32_t process_wait_result{ WAIT_FAILED };
    std::uint32_t process_wait_error{ 0 };
    std::uint32_t process_exit_code{ 0 };
};

struct ProcessLaunchOptions {
    std::size_t worker_id{ 0 };
    std::string exe_path;
    std::string log_directory;
    std::uint32_t hello_timeout_ms{ 10000 };
};

struct ProcessOpenSessionOptions {
    std::string runtime_root;
    std::string user_directory;
    std::string iso_path;
    bool visual{ false };
    std::uint64_t render_widget_handle{ 0 };
    std::string screenshot_directory;
    std::uint32_t screenshot_timeout_ms{ 5000 };
    bool screenshot_on_terminal{ false };
};

// Retained as a source-compatible launch description while callers migrate to
// launch_and_negotiate() plus open_session(). No session concern is placed on
// the child command line.
struct ProcStartParams {
    std::size_t worker_id{ 0 };
    std::string exe_path;
    std::string iso_path;
    std::string dolphin_base_dir;
    std::string user_dir;
    bool vm_control{ false };
    bool visual{ false };
    bool visual_debug{ false };
    std::uint64_t render_widget_handle{ 0 };
    std::string visual_control_pipe_name;
    std::string visual_host_events_pipe_name;
    std::string visual_screenshot_dir;
};

struct ProcessCommandCompletion {
    runtime::WireRequestId request_id;
    bool request_frame_written{ false };
    bool correlated_response_received{ false };
    wrms::MessageKind response_kind{ wrms::MessageKind::CommandResult };
    std::vector<std::uint8_t> payload;
};

enum class ProcessWorksetSubmitDisposition : std::uint8_t {
    Accepted,
    DefiniteRejected,
    AmbiguousAfterWrite,
};

struct ProcessWorksetSubmitOutcome {
    ProcessWorksetSubmitDisposition disposition{
        ProcessWorksetSubmitDisposition::DefiniteRejected};
    bool request_frame_written{ false };
    bool correlated_result_received{ false };
    wrms::CommandResultPayload result;
    std::string diagnostic;

    [[nodiscard]] bool accepted() const noexcept
    {
        return disposition == ProcessWorksetSubmitDisposition::Accepted;
    }
};

struct ProcessWorkerSnapshot {
    bool running{ false };
    bool hello_received{ false };
    runtime::WorkerCapabilityMask process_capabilities{ 0 };
    bool session_open{ false };
    bool session_visual_intent{ false };
    runtime::SessionId session_id;
    runtime::StateEpoch state_epoch;
    runtime::WorkerCapabilityMask session_capabilities{ 0 };
    runtime::WorkerState worker_state{ runtime::WorkerState::Starting };
    runtime::SessionDisposition session_disposition{ runtime::SessionDisposition::Closed };
    wrms::ExecutionActivityCode execution_activity{
        wrms::ExecutionActivityCode::IdlePaused};
    std::uint64_t execution_operation_id{ 0 };
    std::optional<wrms::ExecutionControlKind> active_execution_control;
    std::uint64_t execution_completed_count{ 0 };
    std::uint32_t execution_program_counter{ 0 };
    runtime::WorkerRejectionCode last_rejection_code{
        runtime::WorkerRejectionCode::None};
    bool shutdown_graceful{ false };
    std::string last_error;
    bool runtime_manifest_received{ false };
    std::optional<runtime::WorkerWorksetId> active_workset;
    std::optional<runtime::WorkerWorksetId> staged_workset;
    std::uint32_t available_item_credits{ 0 };
    std::uint32_t active_and_staged_items{ 0 };
    std::uint32_t retained_terminals{ 0 };
    std::uint64_t last_outbound_sequence{ 0 };
};

class ProcessWorker {
public:
    using InvocationProgressCallback =
        std::function<void(const wrms::InvocationProgressPayload&)>;
    using InvocationTerminalCallback =
        std::function<void(const wrms::InvocationTerminalPayload&)>;
    using HostEventCallback =
        std::function<void(const wrms::HostEventPayload&)>;
    using ExecutionStateCallback =
        std::function<void(const wrms::ExecutionStatePayload&)>;
    using WorksetStateCallback =
        std::function<void(const wrms::WorksetStatePayload&)>;
    using WorksetItemStartedCallback =
        std::function<void(const wrms::WorksetItemStartedPayload&)>;
    using WorksetItemTerminalCallback =
        std::function<void(const wrms::WorksetItemTerminalPayload&)>;
    using WorksetCreditsCallback =
        std::function<void(const wrms::WorksetCreditsPayload&)>;
    using WorksetSummaryCallback =
        std::function<void(const wrms::WorksetSummaryPayload&)>;

    ProcessWorker() = default;
    explicit ProcessWorker(
        std::shared_ptr<const ProcessWorkerTestHooks> test_hooks)
        : test_hooks_(std::move(test_hooks))
    {
    }
    ~ProcessWorker();

    ProcessWorker(const ProcessWorker&) = delete;
    ProcessWorker& operator=(const ProcessWorker&) = delete;

    bool launch_and_negotiate(
        const ProcessLaunchOptions& options,
        std::string* error_out = nullptr);
    bool open_session(
        const ProcessOpenSessionOptions& options,
        wrms::OpenSessionResultPayload* result_out = nullptr,
        std::string* error_out = nullptr,
        std::uint32_t timeout_ms = 30000);

    bool prepare_encoded_module(
        const runtime::EncodedModuleEnvelope& module,
        wrms::CommandResultPayload* result_out = nullptr,
        std::uint32_t timeout_ms = 10000);
    bool submit_encoded_invocation(
        const runtime::EncodedInvocationEnvelope& invocation,
        wrms::CommandResultPayload* result_out = nullptr,
        std::uint32_t timeout_ms = 10000);
    bool submit_workset(
        const runtime::WorkerWorksetDefinition& workset,
        wrms::CommandResultPayload* result_out = nullptr,
        std::uint32_t timeout_ms = 10000);
    ProcessWorksetSubmitOutcome submit_workset_with_outcome(
        const runtime::WorkerWorksetDefinition& workset,
        std::uint32_t timeout_ms = 10000);
    bool submit_one_item_workset(
        const runtime::WorkerWorksetDefinition& workset,
        wrms::CommandResultPayload* result_out = nullptr,
        std::uint32_t timeout_ms = 10000);
    bool cancel_workset_item(
        runtime::WorkerWorksetId workset_id,
        runtime::WorkerWorksetItemId item_id,
        std::string reason,
        wrms::CommandResultPayload* result_out = nullptr,
        std::uint32_t timeout_ms = 10000);
    bool cancel_workset(
        runtime::WorkerWorksetId workset_id,
        std::string reason,
        wrms::CommandResultPayload* result_out = nullptr,
        std::uint32_t timeout_ms = 10000);
    bool acknowledge_terminal(
        const runtime::WorkerItemTerminalCorrelation& terminal,
        wrms::CommandResultPayload* result_out = nullptr,
        std::uint32_t timeout_ms = 10000);
    bool cancel_invocation(
        runtime::InvocationId invocation_id,
        std::string reason,
        wrms::CommandResultPayload* result_out = nullptr,
        std::uint32_t timeout_ms = 10000);
    bool request_screenshot(
        runtime::SessionId session_id,
        std::string output_path,
        std::uint32_t capture_timeout_ms,
        wrms::ScreenshotResultPayload* result_out = nullptr,
        std::uint32_t command_timeout_ms = 10000);
    bool pause_guest_execution(
        runtime::SessionId session_id,
        runtime::StateEpoch expected_state_epoch,
        wrms::ExecutionResultPayload* result_out = nullptr,
        std::uint32_t operation_timeout_ms = 3000,
        std::uint32_t command_timeout_ms = 10000);
    bool resume_guest_execution(
        runtime::SessionId session_id,
        runtime::StateEpoch expected_state_epoch,
        wrms::ExecutionResultPayload* result_out = nullptr,
        std::uint32_t command_timeout_ms = 10000);
    bool step_guest_frames(
        runtime::SessionId session_id,
        runtime::StateEpoch expected_state_epoch,
        std::uint32_t count = 1,
        wrms::ExecutionResultPayload* result_out = nullptr,
        std::uint32_t operation_timeout_ms = 3000,
        std::uint32_t command_timeout_ms = 10000);

    runtime::WorkerCapabilityMask process_capabilities() const;
    runtime::WorkerCapabilityMask session_capabilities() const;
    bool has_process_capability(runtime::WorkerCapability capability) const;
    bool has_session_capability(runtime::WorkerCapability capability) const;
    ProcessWorkerSnapshot latest_snapshot() const;
    std::optional<runtime::WorkerRuntimeManifest> runtime_manifest() const;
    std::string last_error() const;

    void set_invocation_progress_callback(InvocationProgressCallback callback);
    void set_invocation_terminal_callback(InvocationTerminalCallback callback);
    void set_host_event_callback(HostEventCallback callback);
    void set_execution_state_callback(ExecutionStateCallback callback);
    void set_workset_state_callback(WorksetStateCallback callback);
    void set_workset_item_started_callback(
        WorksetItemStartedCallback callback);
    void set_workset_item_terminal_callback(
        WorksetItemTerminalCallback callback);
    void set_workset_credits_callback(WorksetCreditsCallback callback);
    void set_workset_summary_callback(WorksetSummaryCallback callback);

    // Transitional convenience: launch the v1 process and explicitly open its
    // one session. This does not restore any legacy program execution path.
    bool start(ProcStartParams& params, TSQueue<PRResult>* out_queue);

    // Deprecated hard-cutover APIs. They fail locally and never write a frame.
    bool send_job(std::uint64_t job_id, std::uint64_t dispatch_epoch, const PSJob& job);
    bool ctl_set_program(std::uint8_t init_kind, std::uint8_t main_kind, const PSInit& init);
    bool ctl_run_init_once();
    bool ctl_activate_main();
    bool visual_pause_emulation();
    bool visual_resume_emulation();
    bool visual_step_vm();

    void stop();
    ProcessWorkerStopSnapshot last_stop_snapshot() const;

    bool is_ready() const;
    bool is_failed() const;
    bool is_running() const { return running_.load(std::memory_order_acquire); }
    std::uint32_t ready_error() const { return ready_error_.load(std::memory_order_acquire); }
    HANDLE process_handle() const { return process_handle_; }
    bool wait_ready(std::uint32_t timeout_ms);

    bool try_acquire_slot()
    {
        bool expected = false;
        return busy_.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel);
    }
    void release_slot() { busy_.store(false, std::memory_order_release); }
    bool has_slot() const { return !busy_.load(std::memory_order_acquire); }

    void set_progress_queue(TSQueue<PRProgress>* queue) { progress_out_ = queue; }
    bool try_get_last_progress(PRProgress& out) const;

    std::int64_t GetPid() const { return static_cast<std::int64_t>(process_id_); }

private:
    friend class ProcessWorkerTestPeer;

    struct PendingResponse {
        std::mutex mutex;
        std::condition_variable cv;
        bool completed{ false };
        bool transport_ok{ false };
        wrms::MessageKind request_kind{ wrms::MessageKind::Shutdown };
        wrms::MessageKind response_kind{ wrms::MessageKind::CommandResult };
        std::vector<std::uint8_t> payload;
    };

    struct PendingCallback {
        std::function<void()> invoke;
        std::size_t resident_bytes{ 0 };
        bool authoritative{ false };
    };

    struct CallbackDispatcherState {
        std::mutex mutex;
        std::condition_variable available;
        std::condition_variable exited_cv;
        std::deque<PendingCallback> queue;
        std::size_t queued_bytes{ 0 };
        bool stop_requested{ false };
        bool exited{ false };
        std::thread::id thread_id;
    };

    struct CallbackFailureTarget {
        std::mutex mutex;
        ProcessWorker* owner{ nullptr };
    };

    struct OutboundWrite {
        wrms::MessageKind kind{ wrms::MessageKind::Shutdown };
        std::vector<std::uint8_t> bytes;
        std::mutex mutex;
        std::condition_variable cv;
        bool completed{ false };
        bool succeeded{ false };
        std::uint32_t error{ 0 };
    };

    bool create_child(const ProcessLaunchOptions& options, std::string* error_out);
    std::shared_ptr<OutboundWrite> enqueue_frame(
        wrms::MessageKind kind,
        runtime::WireRequestId request_id,
        std::span<const std::uint8_t> payload,
        bool allow_during_stop = false);
    bool write_frame(
        wrms::MessageKind kind,
        runtime::WireRequestId request_id,
        std::span<const std::uint8_t> payload,
        std::chrono::steady_clock::time_point deadline,
        bool allow_during_stop = false,
        bool* timed_out = nullptr);
    bool request_response(
        wrms::MessageKind request_kind,
        std::span<const std::uint8_t> request_payload,
        wrms::MessageKind expected_response_kind,
        std::uint32_t timeout_ms,
        ProcessCommandCompletion* completion_out,
        bool allow_during_stop = false);
    bool request_execution_control(
        wrms::ExecutionControlKind control,
        runtime::SessionId session_id,
        runtime::StateEpoch expected_state_epoch,
        std::uint32_t count,
        std::uint32_t operation_timeout_ms,
        wrms::ExecutionResultPayload* result_out,
        std::uint32_t command_timeout_ms);
    [[nodiscard]] static std::uint32_t effective_execution_command_timeout(
        std::uint32_t operation_timeout_ms,
        std::uint32_t command_timeout_ms) noexcept;
    [[nodiscard]] static bool validate_execution_result(
        wrms::ExecutionControlKind control,
        runtime::SessionId session_id,
        runtime::StateEpoch expected_state_epoch,
        std::uint32_t requested_count,
        const wrms::ExecutionResultPayload& result,
        std::string* error_out);
    [[nodiscard]] static bool classify_shutdown_response(
        std::span<const std::uint8_t> payload,
        bool* graceful_out);
    void complete_pending(
        std::uint64_t request_id,
        wrms::MessageKind kind,
        std::span<const std::uint8_t> payload);
    [[nodiscard]] bool has_exact_pending_request(
        std::uint64_t request_id,
        wrms::MessageKind request_kind) const;
    void fail_all_pending();
    void writer_thread();
    void request_writer_stop(ProcessWorkerStopSnapshot* snapshot);
    void join_writer(ProcessWorkerStopSnapshot* snapshot);
    void reader_thread();
    static void callback_thread(
        std::shared_ptr<CallbackDispatcherState> state,
        std::shared_ptr<CallbackFailureTarget> failure_target,
        std::size_t worker_id);
    void start_callback_dispatch();
    bool enqueue_callback(
        std::function<void()> callback,
        bool authoritative = false,
        std::size_t resident_bytes = 0);
    [[nodiscard]] bool stop_callback_dispatch(
        std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] bool on_callback_dispatcher_thread() const;
    void detach_callback_failure_target() noexcept;
    void handle_frame(const wrms::FrameView& frame);
    [[nodiscard]] bool accept_workset_outbound_sequence(
        std::uint64_t sequence);
    void fail_protocol(std::string error);
    void set_last_error(std::string error);
    bool fail_disconnected(const char* operation);
    void close_process_handles(ProcessWorkerStopSnapshot* snapshot);

    HANDLE child_stdin_write_{ nullptr };
    HANDLE child_stdout_read_{ nullptr };
    HANDLE process_handle_{ nullptr };
    HANDLE process_thread_handle_{ nullptr };
    HANDLE job_handle_{ nullptr };
    unsigned long process_id_{ 0 };

    std::thread reader_;
    std::thread writer_;
    std::thread callback_dispatcher_;
    std::shared_ptr<CallbackDispatcherState> callback_dispatcher_state_;
    std::shared_ptr<CallbackFailureTarget> callback_failure_target_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> accepting_writes_{ false };
    std::atomic<bool> writer_cancel_requested_{ false };
    std::atomic<bool> stop_started_{ false };
    std::atomic<bool> stop_repeated_{ false };
    std::atomic<bool> busy_{ false };
    std::atomic<bool> ready_received_{ false };
    std::atomic<bool> ready_ok_{ false };
    std::atomic<std::uint32_t> ready_error_{ 0 };
    std::atomic<std::uint64_t> next_request_id_{ 1 };
    std::atomic<bool> protocol_failed_{ false };

    mutable std::mutex writer_mutex_;
    std::condition_variable writer_cv_;
    std::deque<std::shared_ptr<OutboundWrite>> writer_queue_;
    bool writer_exit_requested_{ false };
    bool writer_active_{ false };
    mutable std::mutex pending_mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<PendingResponse>> pending_;

    mutable std::mutex snapshot_mutex_;
    ProcessWorkerSnapshot snapshot_;
    wrms::ProcessHelloPayload hello_;
    std::optional<runtime::WorkerRuntimeManifest> runtime_manifest_;
    std::condition_variable hello_cv_;

    mutable std::mutex callback_mutex_;
    InvocationProgressCallback invocation_progress_callback_;
    InvocationTerminalCallback invocation_terminal_callback_;
    HostEventCallback host_event_callback_;
    ExecutionStateCallback execution_state_callback_;
    WorksetStateCallback workset_state_callback_;
    WorksetItemStartedCallback workset_item_started_callback_;
    WorksetItemTerminalCallback workset_item_terminal_callback_;
    WorksetCreditsCallback workset_credits_callback_;
    WorksetSummaryCallback workset_summary_callback_;

    mutable std::mutex progress_mutex_;
    PRProgress last_progress_{};
    bool have_progress_{ false };
    TSQueue<PRProgress>* progress_out_{ nullptr };
    TSQueue<PRResult>* legacy_result_out_{ nullptr };
    std::size_t worker_id_{ 0 };

    mutable std::mutex stop_mutex_;
    ProcessWorkerStopSnapshot last_stop_snapshot_;
    mutable std::mutex stop_completion_mutex_;
    std::condition_variable stop_completion_cv_;
    bool stop_completed_{ true };
    std::shared_ptr<const ProcessWorkerTestHooks> test_hooks_;
};

} // namespace savor
