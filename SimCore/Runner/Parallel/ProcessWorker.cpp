#include "ProcessWorker.h"
#include "../IPC/Wire.h"
#include <sstream>
#include <filesystem>
#include "../../Utils/ThreadName.h"
#include "../Script/KeyRegistry.h"
#include "../Script/PSContextCodec.h"

namespace simcore {
    static std::string make_visual_control_pipe_name(size_t worker_id)
    {
        std::ostringstream oss;
        oss << "\\\\.\\pipe\\soasim_visual_" << GetCurrentProcessId() << "_" << worker_id;
        return oss.str();
    }

    static bool CreateChild(const ProcStartParams& p,
        HANDLE& hInWrite, HANDLE& hOutRead,
        HANDLE& hProcess, HANDLE& hThread,
        unsigned long& dwProcessId, HANDLE& hJobOut)
    {
        SECURITY_ATTRIBUTES saAttr{ sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

        HANDLE hOutReadTmp = NULL, hOutWrite = NULL;
        if (!CreatePipe(&hOutReadTmp, &hOutWrite, &saAttr, 0)) return false;
        if (!SetHandleInformation(hOutReadTmp, HANDLE_FLAG_INHERIT, 0)) return false;

        HANDLE hInRead = NULL, hInWriteTmp = NULL;
        if (!CreatePipe(&hInRead, &hInWriteTmp, &saAttr, 0)) return false;
        if (!SetHandleInformation(hInWriteTmp, HANDLE_FLAG_INHERIT, 0)) return false;

        STARTUPINFOA si{}; si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = hInRead;
        si.hStdOutput = hOutWrite;
        si.hStdError = hOutWrite;

        // Command line for worker
        std::ostringstream cmd;
        cmd << "\"" << p.exe_path << "\""
            << " --worker"
            << " --id " << p.worker_id
            << " --iso \"" << p.iso_path << "\""
            << " --userdir \"" << p.user_dir << "\""
            << " --vmctrl";
        if (!p.dolphin_base_dir.empty()) {
            cmd << " --qtbase \"" << p.dolphin_base_dir << "\"";
        }
        if (p.visual) {
            cmd << " --visual";
        }
        if (p.render_widget_handle != 0) {
            cmd << " --render-hwnd " << p.render_widget_handle;
        }
        if (p.visual) {
            if (!p.visual_control_pipe_name.empty()) {
                cmd << " --visual-control-pipe \"" << p.visual_control_pipe_name << "\"";
            }
            if (!p.visual_host_events_pipe_name.empty()) {
                cmd << " --visual-host-events-pipe \"" << p.visual_host_events_pipe_name << "\"";
            }
            if (!p.visual_screenshot_dir.empty()) {
                cmd << " --visual-screenshot-dir \"" << p.visual_screenshot_dir << "\"";
            }
        }

        PROCESS_INFORMATION pi{};
        std::string cmdline = cmd.str();
        BOOL ok = CreateProcessA(
            NULL, cmdline.data(), NULL, NULL, TRUE,
            CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
        
        // Close handles not needed by parent
        CloseHandle(hOutWrite);
        CloseHandle(hInRead);

        if (!ok) {
            CloseHandle(hOutReadTmp); 
            CloseHandle(hInWriteTmp);
            return false;
        }

        // Create a Job and assign the child so we can kill the whole subtree later
        HANDLE hJob = CreateJobObjectA(NULL, NULL);
        if (hJob) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)) ||
                !AssignProcessToJobObject(hJob, pi.hProcess))
            {
                // If job setup fails, just close it; we'll fall back to TerminateProcess
                CloseHandle(hJob);
                hJob = NULL;
            }
        }

        hOutRead = hOutReadTmp;
        hInWrite = hInWriteTmp;
        hProcess = pi.hProcess;
        hThread = pi.hThread;
        dwProcessId = pi.dwProcessId;
        hJobOut = hJob;
        return true;
    }

    ProcessWorker::~ProcessWorker() { stop(); }

    bool ProcessWorker::start(ProcStartParams& p, TSQueue<PRResult>* outq)
    {
        out_ = outq;
        id_ = p.worker_id;
        visual_control_pipe_name_ = p.visual_control_pipe_name.empty() && p.visual
            ? make_visual_control_pipe_name(p.worker_id)
            : p.visual_control_pipe_name;
        visual_host_events_pipe_name_ = p.visual_host_events_pipe_name;
        p.visual_control_pipe_name = visual_control_pipe_name_;
        p.visual_host_events_pipe_name = visual_host_events_pipe_name_;
        {
            std::lock_guard<std::mutex> lock(visual_pipe_m_);
            if (hVisualControlPipe_ != INVALID_HANDLE_VALUE) {
                CloseHandle(hVisualControlPipe_);
                hVisualControlPipe_ = INVALID_HANDLE_VALUE;
            }
        }
        if (!CreateChild(p, hChildStd_IN_Wr, hChildStd_OUT_Rd, hProcess, hThread, dwProcessId, hJob))
            return false;

        running_.store(true);
        ready_received_.store(false);
        ready_ok_.store(false);
        ready_error_.store(0);
        busy_.store(false);

        reader_ = std::thread(&ProcessWorker::reader_thread, this);

        return true;
    }

    static bool write_all(HANDLE h, const void* p, size_t n) {
        const BYTE* b = static_cast<const BYTE*>(p);
        DWORD w = 0;
        while (n) {
            if (!WriteFile(h, b, (DWORD)std::min(n, (size_t)0x7FFFFFFF), &w, NULL)) 
            {
                return false;
            }
            if (w == 0) 
            {
                return false;
            }
            b += w; n -= w;
        }
        return true;
    }

    static bool read_all(HANDLE h, void* p, size_t n) {
        BYTE* b = static_cast<BYTE*>(p);
        DWORD r = 0;
        while (n) {
            if (!ReadFile(h, b, (DWORD)std::min(n, (size_t)0x7FFFFFFF), &r, NULL)) return false;
            if (r == 0) return false;
            b += r; n -= r;
        }
        return true;
    }

    static bool write_job_envelope(HANDLE h,
        uint64_t job_id,
        uint64_t epoch,
        const std::vector<uint8_t>& payload)
    {
        WireJobHeader hdr{};
        hdr.tag = MSG_JOB;
        hdr.job_id = job_id;
        hdr.epoch = static_cast<uint32_t>(epoch);
        hdr.payload_len = static_cast<uint32_t>(payload.size());
        if (!write_all(h, &hdr.tag, sizeof(hdr.tag))) return false;
        if (!write_all(h, &hdr, sizeof(hdr))) return false;
        if (hdr.payload_len) {
            if (!write_all(h, payload.data(), payload.size())) return false;
        }
        return true;
    }

    template <size_t N>
    static inline bool copy_cstr_nt(char(&dst)[N], std::string_view src)
    {
        static_assert(N > 0, "dst must have space for NUL");
        const size_t n = (src.size() < (N - 1)) ? src.size() : (N - 1);
        if (n) std::memcpy(dst, src.data(), n);
        dst[n] = '\0';
        return n == src.size(); // true if not truncated
    }

    bool ProcessWorker::ctl_set_program(uint8_t init_kind, uint8_t main_kind, const PSInit& init) {
        WireSetProgram sp{};
        sp.tag = MSG_SET_PROGRAM;
        sp.init_kind = init_kind;
        sp.main_kind = main_kind;
        sp.timeout_ms = init.default_timeout_ms;
        sp.buff_kind = (uint8_t)init.derived_buffer_type;

        std::memset(sp.savestate_path, 0, sizeof(sp.savestate_path));
        if (!init.savestate_path.empty()) {
            if (!copy_cstr_nt(sp.savestate_path, init.savestate_path)) {
                SCLOGW("[worker %zu] savestate_path truncated", id_);
            }
        }

        ack_.request('S');
        if (!write_all(hChildStd_IN_Wr, &sp, sizeof(sp))) {
            ack_.cancel_all();
            return false;
        }
        // wait for MSG_ACK(code='S') by reader_thread
        return ack_.wait_for(init.default_timeout_ms ? init.default_timeout_ms : 10000);
    }

    bool ProcessWorker::ctl_run_init_once() {
        const uint32_t tag = MSG_RUN_INIT_ONCE;
        ack_.request('I');
        if (!write_all(hChildStd_IN_Wr, &tag, sizeof(tag))) {
            ack_.cancel_all();
            return false;
        }
        return ack_.wait_for(10000);
    }

    bool ProcessWorker::send_visual_control_command(VisualControlCommand command)
    {
        std::string command_line;
        switch (command) {
        case VisualControlCommand::Pause: command_line = "PAUSE\n"; break;
        case VisualControlCommand::Resume: command_line = "RESUME\n"; break;
        case VisualControlCommand::VmStep: command_line = "VM_STEP\n"; break;
        default: return false;
        }

        bool sent_over_pipe = false;
        if (!visual_control_pipe_name_.empty()) {
            std::lock_guard<std::mutex> lock(visual_pipe_m_);
            if (hVisualControlPipe_ == INVALID_HANDLE_VALUE) {
                hVisualControlPipe_ = CreateFileA(
                    visual_control_pipe_name_.c_str(),
                    GENERIC_WRITE,
                    0,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);
            }
            if (hVisualControlPipe_ != INVALID_HANDLE_VALUE) {
                DWORD bytes_written = 0;
                if (WriteFile(hVisualControlPipe_, command_line.data(), static_cast<DWORD>(command_line.size()), &bytes_written, nullptr) &&
                    bytes_written == command_line.size()) {
                    sent_over_pipe = true;
                } else {
                    CloseHandle(hVisualControlPipe_);
                    hVisualControlPipe_ = INVALID_HANDLE_VALUE;
                }
            }
        }

        return sent_over_pipe;
    }

    bool ProcessWorker::visual_pause_emulation()
    {
        return send_visual_control_command(VisualControlCommand::Pause);
    }

    bool ProcessWorker::visual_resume_emulation()
    {
        return send_visual_control_command(VisualControlCommand::Resume);
    }

    bool ProcessWorker::visual_step_vm()
    {
        return send_visual_control_command(VisualControlCommand::VmStep);
    }

    bool ProcessWorker::ctl_activate_main() {
        const uint32_t tag = MSG_ACTIVATE_MAIN;
        ack_.request('A');
        if (!write_all(hChildStd_IN_Wr, &tag, sizeof(tag))) {
            ack_.cancel_all();
            return false;
        }
        return ack_.wait_for(10000);
    }

    bool ProcessWorker::send_job(uint64_t job_id, uint64_t epoch, const PSJob& job)
    {
        if (!running_.load()) return false;

        // PSJob now owns the already-encoded payload bytes (first byte == PK_*)
        if (!write_job_envelope(hChildStd_IN_Wr, job_id, epoch, job.payload))
        {
            release_slot();
            return false;
        }
        return true;
    }

    bool ProcessWorker::wait_ready(uint32_t timeout_ms)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms ? timeout_ms : 10000);
        while (std::chrono::steady_clock::now() < deadline) {
            if (ready_received_.load()) {
                return ready_ok_.load();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

    void ProcessWorker::reader_thread()
    {
        set_this_thread_name_utf8((std::string("WorkerReader-") + std::to_string(id_)).c_str());

        for (;;) {
            uint8_t tag = 0;
            DWORD got = 0;
            if (!ReadFile(hChildStd_OUT_Rd, &tag, 1, &got, NULL) || got == 0) break;

            if (tag == MSG_READY) {
                WireReady wrdy{}; wrdy.tag = tag;
                if (!read_all(hChildStd_OUT_Rd, reinterpret_cast<char*>(&wrdy) + 1, sizeof(wrdy) - 1)) break;
                ready_ok_.store(wrdy.ok != 0);
                ready_error_.store(wrdy.error);
                ready_received_.store(true);
                continue;
            }

            if (tag == MSG_ACK) {
                WireAck ack{};
                ack.tag = tag;
                if (!read_all(hChildStd_OUT_Rd, reinterpret_cast<char*>(&ack) + 1, sizeof(ack) - 1)) break;
                // Route to waiter by code
                ack_.fulfill(static_cast<char>(ack.code), ack.ok != 0);
                continue;
            }

            if (tag == MSG_RESULT) {
                WireResult wr{}; wr.tag = tag;
                if (!read_all(hChildStd_OUT_Rd, reinterpret_cast<char*>(&wr) + 1, sizeof(wr) - 1))
                { 
                    running_.store(false); 
                    break; 
                }

                std::vector<uint8_t> blob; blob.resize(wr.ctx_len);
                if (wr.ctx_len && !read_all(hChildStd_OUT_Rd, blob.data(), wr.ctx_len)) 
                { 
                    running_.store(false); 
                    break; 
                }

                release_slot();

                PRResult r{};
                r.worker_id = id_; 
                r.job_id = wr.job_id; 
                r.epoch = wr.epoch;
                r.accepted = true; 
                r.ps.ok = (wr.ok != 0);
                r.ps.w_err = wr.err;

                if (wr.ctx_len) simcore::psctx::decode_numeric(blob.data(), blob.size(), r.ps.ctx);

                out_->push(std::move(r));
                continue;
            }

            if (tag == MSG_PROGRESS)
            {
                WireProgress wp{}; wp.tag = tag;

                // We already consumed 1 byte; read the remaining 103 bytes
                if (!read_all(hChildStd_OUT_Rd, reinterpret_cast<char*>(&wp) + 1, sizeof(wp) - 1))
                {
                    running_.store(false);
                    break;
                }

                PRProgress p{};
                p.worker_id = id_;
                p.job_id = wp.job_id;
                p.text.assign(wp.text, strnlen(wp.text, sizeof(wp.text)));

                {
                    std::lock_guard<std::mutex> lk(progress_m_);
                    last_progress_ = p;
                    have_progress_ = true;
                }

                if (progress_out_ && wp.record_progress)
                    progress_out_->push(std::move(p));

                continue;
            }

            // ignore unknown tags
        }

        // Ensure any waiters are released
        ack_.cancel_all();
        release_slot();
        running_.store(false);
    }

    void ProcessWorker::stop()
    {
        if (!running_.exchange(false)) return;

        // kill first (so reader unblocks), then join/close
        if (hJob) {
            TerminateJobObject(hJob, /*exit_code*/1);
        }
        else if (hProcess) {
            TerminateProcess(hProcess, /*exit_code*/1);
        }

        if (reader_.joinable()) reader_.join();

        if (hChildStd_IN_Wr) { CloseHandle(hChildStd_IN_Wr); hChildStd_IN_Wr = NULL; }
        if (hChildStd_OUT_Rd) { CloseHandle(hChildStd_OUT_Rd); hChildStd_OUT_Rd = NULL; }

        if (hProcess) { 
            WaitForSingleObject(hProcess, 2000);
            CloseHandle(hProcess); hProcess = NULL; 
        }

        if (hJob) { CloseHandle(hJob); hJob = NULL; }
        if (hThread) { CloseHandle(hThread); hThread = NULL; }
        {
            std::lock_guard<std::mutex> lock(visual_pipe_m_);
            if (hVisualControlPipe_ != INVALID_HANDLE_VALUE) {
                CloseHandle(hVisualControlPipe_);
                hVisualControlPipe_ = INVALID_HANDLE_VALUE;
            }
        }

        ack_.cancel_all();
    }

} // namespace simcore
