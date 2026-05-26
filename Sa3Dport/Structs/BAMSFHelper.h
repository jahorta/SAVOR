#pragma once

#include "Structs/MathCompat.h"

#include <cmath>
#include <cstdint>

namespace Sa3Dport::Structs {

class BAMSFHelper {
public:
    static constexpr float kPi = 3.14159265358979323846f;
    static constexpr float kTwoPi = 2.0f * kPi;
    static constexpr float kBamsfScale = 65535.0f;
    static constexpr float BAMSF2Deg = kBamsfScale / 360.0f;
    static constexpr float BAMSF2Rad = kBamsfScale / kTwoPi;

    [[nodiscard]] static std::int32_t DegToBAMSF(float degrees) {
        return MathCompat::round_to_even_i32(degrees * BAMSF2Deg);
    }

    [[nodiscard]] static float BAMSFToDeg(std::int32_t bams) {
        return static_cast<float>(bams) / BAMSF2Deg;
    }

    [[nodiscard]] static std::int32_t RadToBAMSF(float radians) {
        return MathCompat::round_to_even_i32(radians * BAMSF2Rad);
    }

    [[nodiscard]] static float BAMSFToRad(std::int32_t value) {
        return static_cast<float>(value) / BAMSF2Rad;
    }

    [[nodiscard]] static std::int32_t DegreesToBams(float degrees) {
        return DegToBAMSF(degrees);
    }

    [[nodiscard]] static float BamsToDegrees(std::int32_t bams) {
        return BAMSFToDeg(bams);
    }

    [[nodiscard]] static float RadiansToBams(float radians) {
        return static_cast<float>(RadToBAMSF(radians));
    }

    [[nodiscard]] static float BamsToRadians(std::int32_t bams) {
        return BAMSFToRad(bams);
    }
};

} // namespace Sa3Dport::Structs
