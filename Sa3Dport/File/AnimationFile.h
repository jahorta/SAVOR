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
        const auto scan = NJBlockUtility::ScanBlocks(data, address);
        const auto blockAddress = NJBlockUtility::FindBlockAddress(scan.blocks, FileHeaders::AnimationBlockHeaders);
        if (!blockAddress.has_value()) {
            throw std::runtime_error("NJ animation block not found");
        }

        const std::uint32_t dataAddress = *blockAddress + 8u;
        const std::uint32_t imageBase = 0u - dataAddress;
        const ::Sa3Dport::Structs::EndianStackReader reader(data, scan.size_endian);

        AnimationFile result;
        result.animation_block_address = *blockAddress;
        result.animation = Animation::Motion::read(reader, dataAddress, nodeCount, imageBase, shortRot);
        return result;
    }
};

} // namespace Sa3Dport::File
