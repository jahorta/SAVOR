#pragma once

#include <bit>
#include <cstdint>

namespace Sa3Dport::Animation {

enum class KeyframeAttributes : std::uint16_t {
    None = 0,
    Position = 1u << 0,
    EulerRotation = 1u << 1,
    Scale = 1u << 2,
    Vector = 1u << 3,
    Vertex = 1u << 4,
    Normal = 1u << 5,
    Target = 1u << 6,
    Roll = 1u << 7,
    Angle = 1u << 8,
    LightColor = 1u << 9,
    Intensity = 1u << 10,
    Spot = 1u << 11,
    Point = 1u << 12,
    QuaternionRotation = 1u << 13,
};

enum class InterpolationMode {
    Linear = 0,
    Spline = 1,
    User = 2,
};

[[nodiscard]] inline constexpr KeyframeAttributes operator|(KeyframeAttributes left, KeyframeAttributes right) {
    return static_cast<KeyframeAttributes>(
        static_cast<std::uint16_t>(left) | static_cast<std::uint16_t>(right));
}

[[nodiscard]] inline constexpr KeyframeAttributes operator&(KeyframeAttributes left, KeyframeAttributes right) {
    return static_cast<KeyframeAttributes>(
        static_cast<std::uint16_t>(left) & static_cast<std::uint16_t>(right));
}

inline constexpr KeyframeAttributes& operator|=(KeyframeAttributes& left, KeyframeAttributes right) {
    left = left | right;
    return left;
}

[[nodiscard]] inline constexpr bool has_flag(KeyframeAttributes value, KeyframeAttributes flag) {
    return (static_cast<std::uint16_t>(value) & static_cast<std::uint16_t>(flag)) != 0;
}

[[nodiscard]] inline constexpr int channel_count(KeyframeAttributes attributes) {
    const auto masked = static_cast<std::uint16_t>(static_cast<std::uint16_t>(attributes) & 0x3FFFu);
    return std::popcount(masked);
}

inline constexpr KeyframeAttributes kKeyframeAttributeOrder[] {
    KeyframeAttributes::Position,
    KeyframeAttributes::EulerRotation,
    KeyframeAttributes::Scale,
    KeyframeAttributes::Vector,
    KeyframeAttributes::Vertex,
    KeyframeAttributes::Normal,
    KeyframeAttributes::Target,
    KeyframeAttributes::Roll,
    KeyframeAttributes::Angle,
    KeyframeAttributes::LightColor,
    KeyframeAttributes::Intensity,
    KeyframeAttributes::Spot,
    KeyframeAttributes::Point,
    KeyframeAttributes::QuaternionRotation,
};

} // namespace Sa3Dport::Animation
