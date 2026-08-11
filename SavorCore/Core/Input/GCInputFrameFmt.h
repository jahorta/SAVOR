#pragma once

#include "GCInputFrame.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace savor {

using ButtonName = std::pair<std::uint16_t, std::string>;
using ButtonNameMap = std::vector<ButtonName>;

inline ButtonNameMap GenerateButtonNameMap()
{
    return {
        {GC_A, "A"}, {GC_B, "B"}, {GC_X, "X"}, {GC_Y, "Y"},
        {GC_START, "START"}, {GC_Z, "Z"}, {GC_L_BTN, "L"}, {GC_R_BTN, "R"},
        {GC_DU, "DUP"}, {GC_DD, "DDOWN"}, {GC_DL, "DLEFT"}, {GC_DR, "DRIGHT"},
    };
}

[[nodiscard]] std::string DescribeFrame(
    const GCInputFrame& frame,
    const ButtonNameMap& names = GenerateButtonNameMap(),
    const GCInputFrame& neutral = GCInputFrame{});

[[nodiscard]] std::string DescribeFrameCompact(
    const GCInputFrame& frame,
    const ButtonNameMap& names = GenerateButtonNameMap(),
    const GCInputFrame& neutral = GCInputFrame{});

} // namespace savor
