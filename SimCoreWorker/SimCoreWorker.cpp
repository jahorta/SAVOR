// SimCoreWorker.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

// SimCoreWorker/WorkerMain.cpp
#include <string>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#include "Utils/Log.h"
#include "Boot/Boot.h"
#include "Core/DolphinWrapper.h"
#include "Runner/Breakpoints/BPRegistry.h"
#include "Runner/Script/PhaseScriptVM.h"
#include "Runner/Script/PSContextCodec.h"
#include "Runner/Script/KeyRegistry.h"
#include "Phases/Programs/ProgramRegistry.h"
#include "Runner/Parallel/ParallelPhaseScriptRunner.h"
#include "Runner/IPC/Wire.h"

#include <windows.h>
#include <Utils/ThreadName.h>

using namespace simcore;

static std::wstring exe_dir_w() {
    wchar_t buf[MAX_PATH]; GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf); size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : p.substr(0, pos);
}

static uint32_t parse_hex_u32(const char* s) {
    return s ? static_cast<uint32_t>(std::strtoul(s, nullptr, 16)) : 0u;
}
static uint32_t parse_u32(const char* s) {
    return s ? static_cast<uint32_t>(std::strtoul(s, nullptr, 10)) : 0u;
}
static uint64_t parse_u64(const char* s) {
    return s ? static_cast<uint64_t>(std::strtoull(s, nullptr, 10)) : 0ull;
}
static const char* argv_next(int& i, int argc, char** argv) { return (i + 1 < argc) ? argv[++i] : ""; }

enum WorkerExitCode : uint8_t {
    INVALID_HANDLES = 100,
    SEND_READY_FAILED
};

static bool write_all(HANDLE h, const void* p, size_t n) {
    const BYTE* b = static_cast<const BYTE*>(p);
    DWORD w = 0;
    while (n) {
        if (!WriteFile(h, b, (DWORD)std::min(n, (size_t)0x7FFFFFFF), &w, NULL)) 
        {
            SCLOGE("[write] Failed to write");
            return false;
        }
        if (w == 0) 
        {
            SCLOGE("[write] Zero bytes written");
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
        if (!ReadFile(h, b, (DWORD)std::min(n, (size_t)0x7FFFFFFF), &r, NULL)) 
        {
            SCLOGE("[read] Failed to read");
            return false;
        }
        if (r == 0) 
        {
            SCLOGE("[read] Zero bytes read");
            return false;
        }
        b += r; n -= r;
    }
    return true;
}

static bool read_tag(HANDLE h, uint32_t& tag) {
    return read_all(h, &tag, sizeof(tag));
}

static bool read_exact(HANDLE h, void* p, size_t n) { return read_all(h, p, n); }

int main(int argc, char** argv)
{
    // args:
    // --id N --iso <path> --savestate <path> --qtbase <dir> --userdir <dir> [--log <file>]
    size_t worker_id = 0;
    std::string iso, sav, qtbase, userdir, logfile; 
    uint32_t timeout_ms = 10000;
    bool visual = false;
    uint64_t render_hwnd = 0;

    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        if (k == "--id") worker_id = parse_u32(argv_next(i, argc, argv));
        else if (k == "--iso") iso = argv_next(i, argc, argv);
        else if (k == "--qtbase") qtbase = argv_next(i, argc, argv);
        else if (k == "--userdir") userdir = argv_next(i, argc, argv);
        else if (k == "--visual") visual = true;
        else if (k == "--render-hwnd") render_hwnd = parse_u64(argv_next(i, argc, argv));
    }

    set_this_thread_name_utf8((std::string("WorkerMain-") + std::to_string(worker_id)).c_str());

    // Ensure the userdir exists and open a per-worker log file
    std::filesystem::path log_path = std::filesystem::path(userdir) /
        ("worker-" + std::to_string(worker_id) + ".log");
    std::error_code ec;
    std::filesystem::create_directories(log_path.parent_path(), ec);

    auto& L = simcore::logger::Logger::get();
    // File sink: lowest threshold so everything is captured; console is muted
    L.open_file(log_path.string().c_str(), /*append=*/false);
    L.set_levels(simcore::logger::Level::Off, simcore::logger::Level::Debug);

    SCLOGI("[Worker %zu] Initializing", worker_id);

    SCLOGD("[Worker %zu] args iso=%s sav=%s qtbase=%s userdir=%s timeout=%u visual=%d render_hwnd=%llu",
        worker_id, iso.c_str(), sav.c_str(), qtbase.c_str(), userdir.c_str(), timeout_ms, visual ? 1 : 0,
        static_cast<unsigned long long>(render_hwnd));

    // Use inherited anonymous pipes as binary channels
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hIn == NULL || hIn == INVALID_HANDLE_VALUE || hOut == NULL || hOut == INVALID_HANDLE_VALUE) {
        SCLOGE("[Worker %zu] invalid std handles", worker_id);
        return WorkerExitCode::INVALID_HANDLES;
    }

    auto sys_dsp = std::filesystem::path(exe_dir_w()) / "Sys" / "GC" / "dsp_coef.bin";
    if (!std::filesystem::exists(sys_dsp)) {
        std::filesystem::copy(std::filesystem::path(qtbase) / "Sys", std::filesystem::path(exe_dir_w()) / "Sys", std::filesystem::copy_options::recursive);
    }

    if (!std::filesystem::exists(sys_dsp)) {
        WireReady wr{}; wr.tag = MSG_READY; wr.ok = 0; wr.error = WERR_SysMissing;
        (void)write_all(hOut, &wr, sizeof(wr));
        SCLOGE("[Worker %zu] Missing Sys beside exe (%ws). Ensure parent copied from --qtbase.", worker_id, sys_dsp.c_str());
        return WERR_SysMissing;
    }

    BootPlan boot{};
    boot.boot.user_dir = userdir;
    boot.boot.dolphin_qt_base = qtbase;
    boot.boot.force_resync_from_base = true;
    boot.boot.visual = visual;
    boot.boot.render_widget_handle = reinterpret_cast<void*>(render_hwnd);
    boot.boot.save_config_on_success = false;
    boot.iso_path = iso;

    DolphinWrapper host;

    SCLOGD("[Worker %zu] BootDolphinWrapper begin (user_dir=%s qtbase=%s)",
        worker_id, boot.boot.user_dir.c_str(), boot.boot.dolphin_qt_base.c_str());
    std::string err;
    if (!simboot::BootDolphinWrapper(host, boot.boot, &err)) {
        WireReady wr{}; wr.tag = MSG_READY; wr.ok = 0; wr.error = WERR_BootFail;
        (void)write_all(hOut, &wr, sizeof(wr));
        SCLOGE("[Worker %zu] Boot failed: %s", worker_id, err.c_str());
        return WERR_BootFail;
    }
    SCLOGD("[Worker %zu] BootDolphinWrapper ok", worker_id);

    SCLOGD("[Worker %zu] loadGame(%s) begin", worker_id, boot.iso_path.c_str());
    if (!host.loadGame(boot.iso_path)) {
        WireReady wr{}; wr.tag = MSG_READY; wr.ok = 0; wr.error = WERR_LoadGame;
        (void)write_all(hOut, &wr, sizeof(wr));
        SCLOGE("[Worker %zu] loadGame failed: %s", worker_id, boot.iso_path.c_str());
        return WERR_LoadGame;
    }
    SCLOGD("[Worker %zu] loadGame ok", worker_id);

    host.ConfigurePortsStandardPadP1();

    // ----- New control-mode only -----
    BreakpointMap bpmap = bp::BPRegistry::as_map();
    PhaseScriptVM vm(host, bpmap);

    // Advertise "NoProgram" at startup
    {
        WireReady wrdy{}; wrdy.tag = MSG_READY; wrdy.ok = 1; wrdy.state = WSTATE_NoProgram; wrdy.error = WERR_None;
        if (!write_all(hOut, &wrdy, sizeof(wrdy))) {
            SCLOGE("[Worker %zu] failed to write READY(NoProgram)", worker_id);
            return SEND_READY_FAILED;
        }
        SCLOGD("[Worker %zu] READY(NoProgram) sent", worker_id);
    }

    PhaseScript init_prog{};
    PhaseScript main_prog{};
    PSInit psinit{};            // savestate_path may be empty now
    psinit.default_timeout_ms = timeout_ms;
    bool main_active = false;
    uint8_t active_pk;

    for (;;) {
        uint32_t tag = 0;
        if (!read_tag(hIn, tag)) break;

        if (tag == MSG_SET_PROGRAM) {
            WireSetProgram sp{}; sp.tag = tag;
            if (!read_all(hIn, reinterpret_cast<uint8_t*>(&sp) + sizeof(sp.tag),
                sizeof(sp) - sizeof(sp.tag))) break;

            psinit.default_timeout_ms = sp.timeout_ms;
            psinit.savestate_path = sp.savestate_path;
            psinit.derived_buffer_type = (simcore::DBuf)sp.buff_kind;

            active_pk = sp.main_kind;

            main_prog = simcore::programs::build_main_program(active_pk);
            main_active = false;

            WireAck ack{}; ack.tag = MSG_ACK; ack.ok = 1; ack.code = 'S';
            (void)write_all(hOut, &ack, sizeof(ack));
            SCLOGD("[Worker %zu] SET_PROGRAM ok (init=%u main=%u, sav='%s', to=%u)",
                worker_id, sp.init_kind, sp.main_kind, psinit.savestate_path.c_str(), psinit.default_timeout_ms);
        }
        else if (tag == MSG_RUN_INIT_ONCE) {
            if (!init_prog.ops.empty()) {
                if (!vm.init(psinit, init_prog)) {
                    WireAck ack{}; ack.tag = MSG_ACK; ack.ok = 0; ack.code = 'I';
                    (void)write_all(hOut, &ack, sizeof(ack));
                    SCLOGE("[Worker %zu] VM init failed for INIT", worker_id);
                    continue;
                }
                (void)vm.run(PSJob{}); // single-shot
            }
            WireAck ack{}; ack.tag = MSG_ACK; ack.ok = 1; ack.code = 'I';
            (void)write_all(hOut, &ack, sizeof(ack));
            SCLOGD("[Worker %zu] RUN_INIT_ONCE ok", worker_id);
        }
        else if (tag == MSG_ACTIVATE_MAIN) {
            if (!vm.init(psinit, main_prog)) {
                WireAck ack{}; ack.tag = MSG_ACK; ack.ok = 0; ack.code = 'A';
                (void)write_all(hOut, &ack, sizeof(ack));
                SCLOGE("[Worker %zu] VM init failed for MAIN", worker_id);
                continue;
            }
            main_active = true;
            WireAck ack{}; ack.tag = MSG_ACK; ack.ok = 1; ack.code = 'A';
            (void)write_all(hOut, &ack, sizeof(ack));
            SCLOGD("[Worker %zu] ACTIVATE_MAIN ok", worker_id);
        }
        else if (tag == MSG_JOB) {
            // Read header
            WireJobHeader jh{};
            if (!read_exact(hIn, &jh, sizeof(jh))) break;

            if (jh.tag != tag) break;

            // Read payload bytes
            std::vector<uint8_t> payload(jh.payload_len);
            if (jh.payload_len) {
                if (!read_all(hIn, payload.data(), payload.size())) break;
            }

            WireResult wr{}; wr.tag = MSG_RESULT; wr.job_id = jh.job_id; wr.epoch = jh.epoch; wr.err = WERR_None;

            if (!main_active) {
                wr.ok = 0;
                wr.err = WERR_NoProgramLoaded;
                (void)write_all(hOut, &wr, sizeof(wr));
                SCLOGD("[Worker %zu] JOB before ACTIVATE_MAIN -> NoProgram", worker_id);
                continue;
            }

            // Decode by active program via registry (worker stays ignorant of tag semantics)
            PSJob pj{};
            pj.payload = std::move(payload);
            bool decode_ok = simcore::programs::decode_payload_for(/*active program kind*/ active_pk, pj.payload, pj.ctx);
            if (!decode_ok) {
                wr.ok = 0;
                wr.err = WERR_DecodePayloadFail;
                (void)write_all(hOut, &wr, sizeof(wr));
                SCLOGE("[Worker %zu] payload decode failed for active program", worker_id);
                continue;
            }

            pj.ctx[keys::core::GAME_ISO_PATH] = boot.iso_path;

            auto progress_sink = [hOut, jh](const char* text, bool record = true)
                {
                    WireProgress wp{};
                    wp.tag = MSG_PROGRESS;
                    wp.job_id = jh.job_id;
                    wp.record_progress = record;
                    std::memset(wp.text, 0, sizeof(wp.text));
                    if (text && *text)
                        std::strncpy(wp.text, text, sizeof(wp.text) - 1);

                    (void)write_all(hOut, &wp, sizeof(wp));
                };

            uint32_t progress_flags;
            pj.ctx.get(keys::core::PROGRESS_CORE_FLAGS, progress_flags);

            if (progress_flags != 0)
                host.setProgressSink(progress_sink);

            // Run
            auto R = vm.run(pj);

            // --- clear sink after job ---
            if (progress_flags != 0)
                host.setProgressSink(nullptr);

            // Encode numeric context
            std::vector<uint8_t> blob;
            bool enc_ok = simcore::psctx::encode_numeric(R.ctx, blob);

            // Transport envelope
            wr.ok = (R.ok && enc_ok) ? 1 : 0;
            if (!enc_ok) wr.err = WERR_EncodePayloadFail;

            wr.ctx_len = static_cast<uint32_t>(blob.size());

            // Send tag, then send full header, then blob (if any)
            (void)write_all(hOut, &wr, sizeof(wr));
            if (wr.ctx_len) (void)write_all(hOut, blob.data(), blob.size());
        }
        else {
            SCLOGD("[Worker %zu] unknown tag=%u (closing)", worker_id, tag);
            break;
        }
    }

    return WERR_None;
}
