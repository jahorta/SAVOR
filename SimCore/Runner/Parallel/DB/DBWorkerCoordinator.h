// SimCore/Runner/Scheduling/WorkerCoordinator.h
#pragma once
#include <vector>
#include <memory>
#include <atomic>
#include <optional>
#include <string>
#include <chrono>
#include <thread>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "../PRTypes.h"
#include "../ProcessWorker.h"
#include "../../../DB/ProgramDB/IProgramDBCodec.h"
#include "../../../DB/Scheduling/JobsRepo.h"
#include "../../../DB/Scheduling/JobEventsRepo.h"
#include "DBWorkerCoordinatorConfig.h"
#include "../WorkerStatusRegistry.h"
#include "../../../DB/Scheduling/DebugSessionsRepo.h"

namespace simcore::debug {
    class LocalDebugControlServer;
}

namespace simcore {

    class WorkerCoordinator {
    public:
        explicit WorkerCoordinator(const WorkerCoordinatorConfig& cfg);
        ~WorkerCoordinator();

        void start();
        void stop();

        PRStatus snapshot_status() const;

        void RegisterWorker(int64_t worker_id, const std::string& host, int pid, const std::string& boot_uuid);
        void UnregisterWorker(int64_t worker_id);

        void UpdateState(int64_t worker_id, WorkerStateKind s);
        void SetCurrentJob(int64_t worker_id, std::optional<int64_t> job_id, std::optional<int> program_kind);
        void SetLeaseInfo(int64_t worker_id, std::optional<int64_t> lease_expires_at, int attempts, int max_attempts);

        void RecordEvent(int64_t worker_id, WorkerEventKind k, std::optional<int64_t> job_id = std::nullopt, const std::string& note = {});
        void RecordHeartbeat(int64_t worker_id);
        void RecordDbSuccess(int64_t worker_id);
        void RecordError(int64_t worker_id, const std::string& err);

        std::vector<WorkerSnapshot> GetClusterSnapshot() const;
        void SetEventBufferCapacity(size_t n);

        // dynamic controls
        void set_target_workers(size_t n);
        void set_paused(bool p);
        bool is_paused() const { return paused_.load(); }

        struct DebugStartResult {
            bool ok{ false };
            int64_t request_id{ 0 };
            std::string status;
            std::string error;
        };

        struct DebugRuntimeSnapshot {
            int64_t session_id{ 0 };
            int64_t job_id{ 0 };
            std::string vm_state{ "VM_PAUSED" };
            std::string emu_state{ "EMU_PAUSED" };
            std::string ux_mode{ "FRAME_STEP_DEFAULT" };
            std::string break_reason{ "paused" };
            std::string script_name{ "<unknown>" };
            uint32_t script_pc{ 0 };
            int64_t frame_index{ 0 };
            std::string current_input{ "<none>" };
            int64_t sequence{ 0 };
            int64_t timestamp{ 0 };
            bool frame_ready{ false };
            std::vector<int64_t> breakpoints;
        };

        DebugStartResult StartDebug(int64_t job_id, const std::string& started_by = "gui");
        DbResult<void> StopDebug(int64_t session_id);
        DbResult<void> CancelStartDebug(int64_t request_id);
        DbResult<void> StepDebugVmInstruction(int64_t session_id);
        DbResult<void> StepDebugFrame(int64_t session_id);
        DbResult<void> RunToDebugBreakpoint(int64_t session_id);
        DbResult<void> PauseDebugSession(int64_t session_id);
        DbResult<void> ToggleDebugBreakpoint(int64_t session_id, int64_t step_id, bool enabled);
        DbResult<DebugRuntimeSnapshot> GetDebugRuntimeSnapshot(int64_t session_id) const;
        DbResult<std::optional<simcore::db::DebugSessionRow>> GetDebugSession(int64_t session_id) const;
        DbResult<std::optional<simcore::db::DebugSessionRow>> GetActiveDebugSessionForJob(int64_t job_id) const;

    private:
        struct Slot {
            enum class Phase : uint8_t {
                Uninitialized,
                PendingStart,
                StartingProcess,
                WaitingReady,
                Ready,
                Stopping,
                Dead
            };

            size_t id{ 0 };
            std::unique_ptr<ProcessWorker> proc;
            std::atomic<bool> running{ false };
            std::atomic<bool> ready{ false };
            std::atomic<bool> dead{ false };
            Phase phase{ Phase::Uninitialized };
            uint32_t startup_attempts{ 0 };
            std::chrono::steady_clock::time_point startup_deadline{};
            std::chrono::steady_clock::time_point next_ready_probe{};

            std::optional<int> current_program_kind;
            std::optional<int64_t> current_savestate_id;
            std::optional<int64_t> assigned_job_id;

            std::chrono::steady_clock::time_point idle_deadline{};
            std::chrono::steady_clock::time_point lease_renew_deadline{};
        };

        enum DispatchResult : uint8_t {
            Success,
            NotAvailable,
            BadDecode,
            BadSend
        };

        void controller_loop();
        void drain_progress_loop();
        void drain_results_loop();
        void advance_startup_once();
        void enqueue_startup_slot(size_t slot_id);
        void mark_slot_start_failed(Slot& s, const std::string& err);
        size_t active_slot_count() const;

        bool spawn_slot(Slot& s);
        void shutdown_slot(Slot& s);

        bool ensure_ready(Slot& s);
        bool ensure_program(Slot& s, int program_kind, std::optional<int64_t> required_savestate_id, IProgramDBCodec& codec, int64_t job_id, uint32_t default_timeout_ms = 10000);
        DispatchResult dispatch_one(Slot& s, const simcore::db::JobRow& job, IProgramDBCodec& codec);

        void renew_lease_if_due(int64_t job_id, Slot& s);
        void sweep_expired_leases();

        WorkerCoordinatorConfig cfg_;
        std::vector<std::unique_ptr<Slot>> slots_;

        std::atomic<bool> stop_{ false };
        std::thread controller_;
        std::thread progress_drainer_;
        std::thread results_drainer_;

        TSQueue<PRResult>   results_q_;
        TSQueue<PRProgress> progress_q_;

        std::atomic<uint64_t> epoch_{ 1 };
        std::string claim_token_;

        WorkerStatusRegistry worker_status_;

        std::atomic<size_t> desired_workers_{ 0 };
        std::atomic<bool>   paused_{ false };

        std::deque<size_t> startup_queue_;
        std::optional<size_t> startup_in_flight_slot_;

        mutable std::mutex debug_mu_;
        std::optional<int64_t> active_debug_session_id_;
        std::unordered_map<int64_t, DebugRuntimeSnapshot> debug_snapshots_;
        std::unordered_map<int64_t, std::unordered_set<int64_t>> debug_breakpoints_;
        std::unordered_map<int64_t, std::unique_ptr<simcore::debug::LocalDebugControlServer>> debug_control_servers_;
        std::unique_ptr<ProcessWorker> debug_worker_proc_;
        std::optional<int64_t> debug_worker_session_id_;
        std::chrono::steady_clock::time_point next_stale_debug_cleanup_{};

        static int64_t debug_now_sec();
        void stop_debug_worker_locked();
        DbResult<void> mutate_debug_session_(int64_t session_id, const std::function<void(DebugRuntimeSnapshot&)>& mutator);
    };

} // namespace simcore
