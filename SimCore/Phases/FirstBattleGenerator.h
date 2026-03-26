#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <utility>

#include "../Runner/Parallel/WorkerBootPlan.h"
#include "../Runner/Script/PSContext.h"

namespace simcore { class ParallelPhaseScriptRunner; }

namespace simcore::tas_movie {

    struct ConductorArgs {
        BootPlan boot;
        std::string base_dtm;          // source DTM to clone per-RTC
        int rtc_delta_lo{ 0 };         // inclusive (seconds)
        int rtc_delta_hi{ 0 };         // inclusive (seconds)
        std::string out_dir;           // where stamped .dtm and .sav files are written
        uint32_t vi_stall_ms{ 2000 };
        bool save_on_fail{ true };
        std::string gameid = "GEAE8P";
    };

    struct ItemResult {
        int delta_sec{ 0 };
        std::string dtm_path;
        std::string save_path;
        uint64_t job_id{ 0 };
        bool ok{ false };
        uint32_t last_pc{ 0 };
        PSContext ctx;
    };

    struct BatchResult {
        std::vector<ItemResult> items;
        size_t submitted{ 0 };
        size_t succeeded{ 0 };
    };

    // Creates one DTM per second in [lo, hi], submits one job per DTM, and
    // waits for all results. Program PK_TasMovie must be active on the runner.
    BatchResult RunTasMovieOnePerWorkerWithProgress(ParallelPhaseScriptRunner& runner, const ConductorArgs& args);

} // namespace simcore::tas_movie
