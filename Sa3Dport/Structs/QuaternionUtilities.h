#pragma once

#include "Structs/MatrixUtilities.h"
#include "Structs/MathHelper.h"
#include "Structs/Quaternion.h"
#include "Structs/Vector3.h"

#include <cmath>

namespace Sa3Dport::Structs::QuaternionUtilities {

[[nodiscard]] inline Vector3 quaternion_to_euler(Quaternion quaternion, bool rotate_zyx) {
    quaternion = normalize(quaternion);
    const float x = quaternion.x;
    const float y = quaternion.y;
    const float z = quaternion.z;
    const float w = quaternion.w;

    Vector3 result;
    if (rotate_zyx) {
        const float test = (w * x) - (y * z);

        if (test > 0.4995f) {
            result.z = 2.0f * std::atan2(z, x);
            result.x = MathHelper::HalfPi;
        } else if (test < -0.4995f) {
            result.z = -2.0f * std::atan2(z, x);
            result.x = -MathHelper::HalfPi;
        } else {
            result.x = std::asin(2.0f * test);
            result.y = std::atan2(2.0f * ((w * y) + (z * x)), 1.0f - (2.0f * ((y * y) + (x * x))));
            result.z = std::atan2(2.0f * ((w * z) + (y * x)), 1.0f - (2.0f * ((z * z) + (x * x))));
        }
    } else {
        const float test = (w * y) - (x * z);

        if (test > 0.4995f) {
            result.x = 2.0f * std::atan2(x, y);
            result.y = MathHelper::HalfPi;
        } else if (test < -0.4995f) {
            result.x = -2.0f * std::atan2(x, y);
            result.y = -MathHelper::HalfPi;
        } else {
            result.x = std::atan2(2.0f * ((w * x) + (z * y)), 1.0f - (2.0f * ((y * y) + (x * x))));
            result.y = std::asin(2.0f * test);
            result.z = std::atan2(2.0f * ((w * z) + (y * x)), 1.0f - (2.0f * ((z * z) + (y * y))));
        }
    }

    auto normalize_angle = [](float value) {
        value = std::fmod(value, MathHelper::Tau);
        if (value < -MathHelper::Pi) {
            value += MathHelper::Tau;
        } else if (value > MathHelper::Pi) {
            value -= MathHelper::Tau;
        }
        return value;
    };

    result.x = normalize_angle(result.x);
    result.y = normalize_angle(result.y);
    result.z = normalize_angle(result.z);
    return result;
}

[[nodiscard]] inline Vector3 quaternion_to_compatible_euler(Quaternion rotation, Vector3 previous, bool rotate_zyx) {
    rotation = normalize(rotation);
    return MatrixUtilities::to_compatible_euler(create_from_quaternion(rotation), previous, rotate_zyx);
}

[[nodiscard]] inline Quaternion euler_to_quaternion(Vector3 rotation, bool rotate_zyx) {
    return Quaternion::create_from_rotation_matrix(MatrixUtilities::create_rotation_matrix(rotation, rotate_zyx));
}

[[nodiscard]] inline Quaternion real_lerp(Quaternion from, Quaternion to, float t) {
    return {
        from.x + (to.x - from.x) * t,
        from.y + (to.y - from.y) * t,
        from.z + (to.z - from.z) * t,
        from.w + (to.w - from.w) * t};
}

} // namespace Sa3Dport::Structs::QuaternionUtilities
