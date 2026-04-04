#pragma once

#include <chrono>

namespace simcore::db::types {

using UtcTimePoint = std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>;

inline UtcTimePoint UtcNow() {
    return std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
}

} // namespace simcore::db::types
