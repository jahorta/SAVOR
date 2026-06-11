#pragma once
#include "../../../Runner/Script/PhaseScriptVM.h"
#include "../../../Runner/Script/CtxRegistry.h"

namespace savor::tasframedetector {

inline PhaseScript MakeTasFrameDetectorProgram()
{
    PhaseScript p{};
    p.ops.push_back(OpRequireDiscGameIdFrom(savor::context::key::tasframedetector::DISC_ID6));
    p.ops.push_back(OpMoviePlayFrom(savor::context::key::tasframedetector::DTM_PATH));
    p.ops.push_back(OpSetU32(savor::context::key::tasframedetector::SAMPLE_COUNT, 0));

    p.ops.push_back(OpRecordTasInputSample());
    p.ops.push_back(OpGotoIf(savor::context::key::tasframedetector::MOVIE_ENDED, PSCmp::NE, 0, "DONE"));

    p.ops.push_back(OpLabel("LOOP"));
    p.ops.push_back(OpStepFrames(1, true));
    p.ops.push_back(OpRecordTasInputSample());
    p.ops.push_back(OpGotoIf(savor::context::key::tasframedetector::MOVIE_ENDED, PSCmp::EQ, 0, "LOOP"));

    p.ops.push_back(OpLabel("DONE"));
    p.ops.push_back(OpMovieStop());
    p.ops.push_back(OpReturnResult(savor::context::key::tasframedetector::MOVIE_FAILED, 0));
    return p;
}

} // namespace savor::tasframedetector
