#pragma once
#include "WorkerTelemetry.h"
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <vector>
#include <chrono>
#include <optional>

class WorkerStatusRegistry {
public:
    WorkerStatusRegistry();

    void SetEventBufferCapacity(size_t n);

    void RegisterWorker(int64_t worker_id, const std::string& host, int pid, const std::string& boot_uuid);
    void UnregisterWorker(int64_t worker_id);

    void UpdateState(int64_t worker_id, WorkerStateKind s);
    void SetCurrentJob(int64_t worker_id, std::optional<int64_t> job_id, std::optional<int> program_kind);
    void SetLeaseInfo(int64_t worker_id, std::optional<int64_t> lease_expires_at, int attempts, int max_attempts);

    void RecordEvent(int64_t worker_id, WorkerEventKind k, std::optional<int64_t> job_id = std::nullopt, const std::string& note = {});
    void RecordHeartbeat(int64_t worker_id);
    void RecordDbSuccess(int64_t worker_id);
    void RecordError(int64_t worker_id, const std::string& err);

    WorkerStateKind GetWorkerState(int64_t worker_id) const;
    std::vector<WorkerSnapshot> GetClusterSnapshot() const;

private:
    struct Ring {
        std::vector<WorkerEvent> buf;
        size_t head{ 0 };
        size_t size{ 0 };
        void ensure_capacity(size_t cap) {
            if (buf.size() == cap) return;
            std::vector<WorkerEvent> nb; nb.reserve(cap);
            size_t to_copy = size < cap ? size : cap;
            for (size_t i = 0; i < to_copy; ++i) {
                nb.push_back(buf[(head + i) % (buf.size() ? buf.size() : 1)]);
            }
            buf.swap(nb);
            head = 0;
            size = to_copy;
        }
        void push(const WorkerEvent& e) {
            if (buf.empty()) return;
            if (size < buf.size()) {
                buf[(head + size) % buf.size()] = e;
                ++size;
            }
            else {
                buf[head] = e;
                head = (head + 1) % buf.size();
            }
        }
        std::vector<WorkerEvent> snapshot() const {
            std::vector<WorkerEvent> out;
            out.reserve(size);
            for (size_t i = 0; i < size; ++i) {
                out.push_back(buf[(head + i) % buf.size()]);
            }
            return out;
        }
    };

    struct WorkerRecord {
        int64_t worker_id{};
        std::string host;
        int pid{};
        std::string boot_uuid;

        WorkerStateKind state{ WorkerStateKind::Spawning };
        std::optional<int64_t> job_id{};
        std::optional<int> program_kind{};
        std::optional<int64_t> lease_expires_at{};
        int attempts{};
        int max_attempts{};

        int64_t start_time_utc{};
        int64_t last_state_change_mono_ns{};
        int64_t last_heartbeat_mono_ns{};
        int64_t last_successful_db_call_mono_ns{};
        int consecutive_failures{};
        std::string last_error;

        mutable std::mutex ev_mtx;
        Ring events;
    };

    static int64_t NowEpochNs();
    static int64_t NowMonoNs();

    WorkerRecord& ensure_worker_locked_(int64_t worker_id);

    size_t event_capacity_;
    mutable std::shared_mutex mtx_;
    std::unordered_map<int64_t, WorkerRecord> workers_;
};
