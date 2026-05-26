#pragma once

#include <cmath>

namespace Sa3Dport::Structs {

struct Matrix4x4;

struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    constexpr Quaternion() = default;
    constexpr Quaternion(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    [[nodiscard]] static constexpr Quaternion identity() { return {}; }
    [[nodiscard]] static Quaternion create_from_rotation_matrix(const Matrix4x4& matrix);

    [[nodiscard]] friend constexpr bool operator==(Quaternion lhs, Quaternion rhs) = default;
};

[[nodiscard]] inline constexpr float length_squared(Quaternion value) {
    return value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
}

[[nodiscard]] inline float length(Quaternion value) {
    return std::sqrt(length_squared(value));
}

[[nodiscard]] inline Quaternion normalize(Quaternion value) {
    const float len = length(value);
    if (len == 0.0f) {
        return Quaternion::identity();
    }
    return {value.x / len, value.y / len, value.z / len, value.w / len};
}

} // namespace Sa3Dport::Structs
