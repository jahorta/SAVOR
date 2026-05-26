#pragma once

#include "Structs/MathHelper.h"
#include "Structs/Vector2.h"
#include "Structs/Vector3.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace Sa3Dport::Structs::VectorUtilities {

[[nodiscard]] inline float greatest_value(Vector3 vector) {
    return std::max({vector.x, vector.y, vector.z});
}

[[nodiscard]] inline Vector3 calculate_average(std::span<const Vector3> points) {
    if (points.empty()) {
        return {};
    }

    Vector3 sum;
    for (const auto& point : points) {
        sum += point;
    }
    return sum / static_cast<float>(points.size());
}

[[nodiscard]] inline Vector3 calculate_center(std::span<const Vector3> points) {
    if (points.empty()) {
        return {};
    }

    Vector3 min = points.front();
    Vector3 max = points.front();
    for (const auto& point : points) {
        min.x = std::min(min.x, point.x);
        min.y = std::min(min.y, point.y);
        min.z = std::min(min.z, point.z);
        max.x = std::max(max.x, point.x);
        max.y = std::max(max.y, point.y);
        max.z = std::max(max.z, point.z);
    }
    return (min + max) * 0.5f;
}

[[nodiscard]] inline Vector3 normal_to_xz_angles(Vector3 normal) {
    const bool close0 = std::fabs(normal.x) < 0.002f && std::fabs(normal.y) < 0.002f;

    if (normal.z > 0.9999f || (close0 && normal.z > 0.0f)) {
        return {MathHelper::HalfPi, 0.0f, 0.0f};
    }
    if (normal.z < -0.9999f || (close0 && normal.z < 0.0f)) {
        return {-MathHelper::HalfPi, 0.0f, 0.0f};
    }

    return {
        std::asin(normal.z),
        0.0f,
        -std::atan2(normal.x, normal.y)};
}

[[nodiscard]] inline Vector3 xz_angles_to_normal(Vector3 rotation) {
    const float cos = std::cos(rotation.x);
    return {
        std::sin(-rotation.z) * cos,
        std::cos(-rotation.z) * cos,
        std::sin(rotation.x)};
}

[[nodiscard]] inline bool is_distance_approximate(Vector3 value, Vector3 other, float epsilon = 0.001f) {
    if (value == other) {
        return true;
    }
    return length_squared(value - other) < epsilon * epsilon;
}

[[nodiscard]] inline bool is_distance_approximate(Vector2 value, Vector2 other, float epsilon = 0.001f) {
    if (value == other) {
        return true;
    }
    return length_squared(value - other) < epsilon * epsilon;
}

} // namespace Sa3Dport::Structs::VectorUtilities
