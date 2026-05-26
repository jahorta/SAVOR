#pragma once

#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace Sa3Dport::Structs {

struct Vector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vector3() = default;
    constexpr Vector3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    [[nodiscard]] static constexpr Vector3 zero() { return {}; }
    [[nodiscard]] static constexpr Vector3 one() { return {1.0f, 1.0f, 1.0f}; }
    [[nodiscard]] static constexpr Vector3 unit_x() { return {1.0f, 0.0f, 0.0f}; }
    [[nodiscard]] static constexpr Vector3 unit_y() { return {0.0f, 1.0f, 0.0f}; }
    [[nodiscard]] static constexpr Vector3 unit_z() { return {0.0f, 0.0f, 1.0f}; }

    float& operator[](std::size_t index) {
        if (index == 0) return x;
        if (index == 1) return y;
        if (index == 2) return z;
        throw std::out_of_range("Vector3 index out of range");
    }

    const float& operator[](std::size_t index) const {
        if (index == 0) return x;
        if (index == 1) return y;
        if (index == 2) return z;
        throw std::out_of_range("Vector3 index out of range");
    }

    [[nodiscard]] friend constexpr bool operator==(Vector3 lhs, Vector3 rhs) = default;
};

[[nodiscard]] inline constexpr Vector3 operator+(Vector3 lhs, Vector3 rhs) { return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z}; }
[[nodiscard]] inline constexpr Vector3 operator-(Vector3 lhs, Vector3 rhs) { return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z}; }
[[nodiscard]] inline constexpr Vector3 operator-(Vector3 value) { return {-value.x, -value.y, -value.z}; }
[[nodiscard]] inline constexpr Vector3 operator*(Vector3 lhs, float rhs) { return {lhs.x * rhs, lhs.y * rhs, lhs.z * rhs}; }
[[nodiscard]] inline constexpr Vector3 operator*(float lhs, Vector3 rhs) { return rhs * lhs; }
[[nodiscard]] inline constexpr Vector3 operator/(Vector3 lhs, float rhs) { return {lhs.x / rhs, lhs.y / rhs, lhs.z / rhs}; }
inline constexpr Vector3& operator+=(Vector3& lhs, Vector3 rhs) { lhs = lhs + rhs; return lhs; }
inline constexpr Vector3& operator-=(Vector3& lhs, Vector3 rhs) { lhs = lhs - rhs; return lhs; }
inline constexpr Vector3& operator*=(Vector3& lhs, float rhs) { lhs = lhs * rhs; return lhs; }
inline constexpr Vector3& operator/=(Vector3& lhs, float rhs) { lhs = lhs / rhs; return lhs; }

[[nodiscard]] inline constexpr float dot(Vector3 lhs, Vector3 rhs) { return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z; }
[[nodiscard]] inline constexpr float length_squared(Vector3 value) { return dot(value, value); }
[[nodiscard]] inline float length(Vector3 value) { return std::sqrt(length_squared(value)); }
[[nodiscard]] inline float distance(Vector3 lhs, Vector3 rhs) { return length(lhs - rhs); }
[[nodiscard]] inline Vector3 normalize(Vector3 value) {
    const float len = length(value);
    return len == 0.0f ? Vector3{} : value / len;
}

} // namespace Sa3Dport::Structs
