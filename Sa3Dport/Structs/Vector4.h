#pragma once

#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace Sa3Dport::Structs {

struct Vector4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    constexpr Vector4() = default;
    constexpr Vector4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    [[nodiscard]] static constexpr Vector4 zero() { return {}; }

    float& operator[](std::size_t index) {
        if (index == 0) return x;
        if (index == 1) return y;
        if (index == 2) return z;
        if (index == 3) return w;
        throw std::out_of_range("Vector4 index out of range");
    }

    const float& operator[](std::size_t index) const {
        if (index == 0) return x;
        if (index == 1) return y;
        if (index == 2) return z;
        if (index == 3) return w;
        throw std::out_of_range("Vector4 index out of range");
    }

    [[nodiscard]] friend constexpr bool operator==(Vector4 lhs, Vector4 rhs) = default;
};

[[nodiscard]] inline constexpr Vector4 operator+(Vector4 lhs, Vector4 rhs) { return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z, lhs.w + rhs.w}; }
[[nodiscard]] inline constexpr Vector4 operator-(Vector4 lhs, Vector4 rhs) { return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z, lhs.w - rhs.w}; }
[[nodiscard]] inline constexpr Vector4 operator-(Vector4 value) { return {-value.x, -value.y, -value.z, -value.w}; }
[[nodiscard]] inline constexpr Vector4 operator*(Vector4 lhs, float rhs) { return {lhs.x * rhs, lhs.y * rhs, lhs.z * rhs, lhs.w * rhs}; }
[[nodiscard]] inline constexpr Vector4 operator*(float lhs, Vector4 rhs) { return rhs * lhs; }
[[nodiscard]] inline constexpr Vector4 operator/(Vector4 lhs, float rhs) { return {lhs.x / rhs, lhs.y / rhs, lhs.z / rhs, lhs.w / rhs}; }

[[nodiscard]] inline constexpr float dot(Vector4 lhs, Vector4 rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z + lhs.w * rhs.w;
}
[[nodiscard]] inline constexpr float length_squared(Vector4 value) { return dot(value, value); }
[[nodiscard]] inline float length(Vector4 value) { return std::sqrt(length_squared(value)); }
[[nodiscard]] inline float distance(Vector4 lhs, Vector4 rhs) { return length(lhs - rhs); }
[[nodiscard]] inline Vector4 normalize(Vector4 value) {
    const float len = length(value);
    return len == 0.0f ? Vector4{} : value / len;
}

} // namespace Sa3Dport::Structs
