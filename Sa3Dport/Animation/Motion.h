#pragma once

#include "Animation/Enums.h"
#include "Animation/Keyframes.h"
#include "Structs/EndianStackReader.h"

#include <algorithm>
#include <cstdint>
#include <map>

namespace Sa3Dport::Animation {

struct Motion {
    static constexpr std::uint32_t StructSize = 16;

    std::uint32_t node_count = 0;
    InterpolationMode interpolation_mode = InterpolationMode::Linear;
    bool short_rot = false;
    KeyframeAttributes manual_keyframe_types = KeyframeAttributes::None;
    std::map<int, Keyframes> keyframes;

    [[nodiscard]] KeyframeAttributes keyframe_types() const {
        KeyframeAttributes result = manual_keyframe_types;
        for (const auto& [_, frames] : keyframes) {
            result |= frames.type;
        }
        return result;
    }

    [[nodiscard]] std::uint32_t frame_count() const {
        std::uint32_t result = 0;
        for (const auto& [_, frames] : keyframes) {
            result = std::max(result, frames.keyframe_count);
        }
        return result;
    }

    [[nodiscard]] static Motion read(const Structs::EndianStackReader& reader,
                                     std::uint32_t address,
                                     std::uint32_t modelCount,
                                     std::uint32_t imageBase,
                                     bool shortRot = false) {
        std::uint32_t keyframeAddress = read_motion_pointer(reader, address, imageBase).value_or(0);
        const auto keyframeType = static_cast<KeyframeAttributes>(reader.read_u16(address + 8));
        const std::uint16_t tmp = reader.read_u16(address + 10);
        const auto mode = static_cast<InterpolationMode>((tmp >> 6) & 0x3u);

        Motion result;
        result.interpolation_mode = mode;
        result.node_count = modelCount;
        result.short_rot = shortRot;
        result.manual_keyframe_types = keyframeType;

        for (std::uint32_t i = 0; i < modelCount; ++i) {
            result.keyframes.emplace(static_cast<int>(i), read_keyframes(reader, keyframeAddress, keyframeType, imageBase, shortRot));
        }

        return result;
    }
};

} // namespace Sa3Dport::Animation
