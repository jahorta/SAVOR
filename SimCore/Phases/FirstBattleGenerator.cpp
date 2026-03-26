#include "FirstBattleGenerator.h"

#include <filesystem>
#include <unordered_map>
#include <optional>
#include <algorithm>

#include "../Utils/Log.h"
#include "Programs/PlayTasMovie/TasMoviePayload.h"
#include "Programs/PlayTasMovie/TasMovieScript.h"
#include "../Tas/DtmFile.h"
#include "../Runner/IPC/Wire.h"
#include "../Utils/MultiProgress.h"
#include "../Runner/Parallel/ParallelPhaseScriptRunnerApi.h"

namespace fs = std::filesystem;

namespace simcore::tas_movie {

    static std::string basename_no_ext(const std::string& p) {
        fs::path x(p);
        auto stem = x.stem().string();
        return stem;
    }

    static std::string join(const std::string& a, const std::string& b) {
        return (fs::path(a) / b).string();
    }

    // Clone a DTM and adjust its recording start time by delta_sec; return new path.
    static std::optional<std::string> write_dtm_with_delta(const std::string& base_dtm,
        int delta_sec,
        const std::string& out_dir)
    {
        simcore::tas::DtmFile f;
        if (!f.load(base_dtm)) return std::nullopt;
        const auto info = f.info();

        // Name like: <stem>_rtc+0005.dtm (sign-aware, zero-padded 4+)
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), "_rtc%+05d.dtm", delta_sec);
        const std::string out_name = basename_no_ext(base_dtm) + suffix;

        fs::create_directories(out_dir);
        const std::string out_path = join(out_dir, out_name);

        f.set_recording_start_time(info.recording_start_time + static_cast<uint64_t>(delta_sec));
        if (!f.save(out_path)) return std::nullopt;
        return out_path;
    }

    static uint64_t read_vi_total(const std::string& dtm_path) {
        simcore::tas::DtmFile df;
        if (!df.load(dtm_path)) return 0;
        const auto info = df.info();
        return info.vi_count ? info.vi_count : info.input_count;
    }

    BatchResult RunTasMovieOnePerWorkerWithProgress(ParallelPhaseScriptRunner& runner,
        const ConductorArgs& args)
    {
        auto num_workers = runner.worker_count();
        
        BatchResult br{};
        if (args.rtc_delta_hi < args.rtc_delta_lo || num_workers == 0) return br;

        // Boot & program activation (same as conductor)
        PSInit init{};
        init.savestate_path = "";
        init.default_timeout_ms = 600000;

        if (!runner.set_program(/*init_kind=*/PK_None, /*main_kind=*/PK_TasMovie, init)) { SCLOGE("[tas] set_program failed"); return br; }
        if (!runner.activate_main()) { SCLOGE("[tas] activate_main failed"); return br; }

        // Build up to one job per worker using consecutive RTC deltas from [lo..hi]
        const int span = args.rtc_delta_hi - args.rtc_delta_lo + 1;
        const size_t jobs_to_make = static_cast<size_t>(std::min<int>(int(num_workers), span));

        struct JobMeta { int delta; std::string dtm; std::string sav; uint64_t vi_total; };
        std::vector<JobMeta> all_jobs;

        for (int d = args.rtc_delta_lo; d <= args.rtc_delta_hi; ++d) {
            auto dtm_out = write_dtm_with_delta(args.base_dtm, d, args.out_dir);
            if (!dtm_out) { SCLOGW("[tas] skip delta %d: DTM write failed", d); continue; }
            fs::create_directories(args.out_dir);
            fs::path sav_path = fs::path(args.out_dir) / (fs::path(*dtm_out).stem().string() + ".sav");

            JobMeta jm{};
            jm.delta = d;
            jm.dtm = *dtm_out;
            jm.sav = sav_path.string();
            jm.vi_total = read_vi_total(jm.dtm);
            all_jobs.push_back(std::move(jm));
        }

        // Submit them; enable per-job progress in the payload
        std::unordered_map<uint64_t, ItemResult> by_id;
        std::unordered_map<uint64_t, uint64_t>   job_total_vi;
        std::unordered_map<uint64_t, int>        job_delta;

        for (auto& jm : all_jobs) {
            simcore::tasmovie::EncodeSpec spec{};
            spec.dtm_path = jm.dtm;
            spec.run_ms = 0;
            spec.vi_stall_ms = args.vi_stall_ms;
            spec.progress_enable = true; // <- per-job toggle

            std::vector<uint8_t> blob;
            if (!simcore::tasmovie::encode_payload(spec, blob)) {
                SCLOGW("[tas] skip delta %d: payload encode failed", jm.delta);
                continue;
            }

            PSJob job{};
            job.payload = std::move(blob);

            const uint64_t jid = runner.submit(job);

            ItemResult it{};
            it.delta_sec = jm.delta;
            it.dtm_path = jm.dtm;
            it.save_path = jm.sav;
            it.job_id = jid;

            by_id.emplace(jid, std::move(it));
            job_total_vi[jid] = (jm.vi_total ? jm.vi_total : 1);
            job_delta[jid] = jm.delta;

            ++br.submitted;
        }

        // Init multiprogress bars (one per worker)
        simcore::utils::MultiProgress mp;
        {
            std::vector<simcore::utils::MPBarSpec> specs;
            for (size_t w = 0; w < num_workers; ++w)
                specs.push_back({ "w" + std::to_string(w) + " (idle)", 1 });

            simcore::utils::MultiProgress::Options mopt{};
            mopt.use_stdout = true;
            mopt.use_vt = true;
            mopt.min_redraw_sec = 0.10;
            mopt.bar_width = 40;

            mp.init(specs, mopt);
            mp.start();
        }

        // Track which job is running on each worker so we can set label/total once.
        std::vector<uint64_t> worker_job(num_workers, 0);

        size_t done = 0;
        while (done < by_id.size()) {
            // 1) Drain any results first (they are definitive)
            PRResult r{};
            if (runner.try_get_result(r)) {
                auto it = by_id.find(r.job_id);
                if (it != by_id.end()) {
                    it->second.ctx = r.ps.ctx;
                    it->second.ok = r.ps.ok;
                    if (r.ps.ok) ++br.succeeded;
                }
                // Seal the worker's bar as finished.
                if (r.worker_id < worker_job.size()) {
                    mp.setLabel(r.worker_id, std::string("w") + std::to_string(r.worker_id) + " done");
                    mp.setTotal(r.worker_id, 1);
                    mp.advanceTo(r.worker_id, 1);
                    worker_job[r.worker_id] = 0;
                }
                ++done;
                continue;
            }

            // 2) If no result, try to consume a progress snapshot (non-blocking).
            //    Assumes you added try_get_progress(PRProgress&) to the runner.
            bool saw_any_progress = false;
            for (size_t wid = 0; wid < num_workers; ++wid)
            {
                PRProgress p{};
                if (!runner.try_get_progress(wid, p)) continue; // nothing new for this worker
                saw_any_progress = true;

                // If this is a different job than last time on this worker, rebind the bar.
                if (worker_job[wid] != p.job_id && p.job_id != 0)
                {
                    worker_job[wid] = p.job_id;
                    const int d = job_delta.count(p.job_id) ? job_delta[p.job_id] : 0;
                    char lab[32]; std::snprintf(lab, sizeof(lab), "w%zu rtc%+05d", wid, d);
                    mp.setLabel(wid, lab);

                    mp.setTotal(wid, job_total_vi.count(p.job_id) ? job_total_vi[p.job_id] : 1);
                    mp.advanceTo(wid, 0); // reset bar to 0 for the new job
                }

                // Advance using VI frames if available; fallback to elapsed_ms heuristic.
                //uint64_t cur = p.cur_frames ? p.cur_frames : p.elapsed_ms / 16;/*
                //mp.advanceTo(wid, cur);*/
                mp.setSuffix(wid, p.text);
            }

            if (!saw_any_progress) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }

        mp.finish();

        // Move to output vector in delta order
        br.items.reserve(by_id.size());
        for (auto& kv : by_id) br.items.push_back(std::move(kv.second));
        std::sort(br.items.begin(), br.items.end(), [](const ItemResult& a, const ItemResult& b) {
            return a.delta_sec < b.delta_sec;
            });
        return br;
    }

} // namespace simcore::tas_movie
