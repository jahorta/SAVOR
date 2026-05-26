#pragma once

#include "Structs/Vector3.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace Sa3Dport::Structs {

struct PositionNormal {
    Vector3 position {};
    Vector3 normal {};

    [[nodiscard]] bool operator==(const PositionNormal&) const = default;
};

struct PositionNormalHash {
    [[nodiscard]] std::size_t operator()(const PositionNormal& value) const noexcept {
        std::size_t seed = 0;
        combine(seed, value.position.x);
        combine(seed, value.position.y);
        combine(seed, value.position.z);
        combine(seed, value.normal.x);
        combine(seed, value.normal.y);
        combine(seed, value.normal.z);
        return seed;
    }

private:
    static void combine(std::size_t& seed, float value) noexcept {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        seed ^= std::hash<std::uint32_t>{}(bits) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
    }
};

} // namespace Sa3Dport::Structs
