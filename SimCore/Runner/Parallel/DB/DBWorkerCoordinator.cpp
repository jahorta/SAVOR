// SimCore/Runner/Scheduling/WorkerCoordinator.cpp
#include "DBWorkerCoordinator.h"

#include <windows.h>
#include <sstream>
#include <mutex>  // for std::once_flag / std::call_once
#include <chrono>
#include <algorithm>

#include "../../../Utils/ThreadName.h"
#include "../../../DB/ProgramDB/IProgramDBCodec.h"
#include "../../../DB/Scheduling/JobsRepo.h"
#include "../../../DB/Scheduling/JobEventsRepo.h"
#include "../../../DB/Scheduling/VisualReplayRepo.h"
#include "../../../DB/Scheduling/VisualReplayEventsRepo.h"
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

    static inline void interrupt_in_flight_jobs_with_event() {
        auto ir = simcore::db::JobsRepo::InterruptInFlight();
        if (!ir.ok) return;
        for (const auto job_id : ir.value) {
            (void)simcore::db::JobEventsRepo::Append(job_id, "INTERRUPTED", std::nullopt);
        }
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
        interrupt_in_flight_jobs_with_event();
        desired_workers_.store(cfg_.desired_workers);

        auto normal = std::make_unique<Slot>();
        normal->id = 1;
        normal->proc = std::make_unique<ProcessWorker>();
        normal->proc->set_progress_queue(&progress_q_);
        normal->running.store(false);
        normal->phase = Slot::Phase::Uninitialized;
        slots_.push_back(std::move(normal));
        slots_[0]->running.store(true);
        slots_[0]->phase = Slot::Phase::PendingStart;
        enqueue_startup_slot(0);
        paused_.store(cfg_.start_to_paused);
        stop_.store(false);
        controller_ = std::thread([this] { set_this_thread_name_utf8("WKC-Controller"); controller_loop(); });
        visual_listener_ = std::thread([this] { set_this_thread_name_utf8("WKC-Visual"); visual_listener_loop(); });
        progress_drainer_ = std::thread([this] { set_this_thread_name_utf8("WKC-Progress"); drain_progress_loop(); });
        results_drainer_ = std::thread([this] { set_this_thread_name_utf8("WKC-Results");  drain_results_loop(); });
    }

    void WorkerCoordinator::stop() {
        if (stop_.exchange(true)) return;
        interrupt_in_flight_jobs_with_event();
        startup_in_flight_slot_.reset();
        startup_queue_.clear();
        visual_slot_cv_.notify_all();
        {
            std::lock_guard<std::mutex> lock(visual_slot_mtx_);
            if (visual_slot_) {
                shutdown_slot(*visual_slot_);
                visual_slot_.reset();
            }
        }
        for (auto& s : slots_) { shutdown_slot(*s); }
        progress_q_.close();
        results_q_.close();
        if (controller_.joinable()) controller_.join();
        if (visual_listener_.joinable()) visual_listener_.join();
        if (progress_drainer_.joinable()) progress_drainer_.join();
        if (results_drainer_.joinable())  results_drainer_.join();
        slots_.clear();
    }

    PRStatus WorkerCoordinator::snapshot_status() const {
        PRStatus st{};
        st.epoch = epoch_.load();
        st.workers = active_slot_count();
        size_t running = 0;
        size_t ready = 0;
        size_t pending = 0;
        size_t dead = 0;
        for (auto& s : slots_) {
            if (!s->running.load()) continue;
            if (s->assigned_job_id.has_value()) ++running;
            if (s->phase == Slot::Phase::Ready) ++ready;
            if (s->phase == Slot::Phase::PendingStart || s->phase == Slot::Phase::StartingProcess || s->phase == Slot::Phase::WaitingReady) ++pending;
            if (s->phase == Slot::Phase::Dead) ++dead;
        }
        st.running_workers = running;
        st.ready_workers = ready;
        st.pending_start_workers = pending;
        st.dead_workers = dead;
        st.starting_workers = startup_in_flight_slot_.has_value() ? 1 : 0;
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
        ps.visual = (s.id == 0);
        ps.render_widget_handle = (s.id == 0) ? visual_render_widget_handle_.load(std::memory_order_relaxed) : 0;
        if (!s.proc->start(ps, &results_q_)) {
            s.dead.store(true);
            s.phase = Slot::Phase::Dead;
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
        s.assigned_visual_replay_id.reset();
        s.phase = Slot::Phase::WaitingReady;
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
        s.phase = Slot::Phase::Dead;
        s.assigned_visual_replay_id.reset();
        UpdateState((int64_t)s.id, WorkerStateKind::Dead);
        UnregisterWorker((int64_t)s.id);
    }

    void WorkerCoordinator::enqueue_startup_slot(size_t slot_id) {
        if (slot_id >= slots_.size()) return;
        if (startup_in_flight_slot_.has_value() && *startup_in_flight_slot_ == slot_id) return;
        const auto found = std::find(startup_queue_.begin(), startup_queue_.end(), slot_id);
        if (found == startup_queue_.end()) startup_queue_.push_back(slot_id);
    }

    void WorkerCoordinator::mark_slot_start_failed(Slot& s, const std::string& err) {
        UpdateState((int64_t)s.id, WorkerStateKind::Stopping);
        SetCurrentJob((int64_t)s.id, std::nullopt, std::nullopt);
        if (s.proc) s.proc->stop();
        UnregisterWorker((int64_t)s.id);
        s.ready.store(false);
        s.startup_attempts += 1;
        s.dead.store(true);
        s.phase = Slot::Phase::Dead;
        s.assigned_visual_replay_id.reset();
        UpdateState((int64_t)s.id, WorkerStateKind::Dead);
        RecordError((int64_t)s.id, err);
        if (s.running.load() && active_slot_count() <= desired_workers_.load()) {
            s.phase = Slot::Phase::PendingStart;
            s.dead.store(false);
            enqueue_startup_slot(s.id);
        }
    }

    size_t WorkerCoordinator::active_slot_count() const {
        size_t active = 0;
        for (const auto& s : slots_) if (s->running.load()) ++active;
        return active;
    }

    void WorkerCoordinator::advance_startup_once() {
        if (!startup_in_flight_slot_.has_value()) {
            while (!startup_queue_.empty()) {
                const size_t candidate = startup_queue_.front();
                startup_queue_.pop_front();
                if (candidate >= slots_.size()) continue;
                auto& s = *slots_[candidate];
                if (!s.running.load()) continue;
                if (s.phase != Slot::Phase::PendingStart && s.phase != Slot::Phase::StartingProcess && s.phase != Slot::Phase::WaitingReady) continue;
                startup_in_flight_slot_ = candidate;
                break;
            }
        }

        if (!startup_in_flight_slot_.has_value()) return;

        auto& s = *slots_[*startup_in_flight_slot_];
        if (!s.running.load()) {
            startup_in_flight_slot_.reset();
            return;
        }

        if (s.phase == Slot::Phase::PendingStart) {
            s.phase = Slot::Phase::StartingProcess;
            if (!spawn_slot(s)) {
                mark_slot_start_failed(s, "startup failed while spawning slot");
                startup_in_flight_slot_.reset();
                return;
            }
            s.startup_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.child_launch_timeout_ms);
            s.next_ready_probe = std::chrono::steady_clock::now();
            return;
        }

        if (s.phase == Slot::Phase::StartingProcess || s.phase == Slot::Phase::WaitingReady) {
            s.phase = Slot::Phase::WaitingReady;
            if (s.proc->is_failed()) {
                mark_slot_start_failed(s, "Child reported failure before ready");
                startup_in_flight_slot_.reset();
                return;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= s.startup_deadline) {
                mark_slot_start_failed(s, "wait_ready timed out");
                startup_in_flight_slot_.reset();
                return;
            }

            if (now < s.next_ready_probe) return;

            if (s.proc->wait_ready(1)) {
                s.ready.store(true);
                s.dead.store(false);
                s.startup_attempts = 0;
                s.phase = Slot::Phase::Ready;
                UpdateState((int64_t)s.id, WorkerStateKind::Idle);
                RecordHeartbeat((int64_t)s.id);
                startup_in_flight_slot_.reset();
                return;
            }

            s.next_ready_probe = now + std::chrono::milliseconds(25);
        }
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

    WorkerCoordinator::DispatchResult WorkerCoordinator::dispatch_one(Slot& s, const simcore::db::JobRow& job, IProgramDBCodec& codec, bool update_job_state) {
        if (!s.proc->try_acquire_slot()) return DispatchResult::NotAvailable;

        auto jobr = codec.decode_job_from_db(job.job_id);
        if (!jobr.ok) { s.proc->release_slot(); RecordError((int64_t)s.id, "dispatch_one failed (decode)"); return DispatchResult::BadDecode; }

        if (!s.proc->send_job((uint64_t)job.job_id, epoch_.load(), jobr.value)) {
            s.proc->release_slot();
            RecordError((int64_t)s.id, "dispatch_one failed (send)");
            return DispatchResult::BadSend;
        }

        // worker status: job assigned
        SetCurrentJob((int64_t)s.id, (int64_t)job.job_id, job.program_kind);
        UpdateState((int64_t)s.id, WorkerStateKind::Running);
        const auto lease_exp = now_sec() + (int64_t)cfg_.lease_seconds;
        SetLeaseInfo((int64_t)s.id, lease_exp, /*attempts*/0, /*max_attempts*/0);
        RecordHeartbeat((int64_t)s.id);

        if (update_job_state) {
            simcore::db::JobsRepo::MarkRunning(job.job_id);
            simcore::db::JobEventsRepo::Append(job.job_id, "DISPATCHED", std::nullopt);
        }

        s.assigned_job_id = job.job_id;
        s.lease_renew_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.heartbeat_interval_ms);
        return DispatchResult::Success;
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

            advance_startup_once();

            // scale up
            if (active_slot_count() < desired_workers_.load()) {
                for (int i = slots_.size(); i < desired_workers_; i++) {
                    auto s = std::make_unique<Slot>();
                    s->id = i + 1;
                    s->proc = std::make_unique<ProcessWorker>();
                    s->proc->set_progress_queue(&progress_q_);
                    s->running.store(false);
                    s->phase = Slot::Phase::Uninitialized;
                    slots_.push_back(std::move(s));
                }
                for (auto& sp : slots_) {
                    auto& s = *sp;
                    if (s.running.load()) continue;
                    s.running.store(true);
                    s.dead.store(false);
                    s.ready.store(false);
                    s.assigned_job_id.reset();
                    s.assigned_visual_replay_id.reset();
                    s.current_program_kind.reset();
                    s.current_savestate_id.reset();
                    s.startup_attempts = 0;
                    s.phase = Slot::Phase::PendingStart;
                    enqueue_startup_slot(s.id);
                    break;
                }
            }

            // scale down (idle-only shrink to avoid preempt)
            while (active_slot_count() > desired_workers_.load()) {
                bool removed = false;
                for (size_t i = slots_.size(); i-- > 0;) {
                    auto& sl = *slots_[i];
                    if (!sl.running.load()) continue;
                    if (sl.assigned_job_id.has_value()) continue;
                    if (startup_in_flight_slot_.has_value() && *startup_in_flight_slot_ == i) continue;
                    sl.phase = Slot::Phase::Stopping;
                    shutdown_slot(sl);
                    sl.phase = Slot::Phase::Uninitialized;
                    sl.running.store(false);
                    sl.dead.store(false);
                    sl.ready.store(false);
                    startup_queue_.erase(std::remove(startup_queue_.begin(), startup_queue_.end(), i), startup_queue_.end());
                        removed = true; break;
                }
                if (!removed) break;
            }

            for (auto& sp : slots_) {
                auto& s = *sp;
                if (!s.running.load()) continue;
                if (s.phase != Slot::Phase::Ready) continue;

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

                auto claimr = simcore::db::JobsRepo::ClaimNextReady(claim_token_, cfg_.lease_seconds, cfg_.aging_factor, s.current_savestate_id);
                if (!claimr.ok) { Sleep(cfg_.controller_sleep_ms); continue; }
                if (!claimr.value.has_value()) { Sleep(cfg_.controller_sleep_ms); continue; }

                auto job = claimr.value.value();
                auto& codec = ProgramDBCodecRegistry::for_kind(job.program_kind);

                if (!ensure_program(s, job.program_kind, job.savestate_id, codec, job.job_id)) {
                    simcore::db::JobsRepo::SetState(job.job_id, "QUEUED");
                    RecordError((int64_t)s.id, "ensure_program failed; requeueing");
                    SetCurrentJob((int64_t)s.id, std::nullopt, std::nullopt);
                    s.phase = Slot::Phase::Dead;
                    s.ready.store(false);
                    mark_slot_start_failed(s, "ensure_program failed; restarting worker");
                    continue;
                }

                auto d_res = dispatch_one(s, job, codec);
                if (d_res == DispatchResult::BadDecode) {
                    simcore::db::JobsRepo::SetState(job.job_id, "FAILED");
                    RecordError((int64_t)s.id, "dispatch_one decode failed.");
                    SetCurrentJob((int64_t)s.id, std::nullopt, std::nullopt);
                    UpdateState((int64_t)s.id, WorkerStateKind::Idle);
                    s.phase = Slot::Phase::Ready;
                } else if (d_res == DispatchResult::BadSend || d_res == DispatchResult::NotAvailable){
                    simcore::db::JobsRepo::SetState(job.job_id, "QUEUED");
                    RecordError((int64_t)s.id, "dispatch_one send failed; requeueing");
                    SetCurrentJob((int64_t)s.id, std::nullopt, std::nullopt);
                    UpdateState((int64_t)s.id, WorkerStateKind::Idle);
                    s.phase = Slot::Phase::Ready;
                    continue;
                }
                else {
                    simcore::db::JobsRepo::SetState(job.job_id, "RUNNING");
                }
            }

            Sleep(cfg_.controller_sleep_ms);
        }
    }

    void WorkerCoordinator::visual_listener_loop() {
        for (;;) {
            if (stop_.load()) break;
            if (paused_.load()) {
                Sleep(cfg_.controller_sleep_ms);
                continue;
            }

            auto claimr = simcore::db::VisualReplayRepo::ClaimNextQueued(/*worker_id*/0);
            if (!claimr.ok) {
                Sleep(cfg_.controller_sleep_ms);
                continue;
            }
            if (!claimr.value.has_value()) {
                Sleep(cfg_.controller_sleep_ms);
                continue;
            }

            const auto replay = *claimr.value;
            (void)simcore::db::VisualReplayEventsRepo::Append(replay.visual_replay_id, "CLAIMED", std::to_string(replay.job_id));
            if (visual_render_widget_handle_.load(std::memory_order_relaxed) == 0) {
                (void)simcore::db::VisualReplayRepo::MarkFailed(replay.visual_replay_id, "visual replay requested without a render widget handle");
                (void)simcore::db::VisualReplayEventsRepo::Append(replay.visual_replay_id, "ERROR", "render widget handle is not set");
                continue;
            }
            auto job_get = simcore::db::JobsRepo::Get(replay.job_id);
            if (!job_get.ok) {
                (void)simcore::db::VisualReplayRepo::MarkFailed(replay.visual_replay_id, "visual replay job not found");
                (void)simcore::db::VisualReplayEventsRepo::Append(replay.visual_replay_id, "ERROR", "job not found");
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(visual_slot_mtx_);
                visual_slot_ = std::make_unique<Slot>();
                visual_slot_->id = 0;
                visual_slot_->proc = std::make_unique<ProcessWorker>();
                visual_slot_->proc->set_progress_queue(&progress_q_);
                visual_slot_->running.store(true);
                visual_slot_->phase = Slot::Phase::PendingStart;
                if (!spawn_slot(*visual_slot_)) {
                    visual_slot_.reset();
                }
            }

            {
                std::lock_guard<std::mutex> lock(visual_slot_mtx_);
                if (!visual_slot_) {
                    (void)simcore::db::VisualReplayRepo::MarkFailed(replay.visual_replay_id, "failed to spawn visual worker");
                    (void)simcore::db::VisualReplayEventsRepo::Append(replay.visual_replay_id, "ERROR", "failed to spawn visual worker");
                    continue;
                }
            }

            bool ready = false;
            {
                std::lock_guard<std::mutex> lock(visual_slot_mtx_);
                if (visual_slot_) ready = ensure_ready(*visual_slot_);
            }
            if (!ready) {
                (void)simcore::db::VisualReplayRepo::MarkFailed(replay.visual_replay_id, "visual worker failed to become ready");
                (void)simcore::db::VisualReplayEventsRepo::Append(replay.visual_replay_id, "ERROR", "visual worker failed to become ready");
                std::lock_guard<std::mutex> lock(visual_slot_mtx_);
                if (visual_slot_) {
                    shutdown_slot(*visual_slot_);
                    visual_slot_.reset();
                }
                continue;
            }

            auto& codec = ProgramDBCodecRegistry::for_kind(job_get.value.program_kind);
            auto psi_res = codec.build_psinit_for_job(job_get.value.job_id);
            if (!psi_res.ok) {
                (void)simcore::db::VisualReplayRepo::MarkFailed(replay.visual_replay_id, "failed to build visual replay program config");
                (void)simcore::db::VisualReplayEventsRepo::Append(replay.visual_replay_id, "ERROR", "failed to build visual replay program config");
                std::lock_guard<std::mutex> lock(visual_slot_mtx_);
                if (visual_slot_) {
                    shutdown_slot(*visual_slot_);
                    visual_slot_.reset();
                }
                continue;
            }

            auto psi = psi_res.value;
            if (psi.default_timeout_ms == 0) psi.default_timeout_ms = 10000;
            bool configured = false;
            {
                std::lock_guard<std::mutex> lock(visual_slot_mtx_);
                if (visual_slot_ && visual_slot_->proc) {
                    configured = visual_slot_->proc->ctl_set_program(
                        static_cast<uint8_t>(job_get.value.program_kind),
                        static_cast<uint8_t>(job_get.value.program_kind),
                        psi)
                        && visual_slot_->proc->ctl_activate_main();
                    if (configured) {
                        visual_slot_->assigned_visual_replay_id = replay.visual_replay_id;
                    }
                }
            }
            if (!configured) {
                (void)simcore::db::VisualReplayRepo::MarkFailed(replay.visual_replay_id, "failed to configure visual worker program");
                (void)simcore::db::VisualReplayEventsRepo::Append(replay.visual_replay_id, "ERROR", "failed to configure visual worker program");
                std::lock_guard<std::mutex> lock(visual_slot_mtx_);
                if (visual_slot_) {
                    shutdown_slot(*visual_slot_);
                    visual_slot_.reset();
                }
                continue;
            }

            DispatchResult dispatch = DispatchResult::BadSend;
            {
                std::lock_guard<std::mutex> lock(visual_slot_mtx_);
                if (visual_slot_) {
                    dispatch = dispatch_one(*visual_slot_, job_get.value, codec, false);
                }
            }
            if (dispatch != DispatchResult::Success) {
                (void)simcore::db::VisualReplayRepo::MarkFailed(replay.visual_replay_id, "failed to dispatch visual replay job");
                (void)simcore::db::VisualReplayEventsRepo::Append(replay.visual_replay_id, "ERROR", "failed to dispatch visual replay job");
                std::lock_guard<std::mutex> lock(visual_slot_mtx_);
                if (visual_slot_) {
                    shutdown_slot(*visual_slot_);
                    visual_slot_.reset();
                }
                continue;
            }

            std::unique_lock<std::mutex> lk(visual_slot_mtx_);
            visual_slot_cv_.wait(lk, [this]() {
                return stop_.load() || !visual_slot_ || !visual_slot_->assigned_job_id.has_value();
                });
            if (visual_slot_) {
                shutdown_slot(*visual_slot_);
                visual_slot_.reset();
            }
            lk.unlock();
            (void)simcore::db::VisualReplayEventsRepo::Append(replay.visual_replay_id, "COMPLETE", std::nullopt);
            visual_slot_cv_.notify_all();
        }
    }

    void WorkerCoordinator::drain_progress_loop() {
        PRProgress p;
        while (progress_q_.pop_wait(p)) {
            if (p.worker_id == 0) {
                std::optional<int64_t> replay_id;
                {
                    std::lock_guard<std::mutex> lock(visual_slot_mtx_);
                    if (visual_slot_ && visual_slot_->assigned_visual_replay_id.has_value()) replay_id = visual_slot_->assigned_visual_replay_id;
                }
                if (replay_id.has_value()) {
                    (void)simcore::db::VisualReplayEventsRepo::Append(*replay_id, "PROGRESS", p.text);
                }
                if (stop_.load()) break;
                continue;
            }
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
            if (r.worker_id == 0) {
                std::optional<int64_t> replay_id;
                {
                    std::lock_guard<std::mutex> lock(visual_slot_mtx_);
                    if (visual_slot_ && visual_slot_->assigned_visual_replay_id.has_value()) replay_id = visual_slot_->assigned_visual_replay_id;
                }
                if (replay_id.has_value()) {
                    auto jr = simcore::db::JobsRepo::Get((int64_t)r.job_id);
                    if (jr.ok) {
                        auto& codec = ProgramDBCodecRegistry::for_kind(jr.value.program_kind);
                        auto ini = codec.build_results_ini_from_prresult((int64_t)r.job_id, r);
                        if (ini.ok) (void)simcore::db::VisualReplayEventsRepo::Append(*replay_id, "RESULTS", ini.value);
                    }
                    (void)simcore::db::VisualReplayEventsRepo::Append(*replay_id, "RESULT_STATUS", r.ps.ok ? "ok" : "error");
                }
            }

            auto jr = simcore::db::JobsRepo::Get((int64_t)r.job_id);
            if (jr.ok) {
                if (r.worker_id == 0) {
                    // Visual replay result stream is stored on visual_replay_events instead of mutating normal job outputs.
                } else {
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
            }

            if (r.worker_id == 0) {
                std::lock_guard<std::mutex> lock(visual_slot_mtx_);
                if (visual_slot_) {
                    if (visual_slot_->assigned_visual_replay_id.has_value()) {
                        if (r.ps.ok) (void)simcore::db::VisualReplayRepo::MarkSucceeded(*visual_slot_->assigned_visual_replay_id);
                        else (void)simcore::db::VisualReplayRepo::MarkFailed(*visual_slot_->assigned_visual_replay_id, "visual replay worker reported failure");
                    }
                    visual_slot_->proc->release_slot();
                    visual_slot_->assigned_job_id.reset();
                    visual_slot_->assigned_visual_replay_id.reset();
                    visual_slot_->idle_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.idle_keepalive_ms);
                    SetCurrentJob((int64_t)visual_slot_->id, std::nullopt, std::nullopt);
                    UpdateState((int64_t)visual_slot_->id, WorkerStateKind::Idle);
                    visual_slot_->phase = Slot::Phase::Ready;
                    RecordHeartbeat((int64_t)visual_slot_->id);
                    visual_slot_cv_.notify_all();
                }
            } else if (r.worker_id < slots_.size()) {
                auto& s = *slots_[(size_t)r.worker_id];
                s.proc->release_slot();
                s.assigned_job_id.reset();
                s.idle_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.idle_keepalive_ms);
                SetCurrentJob((int64_t)s.id, std::nullopt, std::nullopt);
                UpdateState((int64_t)s.id, WorkerStateKind::Idle);
                s.phase = Slot::Phase::Ready;
                RecordHeartbeat((int64_t)s.id);
            }

            if (stop_.load()) break;
        }
    }

    void WorkerCoordinator::set_target_workers(size_t n) { desired_workers_.store((std::min)(n, cfg_.max_concurrent_processes)); }
    void WorkerCoordinator::set_paused(bool p) { 
        paused_.store(p);
        for (auto& sp : slots_) {
            auto& s = *sp;
            if (!s.running.load()) continue;
            if (s.assigned_job_id.has_value()) continue; // don't override busy workers
            UpdateState((int64_t)s.id, p ? WorkerStateKind::Paused : WorkerStateKind::Idle);
        }
    }
    void WorkerCoordinator::SetVisualRenderWidgetHandle(uint64_t hwnd) {
        visual_render_widget_handle_.store(hwnd, std::memory_order_relaxed);
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
