#pragma once

#include "File/FileHeaders.h"
#include "File/NJBlockUtility.h"
#include "ObjectData/Enums/ModelFormat.h"
#include "ObjectData/Node.h"
#include "Structs/EndianStackReader.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>

namespace Sa3Dport::File {

class ModelFile {
public:
    bool nj_file = false;
    ObjectData::Enums::ModelFormat format = ObjectData::Enums::ModelFormat::SA2;
    ObjectData::NodePtr model;
    std::optional<std::uint32_t> model_block_address;
    std::optional<std::uint32_t> texture_list_block_address;

    [[nodiscard]] static bool check_is_model_file(std::span<const std::byte> data, std::uint32_t address = 0) {
        return check_is_nj_model_file(data, address);
    }

    [[nodiscard]] static bool check_is_nj_model_file(std::span<const std::byte> data, std::uint32_t address = 0) {
        const auto blocks = NJBlockUtility::GetBlockAddresses(data, address);
        return NJBlockUtility::FindBlockAddress(blocks, FileHeaders::ModelBlockHeaders).has_value();
    }

    [[nodiscard]] static ModelFile read_from_bytes(std::span<const std::byte> data, std::uint32_t address = 0) {
        return read(data, address);
    }

    [[nodiscard]] static ModelFile read(std::span<const std::byte> data, std::uint32_t address = 0) {
        if (!check_is_nj_model_file(data, address)) {
            throw std::runtime_error("File is not a model file");
        }
        return read_nj(data, address);
    }

    [[nodiscard]] static ModelFile read_nj(std::span<const std::byte> data, std::uint32_t address = 0) {
        const auto scan = NJBlockUtility::ScanBlocks(data, address);
        const auto modelBlockAddress = NJBlockUtility::FindBlockAddress(scan.blocks, FileHeaders::ModelBlockHeaders);
        if (!modelBlockAddress.has_value()) {
            throw std::runtime_error("NJ model block not found");
        }

        ModelFile result;
        result.nj_file = true;
        result.model_block_address = *modelBlockAddress;
        result.texture_list_block_address = NJBlockUtility::FindBlockAddress(scan.blocks, FileHeaders::TextureListBlockHeaders);

        const ::Sa3Dport::Structs::EndianStackReader reader(data, scan.size_endian);
        const std::uint32_t modelAddress = *modelBlockAddress + 8u;
        std::uint32_t blockHeader = 0;
        for (const auto& block : scan.blocks) {
            if (block.offset == *modelBlockAddress) {
                blockHeader = block.header;
                break;
            }
        }
        result.format = (blockHeader == FileHeaders::ModelBlockHeaders[0])
            ? ObjectData::Enums::ModelFormat::SA2
            : ObjectData::Enums::ModelFormat::SA1;

        ObjectData::NodeReadContext context;
        context.image_base = 0u - modelAddress;
        context.read_attach = result.format == ObjectData::Enums::ModelFormat::SA2;
        result.model = ObjectData::Node::read(reader, modelAddress, result.format, context);
        return result;
    }
};

} // namespace Sa3Dport::File
