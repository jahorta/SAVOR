// SimCore/Runner/Scheduling/WorkerCoordinator.h
#pragma once
#include <vector>
#include <memory>
#include <atomic>
#include <optional>
#include <string>
#include <chrono>
#include <thread>
#include "../PRTypes.h"
#include "../ProcessWorker.h"
#include "../../../DB/ProgramDB/IProgramDBCodec.h"
#include "../../../DB/Scheduling/JobsRepo.h"
#include "../../../DB/Scheduling/JobEventsRepo.h"
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
        void SetEventBufferCapacity(size_t n);

    private:
        struct Slot {
            size_t id{ 0 };
            std::unique_ptr<ProcessWorker> proc;
            std::atomic<bool> running{ false };
            std::atomic<bool> ready{ false };
            std::atomic<bool> dead{ false };

            std::optional<int> current_program_kind;
            std::optional<int64_t> current_savestate_id;
            std::optional<int64_t> assigned_job_id;

            std::chrono::steady_clock::time_point idle_deadline{};
            std::chrono::steady_clock::time_point lease_renew_deadline{};
        };

        void controller_loop();
        void drain_progress_loop();
        void drain_results_loop();

        bool spawn_slot(Slot& s);
        void shutdown_slot(Slot& s);

        bool ensure_ready(Slot& s);
        bool ensure_program(Slot& s, int program_kind, std::optional<int64_t> required_savestate_id, IProgramDBCodec& codec, int64_t job_id, uint32_t default_timeout_ms = 10000);
        bool dispatch_one(Slot& s, const simcore::db::JobRow& job, IProgramDBCodec& codec);

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
    };

} // namespace simcore
