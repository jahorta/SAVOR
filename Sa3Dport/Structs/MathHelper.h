#pragma once

#include "Structs/MathCompat.h"

#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

namespace Sa3Dport::Structs::MathHelper {

inline constexpr float Pi = 3.14159265358979323846f;
inline constexpr float Tau = 2.0f * Pi;
inline constexpr float HalfPi = 0.5f * Pi;

[[nodiscard]] inline std::int32_t rad_to_bams(float radians) {
    return MathCompat::round_to_even_i32(radians * (65536.0f / Tau));
}

[[nodiscard]] inline float bams_to_rad(std::int32_t bams) {
    return static_cast<float>(bams) * (Tau / 65536.0f);
}

[[nodiscard]] inline std::string to_c_hex(std::uint16_t value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "0x" << std::uppercase << std::hex << std::setw(4) << std::setfill('0')
           << static_cast<unsigned int>(value);
    return stream.str();
}

[[nodiscard]] inline std::string to_c_hex(std::uint32_t value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return stream.str();
}

} // namespace Sa3Dport::Structs::MathHelper
