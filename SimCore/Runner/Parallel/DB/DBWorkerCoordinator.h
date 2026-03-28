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
#include <mutex>
#include <condition_variable>
#include "../PRTypes.h"
#include "../ProcessWorker.h"
#include "../../../DB/ProgramDB/IProgramDBCodec.h"
#include "../../../DB/Scheduling/JobsRepo.h"
#include "../../../DB/Scheduling/JobEventsRepo.h"
#include "../../../DB/Scheduling/VisualReplayRepo.h"
#include "DBWorkerCoordinatorConfig.h"
#include "../WorkerStatusRegistry.h"

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
        std::optional<WorkerSnapshot> GetVisualWorkerSnapshot() const;
        void SetEventBufferCapacity(size_t n);

        // dynamic controls
        void set_target_workers(size_t n);
        void set_paused(bool p);
        void SetVisualRenderWidgetHandle(uint64_t hwnd);
        bool is_paused() const { return paused_.load(); }

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
            std::optional<int64_t> assigned_visual_replay_id;

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
        void visual_listener_loop();
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
        DispatchResult dispatch_one(Slot& s, const simcore::db::JobRow& job, IProgramDBCodec& codec, bool update_job_state = true);

        void renew_lease_if_due(int64_t job_id, Slot& s);
        void sweep_expired_leases();

        WorkerCoordinatorConfig cfg_;
        std::vector<std::unique_ptr<Slot>> slots_;
        std::unique_ptr<Slot> visual_slot_;

        std::atomic<bool> stop_{ false };
        std::thread controller_;
        std::thread visual_listener_;
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
        mutable std::mutex visual_slot_mtx_;
        std::condition_variable visual_slot_cv_;
        std::atomic<uint64_t> visual_render_widget_handle_{ 0 };
    };

} // namespace simcore
