#pragma once

#include "Animation/Enums.h"
#include "Structs/ColorIOType.h"
#include "Structs/EndianIOExtensions.h"
#include "Structs/EndianStackReader.h"
#include "Structs/FloatIOType.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace Sa3Dport::Animation {

struct KeyframeChannelSummary {
    KeyframeAttributes type = KeyframeAttributes::None;
    std::uint32_t pointer = 0;
    std::uint32_t count = 0;
    std::uint32_t first_frame = 0;
    std::uint32_t last_frame = 0;
};

struct Keyframes {
    KeyframeAttributes type = KeyframeAttributes::None;
    std::uint32_t keyframe_count = 0;
    std::vector<KeyframeChannelSummary> channels;

    [[nodiscard]] bool has_keyframes() const {
        return !channels.empty();
    }
};

[[nodiscard]] inline constexpr std::uint32_t keyframe_entry_size(KeyframeAttributes type) {
    switch (type) {
    case KeyframeAttributes::Position:
    case KeyframeAttributes::Scale:
    case KeyframeAttributes::Vector:
    case KeyframeAttributes::Target:
        return 16;
    case KeyframeAttributes::EulerRotation:
        return 16;
    case KeyframeAttributes::Vertex:
    case KeyframeAttributes::Normal:
    case KeyframeAttributes::Roll:
    case KeyframeAttributes::Angle:
    case KeyframeAttributes::LightColor:
    case KeyframeAttributes::Intensity:
        return 8;
    case KeyframeAttributes::Spot:
        return 24;
    case KeyframeAttributes::Point:
        return 12;
    case KeyframeAttributes::QuaternionRotation:
        return 20;
    case KeyframeAttributes::None:
        return 0;
    }
    return 0;
}

[[nodiscard]] inline std::optional<std::uint32_t> read_motion_pointer(
    const Structs::EndianStackReader& reader,
    std::uint32_t address,
    std::uint32_t imageBase) {
    const std::uint32_t raw = reader.read_u32(address);
    if (raw == 0) {
        return std::nullopt;
    }
    return raw - imageBase;
}

[[nodiscard]] inline std::uint32_t read_frame_at(
    const Structs::EndianStackReader& reader,
    std::uint32_t address,
    KeyframeAttributes type,
    bool shortRot) {
    if (type == KeyframeAttributes::EulerRotation && shortRot) {
        return reader.read_u16(address);
    }
    return reader.read_u32(address);
}

inline void validate_keyframe_value_reads(
    const Structs::EndianStackReader& reader,
    std::uint32_t address,
    std::uint32_t count,
    KeyframeAttributes type,
    bool shortRot,
    std::uint32_t keyframeTableAddress,
    std::uint32_t imageBase) {
    using Structs::FloatIOType;
    if (count == 0) {
        return;
    }

    switch (type) {
    case KeyframeAttributes::Position:
    case KeyframeAttributes::Scale:
    case KeyframeAttributes::Vector:
    case KeyframeAttributes::Target:
        for (std::uint32_t i = 0; i < count; ++i) {
            (void)reader.read_u32(address);
            address += 4;
            (void)Structs::EndianIOExtensions::read_vector3(reader, address, FloatIOType::Float);
            address += 12;
        }
        break;
    case KeyframeAttributes::EulerRotation: {
        const auto rotType = shortRot ? FloatIOType::BAMS16F : FloatIOType::BAMS32F;
        const std::uint32_t frameSize = shortRot ? 2u : 4u;
        const std::uint32_t valueSize = static_cast<std::uint32_t>(Structs::byte_size(rotType) * 3);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (shortRot) {
                (void)reader.read_u16(address);
            } else {
                (void)reader.read_u32(address);
            }
            address += frameSize;
            (void)Structs::EndianIOExtensions::read_vector3(reader, address, rotType);
            address += valueSize;
        }
        break;
    }
    case KeyframeAttributes::Vertex:
    case KeyframeAttributes::Normal: {
        std::vector<std::uint32_t> frameAddresses;
        frameAddresses.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            (void)reader.read_u32(address);
            const auto vectorAddress = read_motion_pointer(reader, address + 4, imageBase);
            if (vectorAddress.has_value()) {
                frameAddresses.push_back(*vectorAddress);
            }
            address += 8;
        }
        std::sort(frameAddresses.begin(), frameAddresses.end());
        frameAddresses.erase(std::unique(frameAddresses.begin(), frameAddresses.end()), frameAddresses.end());
        if (!frameAddresses.empty()) {
            std::uint32_t smallestSize = (keyframeTableAddress - frameAddresses.back()) / 12u;
            for (std::size_t i = 1; i < frameAddresses.size(); ++i) {
                for (std::size_t j = 0; j < i; ++j) {
                    smallestSize = std::min(smallestSize, (frameAddresses[i] - frameAddresses[j]) / 12u);
                }
            }
            for (const auto vectorAddress : frameAddresses) {
                std::uint32_t vectorRead = vectorAddress;
                for (std::uint32_t j = 0; j < smallestSize; ++j) {
                    (void)Structs::EndianIOExtensions::read_vector3(reader, vectorRead, FloatIOType::Float);
                    vectorRead += 12;
                }
            }
        }
        break;
    }
    case KeyframeAttributes::Roll:
    case KeyframeAttributes::Angle:
    case KeyframeAttributes::Intensity:
        for (std::uint32_t i = 0; i < count; ++i) {
            (void)reader.read_u32(address);
            (void)reader.read_u32(address + 4);
            address += 8;
        }
        break;
    case KeyframeAttributes::LightColor:
        for (std::uint32_t i = 0; i < count; ++i) {
            (void)reader.read_u32(address);
            (void)Structs::EndianIOExtensions::read_color(reader, address + 4, Structs::ColorIOType::ARGB8_32);
            address += 8;
        }
        break;
    case KeyframeAttributes::Spot:
        for (std::uint32_t i = 0; i < count; ++i) {
            (void)reader.read_u32(address);
            (void)reader.read_float(address + 4);
            (void)reader.read_float(address + 8);
            (void)reader.read_i32(address + 12);
            (void)reader.read_i32(address + 16);
            address += 24;
        }
        break;
    case KeyframeAttributes::Point:
        for (std::uint32_t i = 0; i < count; ++i) {
            (void)reader.read_u32(address);
            address += 4;
            (void)Structs::EndianIOExtensions::read_vector2(reader, address, FloatIOType::Float);
            address += 8;
        }
        break;
    case KeyframeAttributes::QuaternionRotation:
        for (std::uint32_t i = 0; i < count; ++i) {
            (void)reader.read_u32(address);
            address += 4;
            (void)Structs::EndianIOExtensions::read_quaternion(reader, address);
            address += 16;
        }
        break;
    case KeyframeAttributes::None:
        break;
    }
}

[[nodiscard]] inline Keyframes read_keyframes(
    const Structs::EndianStackReader& reader,
    std::uint32_t& address,
    KeyframeAttributes type,
    std::uint32_t imageBase,
    bool shortRot = false) {
    const int channels = channel_count(type);
    std::uint32_t keyframePointerArray = address;
    std::uint32_t keyframeCountArray = address + static_cast<std::uint32_t>(4 * channels);

    Keyframes result;
    for (const KeyframeAttributes flag : kKeyframeAttributeOrder) {
        if (!has_flag(type, flag)) {
            continue;
        }

        const auto setAddress = read_motion_pointer(reader, keyframePointerArray, imageBase);
        if (setAddress.has_value()) {
            const std::uint32_t frameCount = reader.read_u32(keyframeCountArray);
            KeyframeChannelSummary channel;
            channel.type = flag;
            channel.pointer = *setAddress;
            channel.count = frameCount;
            if (frameCount > 0) {
                channel.first_frame = read_frame_at(reader, *setAddress, flag, shortRot);
                std::uint32_t lastAddress = *setAddress;
                for (std::uint32_t i = 0; i < frameCount; ++i) {
                    channel.last_frame = read_frame_at(reader, lastAddress, flag, shortRot);
                    lastAddress += (flag == KeyframeAttributes::EulerRotation && shortRot) ? 8u : keyframe_entry_size(flag);
                }
            }
            if (frameCount > 0) {
                result.type |= flag;
                result.keyframe_count = std::max(result.keyframe_count, channel.last_frame + 1u);
                result.channels.push_back(channel);
            }
        }

        keyframePointerArray += 4;
        keyframeCountArray += 4;
    }

    address = keyframeCountArray;
    return result;
}

} // namespace Sa3Dport::Animation
