#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

namespace Sa3Dport::Structs::MathCompat {

[[nodiscard]] inline constexpr float clamp01(float value) {
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

[[nodiscard]] inline float round_to_even(float value) {
    if (!std::isfinite(value)) {
        return value;
    }

    const float sign = value < 0.0f ? -1.0f : 1.0f;
    const float absValue = std::fabs(value);
    const float floorValue = std::floor(absValue);
    const float fraction = absValue - floorValue;

    float rounded = floorValue;
    if (fraction > 0.5f) {
        rounded = floorValue + 1.0f;
    } else if (fraction == 0.5f) {
        const auto floorInt = static_cast<std::int64_t>(floorValue);
        rounded = (floorInt % 2 == 0) ? floorValue : floorValue + 1.0f;
    }

    return sign * rounded;
}

[[nodiscard]] inline std::int16_t round_to_even_i16(float value) {
    return static_cast<std::int16_t>(round_to_even(value));
}

[[nodiscard]] inline std::int32_t round_to_even_i32(float value) {
    return static_cast<std::int32_t>(round_to_even(value));
}

[[nodiscard]] inline std::string fixed5_float(float value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(5) << value << 'f';
    return stream.str();
}

} // namespace Sa3Dport::Structs::MathCompat
