#pragma once

#include "File/FileHeaders.h"
#include "Structs/BAMSFHelper.h"
#include "Structs/EndianIOExtensions.h"
#include "Structs/MathHelper.h"
#include "Structs/PointerLUT.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Sa3Dport::Testing::Slice1 {

inline constexpr auto kNjcmMagic = File::FileHeaders::ToMagic(File::FileHeaders::NJCM);
inline constexpr auto kNjtlMagic = File::FileHeaders::ToMagic(File::FileHeaders::NJTL);

inline constexpr bool MatchesMagic(const std::array<char, 4>& candidate,
                                   const std::array<char, 4>& expected) {
    return File::FileHeaders::MatchesMagic(candidate, File::FileHeaders::FromMagic(expected));
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

inline std::int32_t DegreesToBams(float degrees) {
    return Structs::MathCompat::round_to_even_i32(degrees * (65536.0f / 360.0f));
}

inline float BamsToDegrees(std::int32_t bams) {
    return static_cast<float>(bams) * (360.0f / 65536.0f);
}

inline float RadiansToBams(float radians) {
    return static_cast<float>(Structs::MathHelper::rad_to_bams(radians));
}

inline float BamsToRadians(std::int32_t bams) {
    return Structs::MathHelper::bams_to_rad(bams);
}

} // namespace Sa3Dport::Testing::Slice1
