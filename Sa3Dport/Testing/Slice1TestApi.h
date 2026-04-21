#pragma once

#include "File/FileHeaders.h"
#include "Structs/BAMSFHelper.h"
#include "Structs/EndianIOExtensions.h"
#include "Structs/PointerLUT.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Sa3Dport::Testing::Slice1 {

inline constexpr auto kNjcmMagic = File::FileHeaders::kNjcmMagic;
inline constexpr auto kNjtlMagic = File::FileHeaders::kNjtlMagic;

inline constexpr bool MatchesMagic(const std::array<char, 4>& candidate,
                                   const std::array<char, 4>& expected) {
    return File::FileHeaders::MatchesMagic(candidate, expected);
}

using Endianness = Structs::Endianness;
using EndianReader = Structs::EndianReader;

inline EndianReader MakeReader(std::span<const std::byte> buffer,
                               Endianness endianness,
                               std::uint32_t imageBase = 0) {
    return EndianReader(buffer, endianness, imageBase);
}

template <typename T>
using PointerLUT = Structs::PointerLUT<T>;

inline constexpr float kPi = Structs::BAMSFHelper::kPi;

inline constexpr std::int32_t DegreesToBams(float degrees) {
    return Structs::BAMSFHelper::DegreesToBams(degrees);
}

inline constexpr float BamsToDegrees(std::int32_t bams) {
    return Structs::BAMSFHelper::BamsToDegrees(bams);
}

inline float RadiansToBams(float radians) {
    return Structs::BAMSFHelper::RadiansToBams(radians);
}

inline float BamsToRadians(std::int32_t bams) {
    return Structs::BAMSFHelper::BamsToRadians(bams);
}

} // namespace Sa3Dport::Testing::Slice1
