#pragma once

#include <bit>
#include <cstdint>

namespace Sa3Dport::Structs {

enum class Endian {
    Little,
    Big,
};

[[nodiscard]] inline constexpr std::uint16_t byteswap(std::uint16_t value) {
    return static_cast<std::uint16_t>((value >> 8) | (value << 8));
}

[[nodiscard]] inline constexpr std::int16_t byteswap(std::int16_t value) {
    return static_cast<std::int16_t>(byteswap(static_cast<std::uint16_t>(value)));
}

[[nodiscard]] inline constexpr std::uint32_t byteswap(std::uint32_t value) {
    return ((value & 0x000000FFu) << 24) |
           ((value & 0x0000FF00u) << 8) |
           ((value & 0x00FF0000u) >> 8) |
           ((value & 0xFF000000u) >> 24);
}

[[nodiscard]] inline constexpr std::int32_t byteswap(std::int32_t value) {
    return static_cast<std::int32_t>(byteswap(static_cast<std::uint32_t>(value)));
}

[[nodiscard]] inline float byteswap_float(float value) {
    return std::bit_cast<float>(byteswap(std::bit_cast<std::uint32_t>(value)));
}

[[nodiscard]] inline constexpr bool needs_swap(Endian endian) {
    return (endian == Endian::Little && std::endian::native == std::endian::big) ||
           (endian == Endian::Big && std::endian::native == std::endian::little);
}

} // namespace Sa3Dport::Structs
