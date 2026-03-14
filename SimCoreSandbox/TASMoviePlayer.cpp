#include "TASMoviePlayer.h"
#define NOMINMAX
#include "utils.h"
#include <cstdio>
#include <iostream>

#include <Runner/IPC/Wire.h>
#include <Runner/Script/KeyRegistry.h>
#include <Runner/Script/PhaseScriptVM.h>
#include <Phases/FirstBattleGenerator.h>

namespace sandbox {

    void menu_tas_movie(AppState g)
    {
        using namespace simcore::tas_movie;

        ConductorArgs a{};

        a.vi_stall_ms = 60000;
        a.rtc_delta_lo = 0;         // UNIX seconds
        a.rtc_delta_hi = 0;         // UNIX seconds
        a.base_dtm = { "D:\\SoATAS\\beginning_in_first_battle.dtm" };
        a.out_dir = { "D:\\SoATAS\\beginning_output" };

        for (;;) {
            std::cout << "\n--- TAS Movie -> BP -> Savestate ---\n";
            std::cout << "ISO:                 " << (g.iso_path.empty() ? "<unset>" : g.iso_path) << "\n";
            std::cout << "Dolphin base:        " << (g.dolphin_base_dir.empty() ? "<unset>" : g.dolphin_base_dir) << "\n";
            std::cout << "Workers:             " << g.workers << "\n";
            std::cout << "DTM path:            " << (a.base_dtm.empty() ? "<unset>" : a.base_dtm) << "\n";
            std::cout << "Output dir:          " << (a.out_dir.empty() ? "<unset>" : a.out_dir) << "\n";
            std::cout << "RTC range (start..end): " << a.rtc_delta_lo << " .. " << a.rtc_delta_hi << "\n";
            std::cout << "VI Timeout (ms):        " << a.vi_stall_ms << "\n";
            std::cout << "Require disc ID6:    " << a.gameid << "\n";
            std::cout << "1) Set DTM path\n"
                "2) Set output directory\n"
                "3) Set RTC start\n"
                "4) Set RTC end\n"
                "5) Set timeout (ms)\n"
                "6) Set require ID6 (6 chars)\n"
                "r) Run\n"
                "b) Back\n> ";
            std::string c; if (!std::getline(std::cin, c)) return;

            if (c == "1") a.base_dtm = prompt_path("DTM path: ", true, false, a.base_dtm).string();
            else if (c == "2") a.out_dir = prompt_path("Output directory: ", false, false, a.out_dir).string();
            else if (c == "3") { std::cout << "RTC start (UNIX seconds): "; std::string s; std::getline(std::cin, s); if (!s.empty()) a.rtc_delta_lo = std::stoul(s); }
            else if (c == "4") { std::cout << "RTC end   (UNIX seconds): "; std::string s; std::getline(std::cin, s); if (!s.empty()) a.rtc_delta_hi = std::stoul(s); }
            else if (c == "5") { std::cout << "VI Timeout (ms): "; std::string s; std::getline(std::cin, s); if (!s.empty()) a.vi_stall_ms = std::max(1u, (uint32_t)std::stoul(s)); }
            else if (c == "6") { std::cout << "ID6 (exactly 6 chars): "; std::string s; std::getline(std::cin, s); a.gameid = s.size() > 6 ? s.substr(0, 6) : s; }
            else if (c == "r" || c == "R")
            {
                if (g.iso_path.empty() || g.dolphin_base_dir.empty() || a.base_dtm.empty() || a.out_dir.empty()) {
                    std::cout << "Please set ISO, Dolphin base, DTM, and output dir first.\n";
                    continue;
                }
                if (a.rtc_delta_hi < a.rtc_delta_lo) { std::cout << "RTC end must be >= start.\n"; continue; }
                if (!ensure_sys_from_base_or_warn(g.dolphin_base_dir)) continue;

                simcore::PSInit init{};
                init.savestate_path.clear();
                init.default_timeout_ms = 0;
                simcore::BootPlan boot = make_boot_plan(g);

                simcore::ParallelPhaseScriptRunner runner(g.workers);
                if (!runner.start(boot)) { std::cout << "Failed to start runner.\n"; continue; }

                auto result = RunTasMovieOnePerWorkerWithProgress(runner, a);

                for (auto i : result.items) {
                    if (i.ok) SCLOGI("[rtc %d] Successful save to: %s", i.delta_sec, i.save_path.c_str());
                    else {
                        uint32_t o_temp; i.ctx.get(simcore::keys::core::DW_RUN_OUTCOME_CODE, o_temp); simcore::RunToBpOutcome outcome = static_cast<simcore::RunToBpOutcome>(o_temp);
                        if (outcome != simcore::RunToBpOutcome::Hit)
                            switch (outcome) {
                            case simcore::RunToBpOutcome::Aborted:
                                SCLOGW("[rtc %d] Run Aborted!!!", i.delta_sec);
                                break;
                            case simcore::RunToBpOutcome::MovieEnded:
                                SCLOGW("[rtc %d] Movie ended!!!", i.delta_sec);
                                break;
                            case simcore::RunToBpOutcome::Timeout:
                                SCLOGW("[rtc %d] Timed out!!!", i.delta_sec);
                                break;
                            case simcore::RunToBpOutcome::ViStalled:
                                SCLOGW("[rtc %d] VI Stalled!!!", i.delta_sec);
                                break;
                            case simcore::RunToBpOutcome::Unknown:
                                SCLOGW("[rtc %d] Stopped for unknown reason??", i.delta_sec);
                                break;
                            default:
                                break;
                            }
                        else SCLOGW("[rtc %d] Failed to save. dtm at: %s", i.delta_sec, i.dtm_path.c_str());
                    }
                }

                runner.stop();
            }
            else if (c == "b" || c == "B") return;
        }
    }

}