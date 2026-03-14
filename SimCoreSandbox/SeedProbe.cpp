#define NOMINMAX

#include "SeedProbe.h"
#include <cmath>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <iostream>

#include "utils.h"

#include <Utils/Log.h>
#include <Utils/DeltaColorizer.h>
#include "Phases/RNGSeedDeltaMap.h"
#include "Core/Input/InputPlanFmt.h"

namespace sandbox {

    // ---------- progress bar helpers ----------
    void draw_progress_bar(const char* label, size_t done, size_t total)
    {
        if (total == 0) return;
        const int width = 40; // characters in the bar
        double frac = static_cast<double>(done) / static_cast<double>(total);
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        int filled = static_cast<int>(std::round(frac * width));

        std::printf("\r%-10s [", label);
        for (int i = 0; i < width; ++i)
            std::printf("%c", (i < filled) ? '#' : ' ');
        std::printf("] %3d%% (%zu/%zu)", static_cast<int>(std::round(frac * 100.0)), done, total);
        if (done >= total)
        {
            std::printf("\n");
        }
        std::fflush(stdout);
    }
    // -----------------------------------------
    
    static std::vector<uint8_t> levels_u8(uint8_t lo, uint8_t hi, int n, bool cap_top = false) {
        std::vector<uint8_t> v;
        if (n <= 0) return v;
        uint8_t top = cap_top ? static_cast<uint8_t>(hi - 1) : hi;
        if (n == 1) { v.push_back(static_cast<uint8_t>((int(lo) + int(top)) / 2)); return v; }
        v.reserve(n);
        double step = double(int(top) - int(lo)) / double(n - 1);
        for (int i = 0; i < n; ++i) {
            int val = int(std::round(double(lo) + step * i));
            if (val < lo) val = lo;
            if (val > top) val = top;
            v.push_back(static_cast<uint8_t>(val));
        }
        // Try to force exact 128 into the sequence for sticks when it should be near the middle.
        // (No-op for triggers where center isn't 128.)
        for (auto& x : v) {
            if (std::abs(int(x) - 128) <= 1) { x = 128; break; }
        }
        return v;
    }

    static long long signed_delta(uint32_t a, uint32_t b) {
        const long long sa = (long long)(int32_t)a;
        const long long sb = (long long)(int32_t)b;
        return sa - sb;
    }

    void print_family_grid(const simcore::RandSeedProbeResult& r, simcore::SeedFamily fam, int N, const char* title)
    {
        std::vector<const simcore::RandSeedProbeEntry*> entries;
        entries.reserve(N * N);
        for (const auto& e : r.entries)
            if (e.family == fam)
                entries.push_back(&e);

        if (entries.size() != static_cast<size_t>(N * N)) {
            std::printf("[SeedProbe] Grid for %s missing/size mismatch.\n", title);
            return;
        }

        color_lut_begin();
        for (auto& e : r.entries) color_lut_ingest(e.delta);
        color_lut_finalize();

        // X ticks from first row, Y ticks from first column
        std::vector<int> xvals(N), yvals(N);
        for (int col = 0; col < N; ++col) xvals[col] = static_cast<int>(entries[col]->x);
        for (int row = 0; row < N; ++row) yvals[row] = static_cast<int>(entries[row * N]->y);

        std::printf("[SeedProbe] %s delta grid (N=%d)\nlegend: ' *'(|delta|>FF), '-h'/' h' nibble, '00'..'FF' positive, ' 0' zero\n", title, N);

        // Top axis labels
        std::printf("    "); // left margin for Y label (3) + space
        for (int col = 0; col < N; ++col)
            std::printf("\x1b[2;37m%02X\x1b[0m ", xvals[col]);
        std::printf("\n");

        // Rows with left/right Y labels
        size_t idx = 0;
        for (int row = 0; row < N; ++row) {
            // Left Y label
            std::printf(" \x1b[2;37m%02X\x1b[0m ", yvals[row]);

            for (int col = 0; col < N; ++col) {
                const auto* p = entries[idx++];
                long long d = (p->ok ? p->delta : 0x7fff); // treat missing as big
                std::printf("%s%s%s ", color_for_delta(p->delta), fmt_delta_hex(p->delta).c_str(), color_reset());
            }

            std::printf("\n");
        }

        std::printf("\n\n");
        std::fflush(stdout);
    }

    void log_probe_summary(const simcore::RandSeedProbeResult& r) {
        SCLOGI("[SeedProbe] Summary: base=0x%08X, entries=%zu", r.base_seed, r.entries.size());
        for (const auto& e : r.entries) {
            if (!e.ok) { SCLOGI("  %-28s  hit=0  seed=--------  delta=------", e.label.c_str()); continue; }
            SCLOGI("  %s  hit=1  seed=0x%08X  delta=%lld", e.label.c_str(), e.seed, e.delta);
        }
    }

    std::vector<std::string> to_csv_lines(const simcore::RandSeedProbeResult& r) {
        std::vector<std::string> lines;
        lines.emplace_back("family,x,y,seed_hex,seed_dec,delta");
        auto fam_name = [](simcore::SeedFamily f) {
            switch (f) {
            case simcore::SeedFamily::Neutral:  return "Neutral";
            case simcore::SeedFamily::Main:     return "Main";
            case simcore::SeedFamily::CStick:   return "CStick";
            case simcore::SeedFamily::Triggers: return "Triggers";
            }
            return "Unknown";
            };
        char buf[256];
        for (const auto& e : r.entries) {
            if (!e.ok) continue;
            std::snprintf(buf, sizeof(buf), "%s,%02X,%02X,0x%08X,%u,%lld",
                fam_name(e.family), e.x, e.y, e.seed, e.seed, e.delta);
            lines.emplace_back(buf);
        }
        return lines;
    }

    void run_rng_seed_probe_menu(AppState& g)
    {
        // defaults
        simcore::RngSeedDeltaArgs a{};
        a.savestate_path = g.default_savestate;
        a.samples_per_axis = 8;
        a.min_value = 0x30;  //48
        a.max_value = 0xCF;  //207
        a.cap_trigger_top = true;

        bool find_combos = false;

        for (;;)
        {
            std::cout << "\n--- RNGSeedDeltaMap ---\n";
            std::cout << "ISO:              " << (g.iso_path.empty() ? "<unset>" : g.iso_path) << "\n";
            std::cout << "Dolphin base:     " << (g.dolphin_base_dir.empty() ? "<unset>" : g.dolphin_base_dir) << "\n";
            std::cout << "Workers:          " << g.workers << "\n";
            std::cout << "Savestate:        " << (a.savestate_path.empty() ? "<unset>" : a.savestate_path) << "\n";
            std::cout << "samples_per_axis: " << a.samples_per_axis << "\n";
            std::cout << "min_value:        0x" << std::hex << std::uppercase << a.min_value << std::dec << " (" << a.min_value << ")\n";
            std::cout << "max_value:        0x" << std::hex << std::uppercase << a.max_value << std::dec << " (" << a.max_value << ")\n";
            std::cout << "cap_trigger_top:  " << (a.cap_trigger_top ? "true" : "false") << "\n";
            std::cout << "run_timeout_ms:   " << a.run_timeout_ms << "\n";
            std::cout << "combos_attempts_per_target: " << a.combos_attempts_per_target << "\n";
            std::cout << "combos_sampler_tries: " << a.combos_sampler_tries << "\n";
            std::cout << "\n"
                << "1) Set savestate path\n"
                << "2) Set samples_per_axis\n"
                << "3) Set min_value\n"
                << "4) Set max_value\n"
                << "5) Toggle cap_trigger_top\n"
                << "6) Set run_timeout_ms\n"
                << "7) Set combos_attempts_per_target\n"
                << "8) Set combos_sampler_tries\n"
                << "r) Run\n"
                << "b) Back\n> ";
            std::string c; if (!std::getline(std::cin, c)) return;

            if (c == "1") a.savestate_path = prompt_path("Savestate path: ", true, true, a.savestate_path).string();
            else if (c == "2") { std::cout << "samples_per_axis: "; std::string s; std::getline(std::cin, s); if (!s.empty()) a.samples_per_axis = std::max(1, std::stoi(s)); }
            else if (c == "3") { std::cout << "min_value (0..255): "; std::string s; std::getline(std::cin, s); if (!s.empty()) a.min_value = std::clamp(std::stoi(s), 0, 255); }
            else if (c == "4") { std::cout << "max_value (0..255): "; std::string s; std::getline(std::cin, s); if (!s.empty()) a.max_value = std::clamp(std::stoi(s), 0, 255); }
            else if (c == "5") a.cap_trigger_top = !a.cap_trigger_top;
            else if (c == "6") { std::cout << "timeout (ms): "; std::string s; std::getline(std::cin, s); if (!s.empty()) a.run_timeout_ms = std::max(1, std::stoi(s)); }
            else if (c == "7") { std::cout << "combos_attempts_per_target: "; std::string s; std::getline(std::cin, s); if (!s.empty()) a.combos_attempts_per_target = std::max(1, std::stoi(s)); }
            else if (c == "8") { std::cout << "combos_sampler_tries: "; std::string s; std::getline(std::cin, s); if (!s.empty()) a.combos_sampler_tries = std::max(1, std::stoi(s)); }
            else if (c == "r" || c == "R")
            {
                if (g.iso_path.empty() || g.dolphin_base_dir.empty() || a.savestate_path.empty()) {
                    std::cout << "Please set ISO, Dolphin base, and savestate first.\n";
                    continue;
                }
                if (!ensure_sys_from_base_or_warn(g.dolphin_base_dir)) continue;

                // Runner
                simcore::ParallelPhaseScriptRunner runner(g.workers);

                // Boot plan: use your Boot module; keep ISO and portable base fixed for the lifetime of the pool.
                simcore::BootPlan boot = make_boot_plan(g);

                // Start runners
                if (!runner.start(boot)) {
                    SCLOGE("Failed to boot workers.");
                    exit(1); // or propagate error
                }

                // Seed-probe job args
                simcore::RngSeedDeltaArgs args = a;
                args.boot = boot;
                
                // Display results
                auto seed_delta_grid = RunRngSeedDeltaMap(runner, args);

                if (prompt_bool("Print delta grids?"))
                {
                    sandbox::print_family_grid(seed_delta_grid, simcore::SeedFamily::Main, a.samples_per_axis, "Main Stick");
                    sandbox::print_family_grid(seed_delta_grid, simcore::SeedFamily::CStick, a.samples_per_axis, "C Stick");
                    sandbox::print_family_grid(seed_delta_grid, simcore::SeedFamily::Triggers, a.samples_per_axis, "Triggers");
                }

                if (prompt_bool("Print log summary with all results?")) sandbox::log_probe_summary(seed_delta_grid);
                // Optionally dump CSV
                
                auto out_csv = prompt_path("CSV output path (blank to skip): ", /*require_exists*/false, /*allow_empty*/true, "").string();
                if (!out_csv.empty()) {
                    std::ofstream ofs(out_csv, std::ios::binary | std::ios::trunc);
                    for (const auto& line : sandbox::to_csv_lines(seed_delta_grid)) ofs << line << "\n";
                    SCLOGI("[SeedProbe] wrote CSV: %s", out_csv.c_str());
                }

                if (prompt_bool("Find complete set of unique deltas?")) {
                    runner.increment_epoch();
                    runner.reset_job_ids();

                    auto unique_seeds = RunFindSeedDeltaCombos(runner, args, seed_delta_grid);
                    for (auto s : unique_seeds.entries) {
                        SCLOGI("Found delta=%d using %s", s.delta, simcore::DescribeFrame(s.input).c_str());
                    }
                }

                runner.stop();
            }
            else if (c == "b" || c == "B") {
                return;
            }
        }
    }

} // namespace sandbox
