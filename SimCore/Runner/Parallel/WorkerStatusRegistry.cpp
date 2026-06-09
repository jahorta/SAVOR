#include "WorkerStatusRegistry.h"

using ClockMono = std::chrono::steady_clock;
using ClockSys = std::chrono::system_clock;

static int64_t to_ns(ClockMono::time_point tp) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
}
static int64_t to_ns(ClockSys::time_point tp) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
}

int64_t WorkerStatusRegistry::NowEpochNs() { return to_ns(ClockSys::now()); }
int64_t WorkerStatusRegistry::NowMonoNs() { return to_ns(ClockMono::now()); }

WorkerStatusRegistry::WorkerStatusRegistry()
    : event_capacity_(128) {
}

void WorkerStatusRegistry::SetEventBufferCapacity(size_t n) {
    std::unique_lock lk(mtx_);
    event_capacity_ = n ? n : 1;
    for (auto& kv : workers_) {
        std::lock_guard ek(kv.second.ev_mtx);
        kv.second.events.ensure_capacity(event_capacity_);
    }
}

WorkerStatusRegistry::WorkerRecord& WorkerStatusRegistry::ensure_worker_locked_(int64_t worker_id) {
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) {
        auto [ins, inserted] = workers_.try_emplace(worker_id); // default-construct mapped value
        it = ins;
        WorkerRecord& wr = it->second;
        wr.worker_id = worker_id;
        wr.start_time_utc = NowEpochNs();
        wr.last_state_change_mono_ns = NowMonoNs();
        wr.events.ensure_capacity(event_capacity_);
    }
    return it->second;
}

void WorkerStatusRegistry::RegisterWorker(int64_t worker_id, const std::string& host, int pid, const std::string& boot_uuid) {
    std::unique_lock lk(mtx_);
    auto& wr = ensure_worker_locked_(worker_id);
    wr.host = host;
    wr.pid = pid;
    wr.boot_uuid = boot_uuid;
    wr.state = WorkerStateKind::Spawning;
    wr.last_state_change_mono_ns = NowMonoNs();

    std::lock_guard ek(wr.ev_mtx);
    wr.events.push({ NowMonoNs(), WorkerEventKind::Spawned, std::nullopt, {} });
}

void WorkerStatusRegistry::UnregisterWorker(int64_t worker_id) {
    std::unique_lock lk(mtx_);
    workers_.erase(worker_id);
}

void WorkerStatusRegistry::UpdateState(int64_t worker_id, WorkerStateKind s) {
    std::shared_lock rk(mtx_);
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) return;
    auto& wr = it->second;
    wr.state = s;
    wr.last_state_change_mono_ns = NowMonoNs();
}

void WorkerStatusRegistry::SetCurrentJob(int64_t worker_id, std::optional<int64_t> job_id, std::optional<int> program_kind) {
    std::shared_lock rk(mtx_);
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) return;
    auto& wr = it->second;
    if (wr.job_id != job_id) {
        wr.last_progress.clear();
        wr.last_progress_mono_ns = 0;
    }
    wr.job_id = job_id;
    wr.program_kind = program_kind;
}

void WorkerStatusRegistry::SetLeaseInfo(int64_t worker_id, std::optional<int64_t> lease_expires_at, int attempts, int max_attempts) {
    std::shared_lock rk(mtx_);
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) return;
    auto& wr = it->second;
    wr.lease_expires_at = lease_expires_at;
    wr.attempts = attempts;
    wr.max_attempts = max_attempts;
}

void WorkerStatusRegistry::RecordEvent(int64_t worker_id, WorkerEventKind k, std::optional<int64_t> job_id, const std::string& note) {
    std::shared_lock rk(mtx_);
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) return;
    auto& wr = it->second;
    WorkerEvent e{ NowMonoNs(), k, job_id, note };
    std::lock_guard ek(wr.ev_mtx);
    wr.events.push(e);
}

void WorkerStatusRegistry::RecordHeartbeat(int64_t worker_id) {
    RecordEvent(worker_id, WorkerEventKind::Heartbeat);
    std::shared_lock rk(mtx_);
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) return;
    it->second.last_heartbeat_mono_ns = NowMonoNs();
}

void WorkerStatusRegistry::RecordDbSuccess(int64_t worker_id) {
    std::shared_lock rk(mtx_);
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) return;
    it->second.last_successful_db_call_mono_ns = NowMonoNs();
    it->second.consecutive_failures = 0;
}

void WorkerStatusRegistry::RecordProgress(int64_t worker_id, const std::string& text, std::optional<int64_t> job_id) {
    std::shared_lock rk(mtx_);
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) return;
    auto& wr = it->second;
    const auto now = NowMonoNs();
    wr.last_progress = text;
    wr.last_progress_mono_ns = now;
    WorkerEvent e{ now, WorkerEventKind::Progress, job_id, text };
    std::lock_guard ek(wr.ev_mtx);
    wr.events.push(e);
}

void WorkerStatusRegistry::RecordError(int64_t worker_id, const std::string& err) {
    std::shared_lock rk(mtx_);
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) return;
    auto& wr = it->second;
    const auto now = NowMonoNs();
    wr.last_error = err;
    wr.last_error_mono_ns = now;
    wr.consecutive_failures += 1;
    WorkerEvent e{ now, WorkerEventKind::Error, std::nullopt, err };
    std::lock_guard ek(wr.ev_mtx);
    wr.events.push(e);
}

WorkerStateKind WorkerStatusRegistry::GetWorkerState(int64_t worker_id) const {
    std::shared_lock rk(mtx_);
    auto it = workers_.find(worker_id);
    if (it == workers_.end()) return WorkerStateKind::Dead;
    return it->second.state;
}

std::vector<WorkerSnapshot> WorkerStatusRegistry::GetClusterSnapshot() const {
    std::vector<WorkerSnapshot> out;
    std::shared_lock rk(mtx_);
    out.reserve(workers_.size());
    for (auto const& kv : workers_) {
        auto& wr = kv.second;
        WorkerSnapshot s;
        s.worker_id = wr.worker_id;
        s.host = wr.host;
        s.pid = wr.pid;
        s.boot_uuid = wr.boot_uuid;

        s.state = wr.state;
        s.job_id = wr.job_id;
        s.program_kind = wr.program_kind;
        s.lease_expires_at = wr.lease_expires_at;
        s.attempts = wr.attempts;
        s.max_attempts = wr.max_attempts;

        s.start_time_utc = wr.start_time_utc;
        s.last_state_change_mono_ns = wr.last_state_change_mono_ns;
        s.last_heartbeat_mono_ns = wr.last_heartbeat_mono_ns;
        s.last_successful_db_call_mono_ns = wr.last_successful_db_call_mono_ns;
        s.consecutive_failures = wr.consecutive_failures;
        s.last_error = wr.last_error;
        s.last_error_mono_ns = wr.last_error_mono_ns;
        s.last_progress = wr.last_progress;
        s.last_progress_mono_ns = wr.last_progress_mono_ns;

        std::lock_guard ek(wr.ev_mtx);
        s.recent_events = wr.events.snapshot();

        out.push_back(std::move(s));
    }
    return out;
}
