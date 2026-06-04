#pragma once

#include "Structs/EndianStackReader.h"

#include <cstdint>
#include <optional>

namespace Sa3Dport::Structs::PointerIO {

[[nodiscard]] inline bool is_null_pointer(std::uint32_t raw) {
    return raw == 0 || raw == UINT32_MAX;
}

[[nodiscard]] inline std::optional<std::uint32_t> read_nullable_pointer_subtract_base(
    const EndianStackReader& reader,
    std::uint32_t address,
    std::uint32_t imageBase) {
    const std::uint32_t raw = reader.read_u32(address);
    if (is_null_pointer(raw)) {
        return std::nullopt;
    }

    return raw - imageBase;
}

[[nodiscard]] inline std::optional<std::uint32_t> read_nullable_pointer_add_base(
    const EndianStackReader& reader,
    std::uint32_t address,
    std::uint32_t imageBase) {
    const std::uint32_t raw = reader.read_u32(address);
    if (is_null_pointer(raw)) {
        return std::nullopt;
    }

    return raw + imageBase;
}

[[nodiscard]] inline std::uint32_t read_pointer_add_base(
    const EndianStackReader& reader,
    std::uint32_t address,
    std::uint32_t imageBase) {
    const std::uint32_t raw = reader.read_u32(address);
    if (raw == UINT32_MAX) {
        return UINT32_MAX;
    }

    return raw + imageBase;
}

} // namespace Sa3Dport::Structs::PointerIO
