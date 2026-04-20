#pragma once

#include <cmath>
#include <cstdint>

namespace Sa3Dport::Structs {

class BAMSFHelper {
public:
    static constexpr float kPi = 3.14159265358979323846f;
    static constexpr float kTwoPi = 2.0f * kPi;
    static constexpr float kBamsScale = 65536.0f;

    [[nodiscard]] static constexpr std::int32_t DegreesToBams(float degrees) {
        return static_cast<std::int32_t>(degrees * (kBamsScale / 360.0f));
    }

    [[nodiscard]] static constexpr float BamsToDegrees(std::int32_t bams) {
        return static_cast<float>(bams) * (360.0f / kBamsScale);
    }

    [[nodiscard]] static float RadiansToBams(float radians) {
        return radians * (kBamsScale / kTwoPi);
    }

    [[nodiscard]] static float BamsToRadians(std::int32_t bams) {
        return static_cast<float>(bams) * (kTwoPi / kBamsScale);
    }
};

} // namespace Sa3Dport::Structs
