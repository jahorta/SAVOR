#pragma once

#include "Mesh/Chunk/PolyChunk.h"

#include <cstdlib>
#include <cstdint>

namespace Sa3Dport::Mesh::Chunk::PolyChunks {

class VolumeChunk : public SizedPolyChunk {
public:
    std::uint16_t polygon_count = 0;
    std::uint8_t polygon_attribute_count = 0;
    std::uint16_t size_words = 0;

    explicit VolumeChunk(PolyChunkType chunkType = PolyChunkType::Volume_Polygon3)
        : SizedPolyChunk(chunkType) {}

    [[nodiscard]] std::uint16_t size() const override {
        return size_words;
    }

    [[nodiscard]] static VolumeChunk read(const Sa3Dport::Structs::EndianStackReader& reader,
                                          std::uint32_t& address) {
        const auto start = address;
        const std::uint16_t header = reader.read_u16(address);
        const std::uint16_t header2 = reader.read_u16(address + 4u);

        VolumeChunk result(static_cast<PolyChunkType>(header & 0xFFu));
        result.attributes = static_cast<std::uint8_t>(header >> 8u);
        result.polygon_count = static_cast<std::uint16_t>(header2 & 0x3FFFu);
        result.polygon_attribute_count = static_cast<std::uint8_t>(header2 >> 14u);

        address += 6u;
        switch (result.type) {
        case PolyChunkType::Volume_Polygon3:
            address += static_cast<std::uint32_t>(result.polygon_count) *
                       (6u + (static_cast<std::uint32_t>(result.polygon_attribute_count) * 2u));
            break;
        case PolyChunkType::Volume_Polygon4:
            address += static_cast<std::uint32_t>(result.polygon_count) *
                       (8u + (static_cast<std::uint32_t>(result.polygon_attribute_count) * 2u));
            break;
        case PolyChunkType::Volume_Strip:
            for (std::uint16_t i = 0; i < result.polygon_count; ++i) {
                const auto countValue = reader.read_i16(address);
                const auto count = static_cast<std::uint32_t>(std::abs(static_cast<int>(countValue)));
                address += 2u + (count * 2u);
                if (count > 2u) {
                    address += (count - 2u) * static_cast<std::uint32_t>(result.polygon_attribute_count) * 2u;
                }
            }
            break;
        default:
            break;
        }

        result.size_words = static_cast<std::uint16_t>((address - start - 4u) / 2u);
        return result;
    }
};

} // namespace Sa3Dport::Mesh::Chunk::PolyChunks
