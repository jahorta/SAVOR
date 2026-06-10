#pragma once
#include "InputPlan.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace savor {

    // Mapping from button bit -> human-friendly name (A, B, X, Y, L, R, Z, START, D-UP, etc.)
    using ButtonName = std::pair<uint16_t, std::string>;
    using ButtonNameMap = std::vector<ButtonName>;

    inline ButtonNameMap GenerateButtonNameMap() {
        return {
            { GC_A, "A" }, { GC_B, "B" }, { GC_X, "X" }, { GC_Y, "Y" },
            { GC_START, "START" }, { GC_Z, "Z" }, { GC_L_BTN, "L" }, { GC_R_BTN, "R" },
            { GC_DU, "DUP" }, { GC_DD, "DDOWN" }, { GC_DL, "DLEFT" }, { GC_DR, "DRIGHT" },
        };
    }

    // Pretty-prints only segments that differ from `neutral`.
    // Coalesces consecutive identical frames into [start..end] ranges.
    // Uses `btn_names` to decode the button mask into names (joined with '+').
    std::string DescribeChosenInputs(const InputPlan& plan, const std::string sep = "\n",
        const GCInputFrame& neutral = GCInputFrame(),
        const ButtonNameMap& btn_names = GenerateButtonNameMap());

    // Short one-liner summary of the first `max_segments` changes.
    std::string SummarizeChosenInputs(const InputPlan& plan,
        const GCInputFrame& neutral = GCInputFrame(),
        const ButtonNameMap& btn_names = GenerateButtonNameMap(),
        size_t max_segments = 5);

    std::string DescribeFrame(const GCInputFrame& f,
        const ButtonNameMap& names = GenerateButtonNameMap(),
        const GCInputFrame& neutral = GCInputFrame{});

    std::string DescribeFrameCompact(const GCInputFrame& f,
        const ButtonNameMap& names = GenerateButtonNameMap(),
        const GCInputFrame& neutral = GCInputFrame{});

} // namespace savor
