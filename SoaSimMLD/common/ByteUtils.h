#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace soasim::mld::common {

[[nodiscard]] inline std::optional<std::uint32_t> readU32AtLE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    if (offset + 4 > bytes.size()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

[[nodiscard]] inline std::optional<std::uint32_t> readU32AtBE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    if (offset + 4 > bytes.size()) {
        return std::nullopt;
    }
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
        static_cast<std::uint32_t>(bytes[offset + 3]);
}

[[nodiscard]] inline std::optional<float> readF32AtLE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    const auto bits = readU32AtLE(bytes, offset);
    if (!bits.has_value()) {
        return std::nullopt;
    }

    float out = 0.0F;
    std::memcpy(&out, &(*bits), sizeof(out));
    return out;
}

[[nodiscard]] inline std::optional<float> readF32AtBE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    const auto bits = readU32AtBE(bytes, offset);
    if (!bits.has_value()) {
        return std::nullopt;
    }

    float out = 0.0F;
    std::memcpy(&out, &(*bits), sizeof(out));
    return out;
}

} // namespace soasim::mld::common
