#pragma once

#include <chrono>

namespace savor::db::types {

using UtcTimePoint = std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>;

inline UtcTimePoint UtcNow() {
    return std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
}

} // namespace savor::db::types
