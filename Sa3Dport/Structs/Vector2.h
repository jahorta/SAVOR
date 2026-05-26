#pragma once

#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace Sa3Dport::Structs {

struct Vector2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vector2() = default;
    constexpr Vector2(float x_, float y_) : x(x_), y(y_) {}

    [[nodiscard]] static constexpr Vector2 zero() { return {}; }
    [[nodiscard]] static constexpr Vector2 unit_x() { return {1.0f, 0.0f}; }
    [[nodiscard]] static constexpr Vector2 unit_y() { return {0.0f, 1.0f}; }

    float& operator[](std::size_t index) {
        if (index == 0) return x;
        if (index == 1) return y;
        throw std::out_of_range("Vector2 index out of range");
    }

    const float& operator[](std::size_t index) const {
        if (index == 0) return x;
        if (index == 1) return y;
        throw std::out_of_range("Vector2 index out of range");
    }

    [[nodiscard]] friend constexpr bool operator==(Vector2 lhs, Vector2 rhs) = default;
};

[[nodiscard]] inline constexpr Vector2 operator+(Vector2 lhs, Vector2 rhs) { return {lhs.x + rhs.x, lhs.y + rhs.y}; }
[[nodiscard]] inline constexpr Vector2 operator-(Vector2 lhs, Vector2 rhs) { return {lhs.x - rhs.x, lhs.y - rhs.y}; }
[[nodiscard]] inline constexpr Vector2 operator-(Vector2 value) { return {-value.x, -value.y}; }
[[nodiscard]] inline constexpr Vector2 operator*(Vector2 lhs, float rhs) { return {lhs.x * rhs, lhs.y * rhs}; }
[[nodiscard]] inline constexpr Vector2 operator*(float lhs, Vector2 rhs) { return rhs * lhs; }
[[nodiscard]] inline constexpr Vector2 operator/(Vector2 lhs, float rhs) { return {lhs.x / rhs, lhs.y / rhs}; }
inline constexpr Vector2& operator+=(Vector2& lhs, Vector2 rhs) { lhs = lhs + rhs; return lhs; }
inline constexpr Vector2& operator-=(Vector2& lhs, Vector2 rhs) { lhs = lhs - rhs; return lhs; }
inline constexpr Vector2& operator*=(Vector2& lhs, float rhs) { lhs = lhs * rhs; return lhs; }
inline constexpr Vector2& operator/=(Vector2& lhs, float rhs) { lhs = lhs / rhs; return lhs; }

[[nodiscard]] inline constexpr float dot(Vector2 lhs, Vector2 rhs) { return lhs.x * rhs.x + lhs.y * rhs.y; }
[[nodiscard]] inline constexpr float length_squared(Vector2 value) { return dot(value, value); }
[[nodiscard]] inline float length(Vector2 value) { return std::sqrt(length_squared(value)); }
[[nodiscard]] inline float distance(Vector2 lhs, Vector2 rhs) { return length(lhs - rhs); }
[[nodiscard]] inline Vector2 normalize(Vector2 value) {
    const float len = length(value);
    return len == 0.0f ? Vector2{} : value / len;
}

} // namespace Sa3Dport::Structs
