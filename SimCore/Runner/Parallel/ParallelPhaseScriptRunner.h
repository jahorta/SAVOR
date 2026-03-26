#pragma once
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#include "../Script/PhaseScriptVM.h"
#include "../Breakpoints/BPRegistry.h"
#include "../../Boot/Boot.h"
#include "TSQueue.h"
#include "PRTypes.h"
#include "ProcessWorker.h"

namespace simcore {

    struct BootPlan {
        simboot::BootOptions boot;  // user_dir, dolphin_qt_base, force_p1_standard_pad, etc.
        std::string iso_path;       // game disc to load (no changes after start)
        bool visual = false;
        uint64_t render_widget_handle = 0;
    };

    class ParallelPhaseScriptRunner {
    public:
        ParallelPhaseScriptRunner(size_t workers);
        ~ParallelPhaseScriptRunner();

        bool start(const BootPlan& boot);     // sets epoch=1

        uint64_t submit(const PSJob& job); // enqueues with current epoch, returns job_id
        bool try_get_result(PRResult& out);
        PRStatus status() const;
        void stop();

        bool set_program(uint8_t init_kind, uint8_t main_kind, const PSInit&);  // MSG_SET_PROGRAM to all
        bool run_init_once();                                                   // MSG_RUN_INIT_ONCE to all
        bool activate_main();                                                   // MSG_ACTIVATE_MAIN to all

        inline void increment_epoch() { epoch_.fetch_add(1); for (auto& w : workers_) w->epoch = epoch_.load(); }
        inline void reset_job_ids() { job_seq_.store(0); }

        inline uint32_t worker_count() { return static_cast<uint32_t>(workers_.size()); }

        bool try_get_progress(size_t worker_id, PRProgress& out) const {
            std::lock_guard<std::mutex> lk(progress_m_);
            auto it = last_progress_.find(worker_id);
            if (it == last_progress_.end()) return false;
            out = it->second;
            return true;
        }

    private:
        struct CmdJob { uint64_t job_id; uint64_t epoch; PSJob job; };
        enum class CtrlType { Start, Reconfigure, Shutdown };
        struct CtrlStart { uint64_t epoch; BootPlan boot; PSInit init; PhaseScript program; };
        struct CtrlReconfig { uint64_t epoch; PSInit init; PhaseScript program; };
        struct CtrlCmd { CtrlType type; CtrlStart st; CtrlReconfig rc; };

        struct Worker {
            Worker() = default;

            size_t id{ 0 };
            std::unique_ptr<ProcessWorker> proc;  // child-process wrapper
            std::thread th;
            uint64_t epoch{ 0 };
            std::atomic<bool> running{ false };
            std::atomic<uint64_t> jobs_done{ 0 };
            TSQueue<CmdJob>* jobs{ nullptr };
            TSQueue<PRResult>* out{ nullptr };
        };

        std::vector<std::unique_ptr<TSQueue<CtrlCmd>>> ctrls_;
        std::unique_ptr<TSQueue<CmdJob>> jobs_;
        std::unique_ptr<TSQueue<PRResult>> out_;
        std::vector<std::unique_ptr<Worker>> workers_;
        std::atomic<bool> stop_{ false };
        std::atomic<uint64_t> job_seq_{ 0 };
        std::atomic<uint64_t> epoch_{ 0 };

        std::unordered_map<size_t, PRProgress> last_progress_;
        mutable std::mutex progress_m_;
    };

} // namespace simcore
