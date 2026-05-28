#pragma once

#include <cstdint>

namespace Sa3Dport::ObjectData::Enums {

enum class NodeAttributes : std::uint32_t {
    None = 0,
    NoPosition = 1u << 0,
    NoRotation = 1u << 1,
    NoScale = 1u << 2,
    SkipDraw = 1u << 3,
    SkipChildren = 1u << 4,
    RotateZYX = 1u << 5,
    NoAnimate = 1u << 6,
    NoMorph = 1u << 7,
    Clip = 1u << 8,
    Modifier = 1u << 9,
    UseQuaternionRotation = 1u << 10,
    CacheRotation = 1u << 11,
    ApplyCachedRotation = 1u << 12,
    Envelope = 1u << 13,
};

[[nodiscard]] inline constexpr NodeAttributes operator|(NodeAttributes lhs, NodeAttributes rhs) {
    return static_cast<NodeAttributes>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

[[nodiscard]] inline constexpr NodeAttributes operator&(NodeAttributes lhs, NodeAttributes rhs) {
    return static_cast<NodeAttributes>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}

[[nodiscard]] inline constexpr NodeAttributes operator~(NodeAttributes value) {
    return static_cast<NodeAttributes>(~static_cast<std::uint32_t>(value));
}

inline constexpr NodeAttributes& operator|=(NodeAttributes& lhs, NodeAttributes rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline constexpr NodeAttributes& operator&=(NodeAttributes& lhs, NodeAttributes rhs) {
    lhs = lhs & rhs;
    return lhs;
}

[[nodiscard]] inline constexpr bool has_flag(NodeAttributes value, NodeAttributes flag) {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) == static_cast<std::uint32_t>(flag);
}

} // namespace Sa3Dport::ObjectData::Enums
