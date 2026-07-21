#pragma once

#include "../Model/NavigationAreaModel.h"

#include <cmath>

namespace savor::navigation {

// Centralized coordinate conversion boundary for navigation data. The first
// pass intentionally preserves SPICE/MLD coordinates, but graph construction,
// rendering, and future SCT interpretation can all depend on this policy
// instead of embedding an axis or handedness assumption.
class NavigationCoordinatePolicy final {
public:
    [[nodiscard]] static constexpr NavigationCoordinatePolicy identity() noexcept {
        return NavigationCoordinatePolicy{};
    }

    [[nodiscard]] constexpr NavigationVec3 convertPosition(const NavigationVec3& value) const noexcept {
        return value;
    }

    [[nodiscard]] constexpr NavigationVec3 convertDirection(const NavigationVec3& value) const noexcept {
        return value;
    }

    [[nodiscard]] constexpr float convertYaw(const float value) const noexcept {
        return value;
    }

    // Accepts a yaw that has already passed through convertYaw(). This keeps
    // rendering code from converting opcode-77 yaw twice. Scene yaw rotates
    // around +Y, with zero facing +Z and positive angles turning toward +X.
    [[nodiscard]] NavigationVec3 sceneFacingDirectionFromConvertedYaw(
        const float convertedSceneYawDegrees) const noexcept {
        if (!std::isfinite(convertedSceneYawDegrees)) {
            return NavigationVec3{};
        }

        constexpr float degreesToRadians = 0.017453292519943295769F;
        const float radians = convertedSceneYawDegrees * degreesToRadians;
        return NavigationVec3{
            std::sin(radians),
            0.0F,
            std::cos(radians),
        };
    }

    [[nodiscard]] constexpr NavigationVec3 upAxis() const noexcept {
        return NavigationVec3{ 0.0F, 1.0F, 0.0F };
    }

private:
    constexpr NavigationCoordinatePolicy() noexcept = default;
};

} // namespace savor::navigation
