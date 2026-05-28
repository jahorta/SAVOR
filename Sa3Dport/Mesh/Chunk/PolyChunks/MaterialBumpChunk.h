#pragma once

#include "Mesh/Chunk/PolyChunk.h"

#include <array>
#include <cstdint>

namespace Sa3Dport::Mesh::Chunk::PolyChunks {

class MaterialBumpChunk : public SizedPolyChunk {
public:
    std::uint16_t dx = 0;
    std::uint16_t dy = 0;
    std::uint16_t dz = 0;
    std::uint16_t ux = 0;
    std::uint16_t uy = 0;
    std::uint16_t uz = 0;

    MaterialBumpChunk() : SizedPolyChunk(PolyChunkType::Material_Bump) {}

    [[nodiscard]] std::uint16_t size() const override {
        return 6u;
    }

    [[nodiscard]] static MaterialBumpChunk read(const Sa3Dport::Structs::EndianStackReader& reader,
                                                std::uint32_t address) {
        const std::uint16_t header = reader.read_u16(address);
        address += 4u;

        MaterialBumpChunk result;
        result.attributes = static_cast<std::uint8_t>(header >> 8u);
        result.dx = reader.read_u16(address);
        result.dy = reader.read_u16(address + 2u);
        result.dz = reader.read_u16(address + 4u);
        result.ux = reader.read_u16(address + 6u);
        result.uy = reader.read_u16(address + 8u);
        result.uz = reader.read_u16(address + 10u);
        return result;
    }
};

} // namespace Sa3Dport::Mesh::Chunk::PolyChunks
