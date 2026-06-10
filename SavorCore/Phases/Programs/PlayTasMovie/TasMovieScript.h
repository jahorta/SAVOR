// Script/TasMovieScript.h
#pragma once
#include <string>
#include <vector>
#include "../../../Runner/Script/PhaseScriptVM.h"
#include "../../../Runner/Script/KeyRegistry.h"
#include "../../../Runner/Breakpoints/BPRegistry.h"
#include "TasMoviePayload.h"

namespace savor::tasmovie {

    // Main TAS program: all arguments come from job/context via *_FROM(key).
    inline PhaseScript MakeTasMovieProgram()
    {
        PhaseScript p{};
        p.canonical_bp_keys = {
            bp::prebattle::BeforeRandSeedSet,
        };

        // 1) Make sure the disc matches the movie and reset game state.
        p.ops.push_back(OpRequireDiscGameIdFrom(keys::tas::DISC_ID6));

        // 2) Start movie playback from the DTM path provided by the job.
        p.ops.push_back(OpMoviePlayFrom(keys::tas::DTM_PATH));

        // 3) Set a per-job timeout derived from the DTM header.
        p.ops.push_back(OpSetTimeoutFromKey(keys::core::RUN_MS));

        // 4) Run until any armed canonical BP triggers (or failure/timeouts/watchdogs).
        p.ops.push_back(OpRunUntilBp());

        // 5) Always stop playback cleanly.
        p.ops.push_back(OpMovieStop());
        p.ops.push_back(OpGotoIf(keys::core::DW_RUN_OUTCOME_CODE, PSCmp::NE, 0, "DW_ERR"));

        // 6) Save a state named after the DTM path (worker decides whether to save-on-fail by consulting context).
        p.ops.push_back(OpSaveSavestateFrom(keys::tas::SAVE_PATH));
        p.ops.push_back(OpStepFrames(1, true));  // Prevents worker from not detecting a save-state load if the worker uses the above saved save state.
        p.ops.push_back(OpReturnResult(keys::tas::MOVIE_FAILED, 0));

        p.ops.push_back(OpLabel("DW_ERR"));
        p.ops.push_back(OpReturnResult(keys::tas::MOVIE_FAILED, 1));

        return p;
    }

} // namespace savor::tas_movie


