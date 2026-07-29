// Script/TasMovieScript.h
#pragma once
#include <string>
#include <vector>
#include "../../../Runner/Script/PhaseScriptProgram.h"
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Runner/Breakpoints/BpRegistry.h"
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
        p.ops.push_back(OpRequireDiscGameIdFrom(savor::context::key::tas::DISC_ID6));

        // 2) Start movie playback from the DTM path provided by the job.
        p.ops.push_back(OpMoviePlayFrom(savor::context::key::tas::DTM_PATH));

        // 3) Run until any armed canonical BP triggers or cancellation,
        // movie termination, or infrastructure failure ends the attempt.
        p.ops.push_back(OpRunUntilBp());

        // 4) Always stop playback cleanly.
        p.ops.push_back(OpMovieStop());
        p.ops.push_back(OpGotoIf(savor::context::key::core::DW_RUN_OUTCOME_CODE, PSCmp::NE, 0, "DW_ERR"));

        // 5) Save a state named after the DTM path (worker decides whether to save-on-fail by consulting context).
        p.ops.push_back(OpSaveSavestateFrom(savor::context::key::tas::SAVE_PATH));
        p.ops.push_back(OpStepFrames(1, true));  // Prevents worker from not detecting a save-state load if the worker uses the above saved save state.
        p.ops.push_back(OpReturnResult(savor::context::key::tas::MOVIE_FAILED, 0));

        p.ops.push_back(OpLabel("DW_ERR"));
        p.ops.push_back(OpReturnResult(savor::context::key::tas::MOVIE_FAILED, 1));

        return p;
    }

} // namespace savor::tas_movie


