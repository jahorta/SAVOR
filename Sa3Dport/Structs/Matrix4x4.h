#pragma once

#include "Structs/Quaternion.h"
#include "Structs/Vector3.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Sa3Dport::Structs {

struct Matrix4x4 {
    float m11 = 0.0f, m12 = 0.0f, m13 = 0.0f, m14 = 0.0f;
    float m21 = 0.0f, m22 = 0.0f, m23 = 0.0f, m24 = 0.0f;
    float m31 = 0.0f, m32 = 0.0f, m33 = 0.0f, m34 = 0.0f;
    float m41 = 0.0f, m42 = 0.0f, m43 = 0.0f, m44 = 0.0f;

    [[nodiscard]] friend constexpr bool operator==(Matrix4x4 lhs, Matrix4x4 rhs) = default;
};

[[nodiscard]] inline constexpr Matrix4x4 identity() {
    return {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
}

[[nodiscard]] inline constexpr Matrix4x4 create_scale(float scale) {
    return {
        scale, 0.0f, 0.0f, 0.0f,
        0.0f, scale, 0.0f, 0.0f,
        0.0f, 0.0f, scale, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
}

[[nodiscard]] inline constexpr Matrix4x4 create_scale(Vector3 scale) {
    return {
        scale.x, 0.0f, 0.0f, 0.0f,
        0.0f, scale.y, 0.0f, 0.0f,
        0.0f, 0.0f, scale.z, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
}

[[nodiscard]] inline constexpr Matrix4x4 create_translation(Vector3 position) {
    return {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        position.x, position.y, position.z, 1.0f};
}

[[nodiscard]] inline Matrix4x4 create_from_quaternion(Quaternion q) {
    q = normalize(q);

    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float wz = q.z * q.w;
    const float xz = q.z * q.x;
    const float wy = q.y * q.w;
    const float yz = q.y * q.z;
    const float wx = q.x * q.w;

    return {
        1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy), 0.0f,
        2.0f * (xy - wz), 1.0f - 2.0f * (zz + xx), 2.0f * (yz + wx), 0.0f,
        2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (yy + xx), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
}

[[nodiscard]] inline constexpr Matrix4x4 transpose(const Matrix4x4& m) {
    return {
        m.m11, m.m21, m.m31, m.m41,
        m.m12, m.m22, m.m32, m.m42,
        m.m13, m.m23, m.m33, m.m43,
        m.m14, m.m24, m.m34, m.m44};
}

[[nodiscard]] inline constexpr Matrix4x4 operator*(const Matrix4x4& lhs, const Matrix4x4& rhs) {
    return {
        lhs.m11 * rhs.m11 + lhs.m12 * rhs.m21 + lhs.m13 * rhs.m31 + lhs.m14 * rhs.m41,
        lhs.m11 * rhs.m12 + lhs.m12 * rhs.m22 + lhs.m13 * rhs.m32 + lhs.m14 * rhs.m42,
        lhs.m11 * rhs.m13 + lhs.m12 * rhs.m23 + lhs.m13 * rhs.m33 + lhs.m14 * rhs.m43,
        lhs.m11 * rhs.m14 + lhs.m12 * rhs.m24 + lhs.m13 * rhs.m34 + lhs.m14 * rhs.m44,
        lhs.m21 * rhs.m11 + lhs.m22 * rhs.m21 + lhs.m23 * rhs.m31 + lhs.m24 * rhs.m41,
        lhs.m21 * rhs.m12 + lhs.m22 * rhs.m22 + lhs.m23 * rhs.m32 + lhs.m24 * rhs.m42,
        lhs.m21 * rhs.m13 + lhs.m22 * rhs.m23 + lhs.m23 * rhs.m33 + lhs.m24 * rhs.m43,
        lhs.m21 * rhs.m14 + lhs.m22 * rhs.m24 + lhs.m23 * rhs.m34 + lhs.m24 * rhs.m44,
        lhs.m31 * rhs.m11 + lhs.m32 * rhs.m21 + lhs.m33 * rhs.m31 + lhs.m34 * rhs.m41,
        lhs.m31 * rhs.m12 + lhs.m32 * rhs.m22 + lhs.m33 * rhs.m32 + lhs.m34 * rhs.m42,
        lhs.m31 * rhs.m13 + lhs.m32 * rhs.m23 + lhs.m33 * rhs.m33 + lhs.m34 * rhs.m43,
        lhs.m31 * rhs.m14 + lhs.m32 * rhs.m24 + lhs.m33 * rhs.m34 + lhs.m34 * rhs.m44,
        lhs.m41 * rhs.m11 + lhs.m42 * rhs.m21 + lhs.m43 * rhs.m31 + lhs.m44 * rhs.m41,
        lhs.m41 * rhs.m12 + lhs.m42 * rhs.m22 + lhs.m43 * rhs.m32 + lhs.m44 * rhs.m42,
        lhs.m41 * rhs.m13 + lhs.m42 * rhs.m23 + lhs.m43 * rhs.m33 + lhs.m44 * rhs.m43,
        lhs.m41 * rhs.m14 + lhs.m42 * rhs.m24 + lhs.m43 * rhs.m34 + lhs.m44 * rhs.m44};
}

[[nodiscard]] inline bool invert(const Matrix4x4& input, Matrix4x4& output) {
    std::array<std::array<float, 8>, 4> a {{
        {input.m11, input.m12, input.m13, input.m14, 1.0f, 0.0f, 0.0f, 0.0f},
        {input.m21, input.m22, input.m23, input.m24, 0.0f, 1.0f, 0.0f, 0.0f},
        {input.m31, input.m32, input.m33, input.m34, 0.0f, 0.0f, 1.0f, 0.0f},
        {input.m41, input.m42, input.m43, input.m44, 0.0f, 0.0f, 0.0f, 1.0f},
    }};

    for (std::size_t col = 0; col < 4; ++col) {
        std::size_t pivot = col;
        float best = std::fabs(a[col][col]);
        for (std::size_t row = col + 1; row < 4; ++row) {
            const float candidate = std::fabs(a[row][col]);
            if (candidate > best) {
                best = candidate;
                pivot = row;
            }
        }

        if (best == 0.0f) {
            return false;
        }

        if (pivot != col) {
            std::swap(a[pivot], a[col]);
        }

        const float divisor = a[col][col];
        for (float& value : a[col]) {
            value /= divisor;
        }

        for (std::size_t row = 0; row < 4; ++row) {
            if (row == col) {
                continue;
            }
            const float factor = a[row][col];
            for (std::size_t i = 0; i < 8; ++i) {
                a[row][i] -= factor * a[col][i];
            }
        }
    }

    output = {
        a[0][4], a[0][5], a[0][6], a[0][7],
        a[1][4], a[1][5], a[1][6], a[1][7],
        a[2][4], a[2][5], a[2][6], a[2][7],
        a[3][4], a[3][5], a[3][6], a[3][7]};
    return true;
}

inline Quaternion Quaternion::create_from_rotation_matrix(const Matrix4x4& matrix) {
    const float trace = matrix.m11 + matrix.m22 + matrix.m33;
    Quaternion result;

    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        result.w = 0.25f * s;
        result.x = (matrix.m23 - matrix.m32) / s;
        result.y = (matrix.m31 - matrix.m13) / s;
        result.z = (matrix.m12 - matrix.m21) / s;
    } else if (matrix.m11 > matrix.m22 && matrix.m11 > matrix.m33) {
        const float s = std::sqrt(1.0f + matrix.m11 - matrix.m22 - matrix.m33) * 2.0f;
        result.w = (matrix.m23 - matrix.m32) / s;
        result.x = 0.25f * s;
        result.y = (matrix.m12 + matrix.m21) / s;
        result.z = (matrix.m31 + matrix.m13) / s;
    } else if (matrix.m22 > matrix.m33) {
        const float s = std::sqrt(1.0f + matrix.m22 - matrix.m11 - matrix.m33) * 2.0f;
        result.w = (matrix.m31 - matrix.m13) / s;
        result.x = (matrix.m12 + matrix.m21) / s;
        result.y = 0.25f * s;
        result.z = (matrix.m23 + matrix.m32) / s;
    } else {
        const float s = std::sqrt(1.0f + matrix.m33 - matrix.m11 - matrix.m22) * 2.0f;
        result.w = (matrix.m12 - matrix.m21) / s;
        result.x = (matrix.m31 + matrix.m13) / s;
        result.y = (matrix.m23 + matrix.m32) / s;
        result.z = 0.25f * s;
    }

    return normalize(result);
}

} // namespace Sa3Dport::Structs
