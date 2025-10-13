// SimCore/Runner/Scheduling/WorkerCoordinator.cpp
#include "DBWorkerCoordinator.h"

#include <windows.h>
#include <sstream>
#include <mutex>  // for std::once_flag / std::call_once
#include <chrono>

#include "../../../Utils/ThreadName.h"
#include "../../../DB/ProgramDB/IProgramDBCodec.h"
#include "../../../DB/Scheduling/JobsRepo.h"
#include "../../../DB/Scheduling/JobEventsRepo.h"
#include "../../../DB/DBCore/ObjectStore.h"
#include "../../IPC/Wire.h"
#include "DBTriggerEngine.h"
#include "../../../Utils/ModulePath.h"

namespace simcore {

    static inline std::string make_claim_token() {
        std::ostringstream os;
        os << "WKC-" << GetCurrentProcessId() << "-" << GetCurrentThreadId();
        return os.str();
    }

    static inline int64_t now_sec() {
        using namespace std::chrono;
        return (int64_t)duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    }

    WorkerCoordinator::WorkerCoordinator(const WorkerCoordinatorConfig& cfg)
        : cfg_(cfg), claim_token_(make_claim_token()) {
        auto base_path = utils::getExecutablePath();
        cfg_.worker_exe_path = (base_path / "SimCoreWorker.exe").string();
        cfg_.worker_dir_root = (base_path / ".workers").string();
    }

    WorkerCoordinator::~WorkerCoordinator() { stop(); }

    void WorkerCoordinator::start() {
        if (!slots_.empty()) return;
        const size_t n = cfg_.max_concurrent_processes;
        desired_workers_.store(cfg_.max_concurrent_processes);
        slots_.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            auto s = std::make_unique<Slot>();
            s->id = i;
            s->proc = std::make_unique<ProcessWorker>();
            s->proc->set_progress_queue(&progress_q_);
            s->running.store(true);
            spawn_slot(*s);
            slots_.push_back(std::move(s));
        }
        paused_.store(cfg_.start_to_paused);
        stop_.store(false);
        controller_ = std::thread([this] { set_this_thread_name_utf8("WKC-Controller"); controller_loop(); });
        progress_drainer_ = std::thread([this] { set_this_thread_name_utf8("WKC-Progress"); drain_progress_loop(); });
        results_drainer_ = std::thread([this] { set_this_thread_name_utf8("WKC-Results");  drain_results_loop(); });
    }

    void WorkerCoordinator::stop() {
        if (stop_.exchange(true)) return;
        for (auto& s : slots_) { shutdown_slot(*s); }
        progress_q_.close();
        results_q_.close();
        if (controller_.joinable()) controller_.join();
        if (progress_drainer_.joinable()) progress_drainer_.join();
        if (results_drainer_.joinable())  results_drainer_.join();
        slots_.clear();
    }

    PRStatus WorkerCoordinator::snapshot_status() const {
        PRStatus st{};
        st.epoch = epoch_.load();
        st.workers = slots_.size();
        size_t running = 0;
        for (auto& s : slots_) if (s->assigned_job_id.has_value()) ++running;
        st.running_workers = running;
        return st;
    }

    bool WorkerCoordinator::spawn_slot(Slot& s) {
        ProcStartParams ps{};
        ps.worker_id = s.id;
        ps.exe_path = cfg_.worker_exe_path;
        ps.iso_path = cfg_.iso_path;
        ps.dolphin_base_dir = cfg_.dolphin_base_dir;
        {
            std::ostringstream ud;
            ud << cfg_.worker_dir_root << "\\worker-" << s.id << "\\User";
            ps.user_dir = ud.str();
        }
        ps.vm_control = true;
        if (!s.proc->start(ps, &results_q_)) {
            s.dead.store(true);
            RecordError((int64_t)s.id, "ProcessWorker.start failed");
            UpdateState((int64_t)s.id, WorkerStateKind::Dead);
            return false;
        }
        RegisterWorker((int64_t)s.id, "localhost", /*pid*/ s.proc->GetPid(), /*boot_uuid*/ "");
        UpdateState((int64_t)s.id, WorkerStateKind::Spawning);
        RecordHeartbeat((int64_t)s.id);
        s.ready.store(false);
        s.current_program_kind.reset();
        s.current_savestate_id.reset();
        s.idle_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.idle_keepalive_ms);
        return true;
    }

    void WorkerCoordinator::shutdown_slot(Slot& s) {
        UpdateState((int64_t)s.id, WorkerStateKind::Stopping);
        SetCurrentJob((int64_t)s.id, std::nullopt, std::nullopt);
        s.running.store(false);
        if (s.proc) s.proc->stop();
        s.ready.store(false);
        s.dead.store(true);
        UpdateState((int64_t)s.id, WorkerStateKind::Dead);
        UnregisterWorker((int64_t)s.id);
    }

    bool WorkerCoordinator::ensure_ready(Slot& s) {
        if (s.ready.load()) return true;
        if (s.proc->is_failed()) {
            shutdown_slot(s);
            spawn_slot(s);
            RecordError((int64_t)s.id, "Child reported failure before ready");
            UpdateState((int64_t)s.id, WorkerStateKind::Dead);
            return false;
        }
        if (!s.proc->wait_ready(cfg_.child_launch_timeout_ms)) {
            shutdown_slot(s);
            spawn_slot(s);
            RecordError((int64_t)s.id, "wait_ready timed out");
            UpdateState((int64_t)s.id, WorkerStateKind::Dead);
            return false;
        }
        s.ready.store(true);
        UpdateState((int64_t)s.id, WorkerStateKind::Idle);
        RecordHeartbeat((int64_t)s.id);
        return true;
    }

    bool WorkerCoordinator::ensure_program(Slot& s, int program_kind, std::optional<int64_t> required_savestate_id, IProgramDBCodec& codec, int64_t job_id, uint32_t default_timeout_ms) {
        const bool need_kind = (!s.current_program_kind.has_value()) || (*s.current_program_kind != program_kind);
        const bool need_sav = (s.current_savestate_id != required_savestate_id);
        const bool reuse_ok = std::chrono::steady_clock::now() < s.idle_deadline;
        if (!need_kind && !need_sav && reuse_ok) return true;

        auto psinit_res = codec.build_psinit_for_job(job_id);
        if (!psinit_res.ok) return false;
        auto psi = psinit_res.value;
        if (psi.default_timeout_ms == 0) psi.default_timeout_ms = default_timeout_ms;

        if (!s.proc->ctl_set_program(static_cast<uint8_t>(program_kind), static_cast<uint8_t>(program_kind), psi)) 
        {
            RecordError((int64_t)s.id, "Failed to set program");
            return false;
        }
        if (!s.proc->ctl_activate_main()) 
        {
            RecordError((int64_t)s.id, "Failed to activate program");
            return false;
        }

        SetCurrentJob((int64_t)s.id, std::nullopt, program_kind);
        UpdateState((int64_t)s.id, WorkerStateKind::Idle);
        RecordHeartbeat((int64_t)s.id);

        s.current_program_kind = program_kind;
        s.current_savestate_id = required_savestate_id;
        s.idle_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.idle_keepalive_ms);
        return true;
    }

    bool WorkerCoordinator::dispatch_one(Slot& s, const simcore::db::JobRow& job, IProgramDBCodec& codec) {
        if (!s.proc->try_acquire_slot()) return false;

        auto jobr = codec.decode_job_from_db(job.job_id);
        if (!jobr.ok) { s.proc->release_slot(); RecordError((int64_t)s.id, "dispatch_one failed (decode)"); return false; }

        if (!s.proc->send_job((uint64_t)job.job_id, epoch_.load(), jobr.value)) {
            s.proc->release_slot();
            RecordError((int64_t)s.id, "dispatch_one failed (send)");
            return false;
        }

        // worker status: job assigned
        SetCurrentJob((int64_t)s.id, (int64_t)job.job_id, job.program_kind);
        UpdateState((int64_t)s.id, WorkerStateKind::Running);
        const auto lease_exp = now_sec() + (int64_t)cfg_.lease_seconds;
        SetLeaseInfo((int64_t)s.id, lease_exp, /*attempts*/0, /*max_attempts*/0);
        RecordHeartbeat((int64_t)s.id);

        simcore::db::JobsRepo::MarkRunning(job.job_id);
        simcore::db::JobEventsRepo::Append(job.job_id, "DISPATCHED", std::nullopt);

        s.assigned_job_id = job.job_id;
        s.lease_renew_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.heartbeat_interval_ms);
        return true;
    }

    void WorkerCoordinator::renew_lease_if_due(int64_t job_id, Slot& s) {
        if (std::chrono::steady_clock::now() < s.lease_renew_deadline) return;
        simcore::db::JobsRepo::RenewLease(job_id, cfg_.lease_seconds);
        s.lease_renew_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.heartbeat_interval_ms);
        simcore::db::JobEventsRepo::Append(job_id, "LEASE_RENEWED", std::nullopt);
        RecordHeartbeat((int64_t)s.id);
        const auto lease_exp = now_sec() + (int64_t)cfg_.lease_seconds;
        SetLeaseInfo((int64_t)s.id, lease_exp, /*attempts*/0, /*max_attempts*/0);
        RecordDbSuccess((int64_t)s.id);
    }

    void WorkerCoordinator::sweep_expired_leases() {
        simcore::db::JobsRepo::RequeueExpiredLeases();
    }

    void WorkerCoordinator::controller_loop() {
        for (;;) {
            if (stop_.load()) break;

            sweep_expired_leases();

            // scale up
            while (slots_.size() < desired_workers_.load()) {
                auto s = std::make_unique<Slot>();
                s->id = slots_.size();
                s->proc = std::make_unique<ProcessWorker>();
                s->proc->set_progress_queue(&progress_q_);
                s->running.store(true);
                spawn_slot(*s);
                slots_.push_back(std::move(s));
            }
            // scale down (idle-only shrink to avoid preempt)
            while (slots_.size() > desired_workers_.load()) {
                bool removed = false;
                for (size_t i = slots_.size(); i-- > 0; ) {
                    auto& sl = *slots_[i];
                    if (sl.running.load() && !sl.assigned_job_id.has_value()) {
                        shutdown_slot(sl);
                        slots_.erase(slots_.begin() + i);
                        removed = true; break;
                    }
                }
                if (!removed) break;
            }

            for (auto& sp : slots_) {
                auto& s = *sp;
                if (!s.running.load()) continue;
                if (!ensure_ready(s)) continue;

                if (paused_.load()) { 
                    if (!s.assigned_job_id.has_value() && worker_status_.GetWorkerState(s.id) != WorkerStateKind::Paused) {
                        UpdateState((int64_t)s.id, WorkerStateKind::Paused);
                    }
                    Sleep(cfg_.controller_sleep_ms); 
                    continue; 
                }

                if (s.assigned_job_id.has_value()) {
                    renew_lease_if_due(*s.assigned_job_id, s);
                    continue;
                }

                auto claimr = simcore::db::JobsRepo::ClaimNextReady(claim_token_, cfg_.lease_seconds, cfg_.aging_factor);
                if (!claimr.ok) { Sleep(cfg_.controller_sleep_ms); continue; }
                if (!claimr.value.has_value()) { Sleep(cfg_.controller_sleep_ms); continue; }

                auto job = claimr.value.value();
                auto& codec = ProgramDBCodecRegistry::for_kind(job.program_kind);

                auto need_sav = codec.get_required_savestate_id(job.job_id);
                if (!need_sav.ok) { simcore::db::JobsRepo::SetState(job.job_id, "QUEUED"); RecordError((int64_t)s.id, "Missing required savestate"); continue; }

                if (!ensure_program(s, job.program_kind, need_sav.value, codec, job.job_id)) {
                    simcore::db::JobsRepo::SetState(job.job_id, "QUEUED");
                    RecordError((int64_t)s.id, "ensure_program failed; requeueing");
                    SetCurrentJob((int64_t)s.id, std::nullopt, std::nullopt);
                    continue;
                }

                if (!dispatch_one(s, job, codec)) {
                    simcore::db::JobsRepo::SetState(job.job_id, "QUEUED");
                    RecordError((int64_t)s.id, "dispatch_one failed; requeueing");
                    SetCurrentJob((int64_t)s.id, std::nullopt, std::nullopt);
                    UpdateState((int64_t)s.id, WorkerStateKind::Idle);
                    continue;
                }
            }

            Sleep(cfg_.controller_sleep_ms);
        }
    }

    void WorkerCoordinator::drain_progress_loop() {
        PRProgress p;
        while (progress_q_.pop_wait(p)) {
            auto jr = simcore::db::JobsRepo::Get((int64_t)p.job_id);
            if (!jr.ok) continue;
            auto& job = jr.value;
            auto& codec = ProgramDBCodecRegistry::for_kind(job.program_kind);
            (void)codec.encode_progress_into_db(job.job_id, p.text);
            for (auto& sp : slots_) {
                auto& s = *sp;
                if (s.assigned_job_id == (int64_t)p.job_id) {
                    renew_lease_if_due((int64_t)p.job_id, s);
                    RecordHeartbeat((int64_t)s.id);
                    RecordDbSuccess((int64_t)s.id);
                    break;
                }
            }
            if (stop_.load()) break;
        }
    }

    void WorkerCoordinator::drain_results_loop() {
        PRResult r;
        while (results_q_.pop_wait(r)) {
            auto jr = simcore::db::JobsRepo::Get((int64_t)r.job_id);
            if (jr.ok) {
                if (r.worker_id < slots_.size()) {
                    RecordDbSuccess((int64_t)r.worker_id);
                }

                auto& job = jr.value;
                auto& codec = ProgramDBCodecRegistry::for_kind(job.program_kind);

                auto ini = codec.build_results_ini_from_prresult((int64_t)r.job_id, r);
                if (ini.ok) {
                    (void)codec.encode_results_into_db((int64_t)r.job_id, ini.value, r.ps.ok);
                    auto tr = simcore::TriggerEngine::after_terminal(r.job_id);
                    (void)tr; // ignore errors for now; they will be visible in DB events/logs if you add them later
                }
            }

            if (r.worker_id < slots_.size()) {
                auto& s = *slots_[(size_t)r.worker_id];
                s.proc->release_slot();
                s.assigned_job_id.reset();
                s.idle_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.idle_keepalive_ms);
                SetCurrentJob((int64_t)s.id, std::nullopt, std::nullopt);
                UpdateState((int64_t)s.id, WorkerStateKind::Idle);
                RecordHeartbeat((int64_t)s.id);
            }

            if (stop_.load()) break;
        }
    }

    void WorkerCoordinator::set_target_workers(size_t n) { desired_workers_.store(n); }
    void WorkerCoordinator::set_paused(bool p) { 
        paused_.store(p);
        for (auto& sp : slots_) {
            auto& s = *sp;
            if (!s.running.load()) continue;
            if (s.assigned_job_id.has_value()) continue; // don't override busy workers
            UpdateState((int64_t)s.id, p ? WorkerStateKind::Paused : WorkerStateKind::Idle);
        }
    }

    // Worker Status Fxns
    void WorkerCoordinator::RegisterWorker(int64_t worker_id, const std::string& host, int pid, const std::string& boot_uuid) {
        worker_status_.RegisterWorker(worker_id, host, pid, boot_uuid);
    }
    void WorkerCoordinator::UnregisterWorker(int64_t worker_id) {
        worker_status_.UnregisterWorker(worker_id);
    }

    void WorkerCoordinator::UpdateState(int64_t worker_id, WorkerStateKind s) {
        worker_status_.UpdateState(worker_id, s);
    }
    void WorkerCoordinator::SetCurrentJob(int64_t worker_id, std::optional<int64_t> job_id, std::optional<int> program_kind) {
        worker_status_.SetCurrentJob(worker_id, job_id, program_kind);
    }
    void WorkerCoordinator::SetLeaseInfo(int64_t worker_id, std::optional<int64_t> lease_expires_at, int attempts, int max_attempts) {
        worker_status_.SetLeaseInfo(worker_id, lease_expires_at, attempts, max_attempts);
    }

    void WorkerCoordinator::RecordEvent(int64_t worker_id, WorkerEventKind k, std::optional<int64_t> job_id, const std::string& note) {
        worker_status_.RecordEvent(worker_id, k, job_id, note);
    }
    void WorkerCoordinator::RecordHeartbeat(int64_t worker_id) {
        worker_status_.RecordHeartbeat(worker_id);
    }
    void WorkerCoordinator::RecordDbSuccess(int64_t worker_id) {
        worker_status_.RecordDbSuccess(worker_id);
    }
    void WorkerCoordinator::RecordError(int64_t worker_id, const std::string& err) {
        worker_status_.RecordError(worker_id, err);
    }

    std::vector<WorkerSnapshot> WorkerCoordinator::GetClusterSnapshot() const {
        return worker_status_.GetClusterSnapshot();
    }
    void WorkerCoordinator::SetEventBufferCapacity(size_t n) {
        worker_status_.SetEventBufferCapacity(n);
    }

} // namespace simcore
