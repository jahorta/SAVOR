#pragma once

#include "Animation/Motion.h"
#include "File/FileHeaders.h"
#include "File/NJBlockUtility.h"
#include "Structs/EndianStackReader.h"

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>

namespace Sa3Dport::File {

class AnimationFile {
public:
    Animation::Motion animation;
    std::optional<std::uint32_t> animation_block_address;

    [[nodiscard]] static bool check_is_animation_file(std::span<const std::byte> data, std::uint32_t address = 0) {
        return check_is_nj_animation_file(data, address);
    }

    [[nodiscard]] static bool check_is_nj_animation_file(std::span<const std::byte> data, std::uint32_t address = 0) {
        const auto blocks = NJBlockUtility::GetBlockAddresses(data, address);
        return NJBlockUtility::FindBlockAddress(blocks, FileHeaders::AnimationBlockHeaders).has_value();
    }

    [[nodiscard]] static AnimationFile read_from_bytes(std::span<const std::byte> data,
                                                       std::uint32_t nodeCount,
                                                       bool shortRot = false,
                                                       std::uint32_t address = 0) {
        return read(data, nodeCount, shortRot, address);
    }

    [[nodiscard]] static AnimationFile read(std::span<const std::byte> data,
                                            std::uint32_t nodeCount,
                                            bool shortRot = false,
                                            std::uint32_t address = 0) {
        if (!check_is_nj_animation_file(data, address)) {
            throw std::runtime_error("File is not an animation file");
        }
        return read_nj(data, nodeCount, shortRot, address);
    }

    [[nodiscard]] static AnimationFile read_nj(std::span<const std::byte> data,
                                               std::uint32_t nodeCount,
                                               bool shortRot = false,
                                               std::uint32_t address = 0) {
        const auto payload = NJBlockUtility::RequireBlockPayload(
            data, address, FileHeaders::AnimationBlockHeaders, "NJ animation block not found");

        AnimationFile result;
        result.animation_block_address = payload.block.offset;
        result.animation = Animation::Motion::read(
            payload.reader, payload.data_address, nodeCount, payload.image_base, shortRot);
        return result;
    }
};

} // namespace Sa3Dport::File
