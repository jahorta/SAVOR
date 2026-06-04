#pragma once

#include "File/FileHeaders.h"
#include "Structs/Endian.h"
#include "Structs/EndianStackReader.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Sa3Dport::File {

enum class NJBlockRole {
    None,
    Model,
    Texture,
    Animation,
};

struct NJBlockInfo {
    std::uint32_t offset = 0;
    std::uint32_t header = 0;
    std::uint32_t size = 0;
    NJBlockRole role = NJBlockRole::None;
};

struct NJBlockScanResult {
    std::vector<NJBlockInfo> blocks;
    std::vector<std::string> diagnostics;
    ::Sa3Dport::Structs::Endian size_endian = ::Sa3Dport::Structs::Endian::Little;
};

struct NJBlockPayload {
    NJBlockScanResult scan;
    NJBlockInfo block;
    std::uint32_t data_address = 0;
    std::uint32_t image_base = 0;
    ::Sa3Dport::Structs::EndianStackReader reader;

    NJBlockPayload(NJBlockScanResult scan_,
                   NJBlockInfo block_,
                   std::span<const std::byte> data)
        : scan(std::move(scan_)),
          block(block_),
          data_address(block.offset + 8u),
          image_base(0u - data_address),
          reader(data, scan.size_endian) {}
};

class NJBlockUtility {
public:
    [[nodiscard]] static NJBlockRole GetRole(std::uint32_t header) {
        if (FileHeaders::IsModelBlockHeader(header)) {
            return NJBlockRole::Model;
        }

        if (FileHeaders::IsTextureListBlockHeader(header)) {
            return NJBlockRole::Texture;
        }

        if (FileHeaders::IsAnimationBlockHeader(header)) {
            return NJBlockRole::Animation;
        }

        return NJBlockRole::None;
    }

    [[nodiscard]] static const char* RoleName(NJBlockRole role) {
        switch (role) {
        case NJBlockRole::Model:
            return "model";
        case NJBlockRole::Texture:
            return "texture";
        case NJBlockRole::Animation:
            return "animation";
        case NJBlockRole::None:
        default:
            return "none";
        }
    }

    [[nodiscard]] static NJBlockScanResult ScanBlocks(std::span<const std::byte> data,
                                                      std::uint32_t address = 0) {
        NJBlockScanResult result;
        if (address + 8u > data.size()) {
            result.diagnostics.push_back("address_out_of_range");
            return result;
        }

        result.size_endian = CheckBigEndian32(data, address + 4u) ? ::Sa3Dport::Structs::Endian::Big : ::Sa3Dport::Structs::Endian::Little;
        ::Sa3Dport::Structs::EndianStackReader headerReader(data, ::Sa3Dport::Structs::Endian::Little);
        ::Sa3Dport::Structs::EndianStackReader sizeReader(data, result.size_endian);

        std::uint32_t blockAddress = address;
        while (static_cast<std::uint64_t>(blockAddress) < static_cast<std::uint64_t>(data.size()) + 8u) {
            if (static_cast<std::uint64_t>(blockAddress) + 8u > data.size()) {
                result.diagnostics.push_back("truncated_block_header");
                break;
            }

            const std::uint32_t blockHeader = headerReader.read_u32(blockAddress);
            const std::uint32_t blockSize = sizeReader.read_u32(blockAddress + 4u);
            if (blockHeader == 0 || blockSize == 0) {
                break;
            }

            result.blocks.push_back({blockAddress, blockHeader, blockSize, GetRole(blockHeader)});

            const std::uint64_t nextBlockAddress =
                static_cast<std::uint64_t>(blockAddress) + 8u + static_cast<std::uint64_t>(blockSize);
            if (nextBlockAddress > static_cast<std::uint64_t>(UINT32_MAX)) {
                result.diagnostics.push_back("block_address_overflow");
                break;
            }

            blockAddress = static_cast<std::uint32_t>(nextBlockAddress);
        }

        return result;
    }

    [[nodiscard]] static std::vector<NJBlockInfo> GetBlockAddresses(std::span<const std::byte> data,
                                                                    std::uint32_t address = 0) {
        return ScanBlocks(data, address).blocks;
    }

    [[nodiscard]] static std::optional<std::uint32_t> FindBlockAddress(
        std::span<const NJBlockInfo> blocks,
        std::span<const std::uint32_t> toFind) {
        const auto block = FindBlock(blocks, toFind);
        if (block.has_value()) {
            return block->offset;
        }

        return std::nullopt;
    }

    [[nodiscard]] static std::optional<NJBlockInfo> FindBlock(
        std::span<const NJBlockInfo> blocks,
        std::span<const std::uint32_t> toFind) {
        for (const NJBlockInfo& block : blocks) {
            if (FileHeaders::Contains(toFind, block.header)) {
                return block;
            }
        }

        return std::nullopt;
    }

    [[nodiscard]] static std::optional<std::uint32_t> FindBlockAddress(
        std::span<const std::byte> data,
        std::uint32_t address,
        std::span<const std::uint32_t> toFind) {
        const auto blocks = GetBlockAddresses(data, address);
        return FindBlockAddress(blocks, toFind);
    }

    [[nodiscard]] static std::optional<NJBlockPayload> TryGetBlockPayload(
        std::span<const std::byte> data,
        std::uint32_t address,
        std::span<const std::uint32_t> toFind) {
        auto scan = ScanBlocks(data, address);
        const auto block = FindBlock(scan.blocks, toFind);
        if (!block.has_value()) {
            return std::nullopt;
        }

        return NJBlockPayload(std::move(scan), *block, data);
    }

    [[nodiscard]] static NJBlockPayload RequireBlockPayload(
        std::span<const std::byte> data,
        std::uint32_t address,
        std::span<const std::uint32_t> toFind,
        const char* errorMessage) {
        auto payload = TryGetBlockPayload(data, address, toFind);
        if (!payload.has_value()) {
            throw std::runtime_error(errorMessage);
        }

        return std::move(*payload);
    }

    [[nodiscard]] static bool CheckBigEndian32(std::span<const std::byte> data, std::uint32_t address) {
        if (address + 4u > data.size()) {
            return false;
        }

        const ::Sa3Dport::Structs::EndianStackReader little(data, ::Sa3Dport::Structs::Endian::Little);
        const ::Sa3Dport::Structs::EndianStackReader big(data, ::Sa3Dport::Structs::Endian::Big);
        const std::uint32_t littleValue = little.read_u32(address);
        const std::uint32_t bigValue = big.read_u32(address);

        const auto plausible = [remaining = data.size() - address](std::uint32_t value) {
            return value > 0 && static_cast<std::uint64_t>(value) <= static_cast<std::uint64_t>(remaining);
        };

        const bool littlePlausible = plausible(littleValue);
        const bool bigPlausible = plausible(bigValue);
        if (bigPlausible != littlePlausible) {
            return bigPlausible;
        }

        return false;
    }
};

} // namespace Sa3Dport::File
