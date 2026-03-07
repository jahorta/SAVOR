#pragma once
#include <array>
#include <string_view>
#include <vector>
#include <string>

namespace soasim::ui {

    inline const std::array<const char*, 2> kPredKindLabels = { "ABS", "DELTA" };
    inline const std::array<const char*, 6> kCmpOpLabels = { "==", "!=", "<", "<=", ">", ">=" };

    inline std::vector<std::string> VecPredKinds() {
        return { kPredKindLabels.begin(), kPredKindLabels.end() };
    }

    inline std::vector<std::string> VecCmpOps() {
        return { kCmpOpLabels.begin(), kCmpOpLabels.end() };
    }

} // namespace soasim::ui
