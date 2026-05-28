#pragma once

#include "Animation/Motion.h"
#include "File/FileHeaders.h"
#include "File/NJBlockUtility.h"

#include <cstdint>
#include <span>
#include <stdexcept>

namespace Sa3Dport::Testing::Slice7 {

using Motion = ::Sa3Dport::Animation::Motion;

[[nodiscard]] inline bool CheckIsAnimationBlock(std::span<const std::byte> data) {
    const auto scan = ::Sa3Dport::File::NJBlockUtility::ScanBlocks(data);
    for (const auto& block : scan.blocks) {
        if (block.role == ::Sa3Dport::File::NJBlockRole::Animation) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline Motion ReadMotionBlock(std::span<const std::byte> data,
                                            std::uint32_t nodeCount,
                                            bool shortRot = false) {
    const auto scan = ::Sa3Dport::File::NJBlockUtility::ScanBlocks(data);
    for (const auto& block : scan.blocks) {
        if (block.role != ::Sa3Dport::File::NJBlockRole::Animation) {
            continue;
        }

        const std::uint32_t dataAddress = block.offset + 8u;
        const std::uint32_t imageBase = 0u - dataAddress;
        const ::Sa3Dport::Structs::EndianStackReader reader(data, scan.size_endian);
        return ::Sa3Dport::Animation::Motion::read(reader, dataAddress, nodeCount, imageBase, shortRot);
    }

    throw std::runtime_error("no animation block found");
}

} // namespace Sa3Dport::Testing::Slice7
