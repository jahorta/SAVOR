#pragma once

#include "File/MetaBlockType.h"
#include "File/Structs/MetaWeightNode.h"
#include "Structs/Endian.h"
#include "Structs/EndianStackReader.h"
#include "Structs/PointerIO.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Sa3Dport::File {

struct MetaData {
    std::string author;
    std::string description;
    std::string action_name;
    std::string object_name;
    std::map<std::uint32_t, std::string> labels;
    std::vector<std::string> animation_files;
    std::vector<std::string> morph_files;
    std::vector<Structs::MetaWeightNode> meta_weights;
    std::map<std::uint32_t, std::vector<std::byte>> other;
    std::vector<std::string> diagnostics;
};

struct MetaDataSummary {
    std::size_t label_count = 0;
    std::size_t animation_file_count = 0;
    std::size_t morph_file_count = 0;
    std::size_t meta_weight_node_count = 0;
    std::size_t meta_weight_vertex_count = 0;
    std::size_t meta_weight_count = 0;
    std::size_t unknown_block_count = 0;
    std::size_t diagnostic_count = 0;
    bool has_author = false;
    bool has_description = false;
    bool has_action_name = false;
    bool has_object_name = false;
};

class MetaDataReader {
public:
    [[nodiscard]] static MetaData Read(std::span<const std::byte> data,
                                       std::uint32_t address,
                                       int version,
                                       bool hasAnimMorphFiles,
                                       Sa3Dport::Structs::Endian endian = Sa3Dport::Structs::Endian::Little) {
        MetaDataReader reader(data, endian);
        return reader.Read(address, version, hasAnimMorphFiles);
    }

    [[nodiscard]] static MetaDataSummary Summarize(const MetaData& metaData) {
        MetaDataSummary result;
        result.label_count = metaData.labels.size();
        result.animation_file_count = metaData.animation_files.size();
        result.morph_file_count = metaData.morph_files.size();
        result.meta_weight_node_count = metaData.meta_weights.size();
        result.unknown_block_count = metaData.other.size();
        result.diagnostic_count = metaData.diagnostics.size();
        result.has_author = !metaData.author.empty();
        result.has_description = !metaData.description.empty();
        result.has_action_name = !metaData.action_name.empty();
        result.has_object_name = !metaData.object_name.empty();

        for (const auto& node : metaData.meta_weights) {
            result.meta_weight_vertex_count += node.vertex_weights.size();
            for (const auto& vertex : node.vertex_weights) {
                result.meta_weight_count += vertex.weights.size();
            }
        }

        return result;
    }

private:
    MetaDataReader(std::span<const std::byte> data, Sa3Dport::Structs::Endian endian)
        : data_(data), reader_(data, endian) {}

    [[nodiscard]] MetaData Read(std::uint32_t address, int version, bool hasAnimMorphFiles) {
        MetaData result;
        imageBase_ = 0;

        try {
            switch (version) {
            case 0:
                ReadVersion0(result, address);
                break;
            case 1:
                ReadVersion1(result, address, hasAnimMorphFiles);
                break;
            case 2:
                ReadVersion2(result, address);
                break;
            case 3:
                ReadVersion3(result, address);
                break;
            default:
                result.diagnostics.push_back("unsupported_metadata_version");
                break;
            }
        } catch (const std::out_of_range&) {
            result.diagnostics.push_back("metadata_read_out_of_range");
        }

        return result;
    }

    void ReadVersion0(MetaData& result, std::uint32_t address) {
        std::uint32_t dataAddress = 0;
        if (TryReadPointer(address, dataAddress)) {
            ReadStringPointerListAt(result.animation_files, dataAddress);
        }

        if (TryReadPointer(address + 4u, dataAddress)) {
            ReadStringPointerListAt(result.morph_files, dataAddress);
        }
    }

    void ReadVersion1(MetaData& result, std::uint32_t address, bool hasAnimMorphFiles) {
        if (hasAnimMorphFiles) {
            ReadVersion0(result, address);
            address += 8u;
        }

        std::uint32_t labelsAddress = 0;
        if (!TryReadPointer(address, labelsAddress)) {
            return;
        }

        std::uint32_t labelPointer = ReadUInt(labelsAddress);
        while (labelPointer != UINT32_MAX) {
            const std::uint32_t labelTextPointer = ReadUInt(labelsAddress + 4u);
            result.labels[labelPointer] = ReadString(labelTextPointer);
            labelsAddress += 8u;
            labelPointer = ReadUInt(labelsAddress);
        }
    }

    bool ReadMetaBlockType(MetaData& result, std::uint32_t& address, MetaBlockType type) {
        switch (type) {
        case MetaBlockType::Label:
            while (ReadULong(address) != UINT64_MAX) {
                const std::uint32_t labelAddress = ReadUInt(address);
                address += 4u;
                const std::string label = ReadString(ReadPointer(address));
                result.labels[labelAddress] = label;
                address += 4u;
            }
            break;
        case MetaBlockType::Animation:
            ReadStringPointerList(result.animation_files, address);
            break;
        case MetaBlockType::Morph:
            ReadStringPointerList(result.morph_files, address);
            break;
        case MetaBlockType::Author:
            result.author = ReadString(address);
            break;
        case MetaBlockType::Description:
            result.description = ReadString(address);
            break;
        case MetaBlockType::ActionName:
            result.action_name = ReadString(address);
            break;
        case MetaBlockType::ObjectName:
            result.object_name = ReadString(address);
            break;
        case MetaBlockType::Weight:
            while (ReadUInt(address) != UINT32_MAX) {
                result.meta_weights.push_back(Structs::MetaWeightNode::Read(reader_, address));
            }
            break;
        case MetaBlockType::Tool:
        case MetaBlockType::Texture:
        case MetaBlockType::End:
            break;
        default:
            return false;
        }

        return true;
    }

    void ReadVersion2(MetaData& result, std::uint32_t address) {
        std::uint32_t metaAddress = 0;
        if (!TryReadPointer(address, metaAddress)) {
            return;
        }

        MetaBlockType type = static_cast<MetaBlockType>(ReadUInt(metaAddress));
        while (type != MetaBlockType::End) {
            const std::uint32_t blockSize = ReadUInt(metaAddress + 4u);
            std::uint32_t blockStart = metaAddress + 8u;
            metaAddress = blockStart + blockSize;
            if (!ReadMetaBlockType(result, blockStart, type)) {
                StoreOther(result, type, blockStart, blockSize);
            }

            type = static_cast<MetaBlockType>(ReadUInt(metaAddress));
        }
    }

    void ReadVersion3(MetaData& result, std::uint32_t address) {
        std::uint32_t metaAddress = 0;
        if (!TryReadPointer(address, metaAddress)) {
            return;
        }

        MetaBlockType type = static_cast<MetaBlockType>(ReadUInt(metaAddress));
        while (type != MetaBlockType::End) {
            const std::uint32_t blockSize = ReadUInt(metaAddress + 4u);
            metaAddress += 8u;
            imageBase_ = static_cast<std::uint32_t>(0u - metaAddress);

            std::uint32_t blockAddress = metaAddress;
            if (!ReadMetaBlockType(result, blockAddress, type)) {
                StoreOther(result, type, blockAddress, blockSize);
            }

            metaAddress += blockSize;
            type = static_cast<MetaBlockType>(ReadUInt(metaAddress));
        }
    }

    void ReadStringPointerList(std::vector<std::string>& values, std::uint32_t& address) {
        std::uint32_t pathAddress = ReadPointer(address);
        while (pathAddress != UINT32_MAX) {
            values.push_back(ReadString(pathAddress));
            address += 4u;
            pathAddress = ReadPointer(address);
        }
    }

    void ReadStringPointerListAt(std::vector<std::string>& values, std::uint32_t address) {
        ReadStringPointerList(values, address);
    }

    void StoreOther(MetaData& result, MetaBlockType type, std::uint32_t address, std::uint32_t blockSize) {
        const auto offset = static_cast<std::size_t>(address);
        const auto size = static_cast<std::size_t>(blockSize);
        if (offset + size > data_.size()) {
            result.diagnostics.push_back("unknown_metadata_block_out_of_range");
            return;
        }

        result.other[static_cast<std::uint32_t>(type)] =
            std::vector<std::byte>(data_.begin() + offset, data_.begin() + offset + size);
    }

    [[nodiscard]] bool TryReadPointer(std::uint32_t address, std::uint32_t& value) const {
        const auto pointer = Sa3Dport::Structs::PointerIO::read_nullable_pointer_add_base(reader_, address, imageBase_);
        if (!pointer.has_value()) {
            return false;
        }

        value = *pointer;
        return static_cast<std::size_t>(value) < data_.size();
    }

    [[nodiscard]] std::uint32_t ReadPointer(std::uint32_t address) const {
        return Sa3Dport::Structs::PointerIO::read_pointer_add_base(reader_, address, imageBase_);
    }

    [[nodiscard]] std::uint32_t ReadUInt(std::uint32_t address) const {
        return reader_.read_u32(address);
    }

    [[nodiscard]] std::uint64_t ReadULong(std::uint32_t address) const {
        const std::uint64_t low = reader_.read_u32(address);
        const std::uint64_t high = reader_.read_u32(address + 4u);
        if (reader_.endian() == Sa3Dport::Structs::Endian::Little) {
            return low | (high << 32);
        }

        return (low << 32) | high;
    }

    [[nodiscard]] std::string ReadString(std::uint32_t address) const {
        std::string result;
        for (std::size_t i = address; i < data_.size(); ++i) {
            const char c = static_cast<char>(std::to_integer<unsigned char>(data_[i]));
            if (c == '\0') {
                break;
            }

            result.push_back(c);
        }

        return result;
    }

    std::span<const std::byte> data_;
    Sa3Dport::Structs::EndianStackReader reader_;
    std::uint32_t imageBase_ = 0;
};

} // namespace Sa3Dport::File
