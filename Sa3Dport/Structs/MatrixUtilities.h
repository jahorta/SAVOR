#pragma once

#include "Structs/MathHelper.h"
#include "Structs/Matrix4x4.h"
#include "Structs/Quaternion.h"
#include "Structs/Vector3.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace Sa3Dport::Structs::MatrixUtilities {

[[nodiscard]] inline Matrix4x4 create_rotation_x(float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, c, s, 0.0f,
        0.0f, -s, c, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
}

[[nodiscard]] inline Matrix4x4 create_rotation_y(float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {
        c, 0.0f, -s, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        s, 0.0f, c, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
}

[[nodiscard]] inline Matrix4x4 create_rotation_z(float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {
        c, s, 0.0f, 0.0f,
        -s, c, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
}

[[nodiscard]] inline Matrix4x4 create_rotation_matrix(Vector3 rotation, bool zyx) {
    const auto matX = create_rotation_x(rotation.x);
    const auto matY = create_rotation_y(rotation.y);
    const auto matZ = create_rotation_z(rotation.z);
    return zyx ? (matZ * matX * matY) : (matX * matY * matZ);
}

[[nodiscard]] inline Matrix4x4 create_transform_matrix(Vector3 position, Quaternion rotation, Vector3 scale) {
    return create_scale(scale) * create_from_quaternion(rotation) * create_translation(position);
}

[[nodiscard]] inline Matrix4x4 create_transform_matrix(Vector3 position,
                                                       Vector3 rotation,
                                                       Vector3 scale,
                                                       bool rotate_zyx) {
    return create_scale(scale) * create_rotation_matrix(rotation, rotate_zyx) * create_translation(position);
}

[[nodiscard]] inline Matrix4x4 get_normal_matrix(const Matrix4x4& matrix) {
    Matrix4x4 inverse;
    if (!invert(matrix, inverse)) {
        throw std::runtime_error("matrix is not invertible");
    }
    return transpose(inverse);
}

[[nodiscard]] inline std::pair<Vector3, Vector3> matrix_to_normalized_euler2(const Matrix4x4& matrix,
                                                                             bool rotate_zyx) {
    Vector3 a;
    Vector3 b;

    if (rotate_zyx) {
        const float cx = std::hypot(matrix.m33, matrix.m31);

        if (cx > 16.0f * 1.192092896e-07f) {
            a = {
                std::atan2(-matrix.m32, cx),
                std::atan2(matrix.m31, matrix.m33),
                std::atan2(matrix.m12, matrix.m22)};

            b = {
                std::atan2(-matrix.m32, -cx),
                std::atan2(-matrix.m31, -matrix.m33),
                std::atan2(-matrix.m12, -matrix.m22)};
        } else {
            a = b = {
                std::atan2(-matrix.m32, cx),
                std::atan2(-matrix.m21, matrix.m11),
                0.0f};
        }
    } else {
        const float cy = std::hypot(matrix.m11, matrix.m12);

        if (cy > 16.0f * 1.192092896e-07f) {
            a = {
                std::atan2(matrix.m23, matrix.m33),
                std::atan2(-matrix.m13, cy),
                std::atan2(matrix.m12, matrix.m11)};

            b = {
                std::atan2(-matrix.m23, -matrix.m33),
                std::atan2(-matrix.m13, -cy),
                std::atan2(-matrix.m12, -matrix.m11)};
        } else {
            a = {
                std::atan2(-matrix.m32, matrix.m22),
                std::atan2(-matrix.m13, cy),
                0.0f};
            b = a;
        }
    }

    return {a, b};
}

[[nodiscard]] inline Vector3 compatible_euler(Vector3 rotation, Vector3 previous) {
    constexpr float pi_threshold = 5.1f;
    constexpr float pi2 = 2.0f * MathHelper::Pi;

    Vector3 diff = rotation - previous;
    for (int i = 0; i < 3; ++i) {
        if (diff[static_cast<std::size_t>(i)] > pi_threshold) {
            rotation[static_cast<std::size_t>(i)] -=
                std::floor((diff[static_cast<std::size_t>(i)] / pi2) + 0.5f) * pi2;
        } else if (diff[static_cast<std::size_t>(i)] < pi_threshold) {
            rotation[static_cast<std::size_t>(i)] +=
                std::floor((-diff[static_cast<std::size_t>(i)] / pi2) + 0.5f) * pi2;
        }
    }

    diff = rotation - previous;

    for (int i = 0; i < 3; ++i) {
        const auto current = static_cast<std::size_t>(i);
        const auto next = static_cast<std::size_t>((i + 1) % 3);
        const auto after_next = static_cast<std::size_t>((i + 2) % 3);
        if (std::fabs(diff[current]) > 3.2f &&
            std::fabs(diff[next]) < 1.6f &&
            std::fabs(diff[after_next]) < 1.6f) {
            rotation[current] += diff[current] > 0.0f ? -pi2 : pi2;
        }
    }

    return rotation;
}

[[nodiscard]] inline Vector3 to_euler(const Matrix4x4& matrix, bool rotate_zyx) {
    return matrix_to_normalized_euler2(matrix, rotate_zyx).first;
}

[[nodiscard]] inline Vector3 to_compatible_euler(const Matrix4x4& matrix, Vector3 previous, bool rotate_zyx) {
    auto [a, b] = matrix_to_normalized_euler2(matrix, rotate_zyx);
    a = compatible_euler(a, previous);
    b = compatible_euler(b, previous);

    const float d1 = std::fabs(a.x - previous.x) + std::fabs(a.y - previous.y) + std::fabs(a.z - previous.z);
    const float d2 = std::fabs(b.x - previous.x) + std::fabs(b.y - previous.y) + std::fabs(b.z - previous.z);
    return d1 > d2 ? b : a;
}

} // namespace Sa3Dport::Structs::MatrixUtilities
