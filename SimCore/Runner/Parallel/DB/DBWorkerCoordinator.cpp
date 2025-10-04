// SimCore/Runner/Scheduling/WorkerCoordinator.cpp
#include "DBWorkerCoordinator.h"

#include <windows.h>
#include <sstream>
#include <mutex>  // for std::once_flag / std::call_once

#include "../../../Utils/ThreadName.h"
#include "../../../DB/ProgramDB/IProgramDBCodec.h"
#include "../../../DB/ProgramDB/SeedProbeDBCodec.h"
#include "../../../DB/ProgramDB/TasMovieDBCodec.h"
#include "../../../DB/ProgramDB/ExplorerRunDBCodec.h"
#include "../../../DB/ProgramDB/BattleContextDBCodec.h"
#include "../../../DB/Scheduling/JobsRepo.h"
#include "../../../DB/Scheduling/JobEventsRepo.h"
#include "../../../DB/DBCore/ObjectStore.h"
#include "../../IPC/Wire.h"
#include "DBTriggerEngine.h"

namespace {
    void ensure_codecs_registered() {
        static std::once_flag once;
        std::call_once(once, [] {
            static SeedProbeDBCodec  seed_codec;
            static TasMovieDBCodec   tas_codec;
            static ExplorerRunDBCodec battle_runner_codec;
            static BattleContextDBCodec battle_context_codec;

            // Use your existing ProgramKind ids here:
            ProgramDBCodecRegistry::register_codec(simcore::PK_SeedProbe, &seed_codec);
            ProgramDBCodecRegistry::register_codec(simcore::PK_TasMovie, &tas_codec);
            ProgramDBCodecRegistry::register_codec(simcore::PK_BattleTurnRunner, &battle_runner_codec);
            ProgramDBCodecRegistry::register_codec(simcore::PK_BattleContextProbe, &battle_context_codec);
            });
    }
}

namespace simcore {

    static inline std::string make_claim_token() {
        std::ostringstream os;
        os << "WKC-" << GetCurrentProcessId() << "-" << GetCurrentThreadId();
        return os.str();
    }

    WorkerCoordinator::WorkerCoordinator(const WorkerCoordinatorConfig& cfg)
        : cfg_(cfg), claim_token_(make_claim_token()) {
        ensure_codecs_registered();
    }

    WorkerCoordinator::~WorkerCoordinator() { stop(); }

    void WorkerCoordinator::start() {
        if (!slots_.empty()) return;
        const size_t n = cfg_.max_concurrent_processes;
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
        ps.qt_base_dir = cfg_.qt_base_dir;
        {
            std::ostringstream ud;
            ud << cfg_.user_dir_root << "\\runner-" << s.id << "\\User";
            ps.user_dir = ud.str();
        }
        ps.vm_control = true;
        if (!s.proc->start(ps, &results_q_)) {
            s.dead.store(true);
            return false;
        }
        s.ready.store(false);
        s.current_program_kind.reset();
        s.current_savestate_id.reset();
        s.idle_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.idle_keepalive_ms);
        return true;
    }

    void WorkerCoordinator::shutdown_slot(Slot& s) {
        s.running.store(false);
        if (s.proc) s.proc->stop();
        s.ready.store(false);
        s.dead.store(true);
    }

    bool WorkerCoordinator::ensure_ready(Slot& s) {
        if (s.ready.load()) return true;
        if (s.proc->is_failed()) {
            shutdown_slot(s);
            spawn_slot(s);
            return false;
        }
        if (!s.proc->wait_ready(cfg_.child_launch_timeout_ms)) {
            shutdown_slot(s);
            spawn_slot(s);
            return false;
        }
        s.ready.store(true);
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

        if (!s.proc->ctl_set_program(static_cast<uint8_t>(program_kind), static_cast<uint8_t>(program_kind), psi)) return false;
        if (!s.proc->ctl_activate_main()) return false;

        s.current_program_kind = program_kind;
        s.current_savestate_id = required_savestate_id;
        s.idle_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.idle_keepalive_ms);
        return true;
    }

    bool WorkerCoordinator::dispatch_one(Slot& s, const simcore::db::JobRow& job, IProgramDBCodec& codec) {
        if (!s.proc->try_acquire_slot()) return false;

        auto jobr = codec.decode_job_from_db(job.job_id);
        if (!jobr.ok) { s.proc->release_slot(); return false; }

        if (!s.proc->send_job((uint64_t)job.job_id, epoch_.load(), jobr.value)) {
            s.proc->release_slot();
            return false;
        }

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
    }

    void WorkerCoordinator::sweep_expired_leases() {
        simcore::db::JobsRepo::RequeueExpiredLeases();
    }

    void WorkerCoordinator::controller_loop() {
        for (;;) {
            if (stop_.load()) break;

            sweep_expired_leases();

            for (auto& sp : slots_) {
                auto& s = *sp;
                if (!s.running.load()) continue;
                if (!ensure_ready(s)) continue;

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
                if (!need_sav.ok) { simcore::db::JobsRepo::SetState(job.job_id, "QUEUED"); continue; }

                if (!ensure_program(s, job.program_kind, need_sav.value, codec, job.job_id)) {
                    simcore::db::JobsRepo::SetState(job.job_id, "QUEUED");
                    continue;
                }

                if (!dispatch_one(s, job, codec)) {
                    simcore::db::JobsRepo::SetState(job.job_id, "QUEUED");
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
            }

            if (stop_.load()) break;
        }
    }

} // namespace simcore
