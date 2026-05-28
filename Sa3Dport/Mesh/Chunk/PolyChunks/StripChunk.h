#pragma once

#include "Mesh/Chunk/ChunkTypeExtensions.h"
#include "Mesh/Chunk/PolyChunk.h"
#include "Mesh/Chunk/Structs/ChunkStrip.h"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Sa3Dport::Mesh::Chunk::PolyChunks {

class StripChunk : public SizedPolyChunk {
public:
    std::vector<Sa3Dport::Mesh::Chunk::Structs::ChunkStrip> strips;
    std::uint8_t triangle_attribute_count = 0;

    explicit StripChunk(PolyChunkType chunkType = PolyChunkType::Strip_Blank)
        : SizedPolyChunk(chunkType) {
        if (!check_is_strip(chunkType)) {
            throw std::invalid_argument("poly chunk type is not a strip chunk type");
        }
    }

    [[nodiscard]] int texcoord_count() const {
        return get_strip_texcoord_count(type);
    }

    [[nodiscard]] bool has_hd_texcoords() const {
        return check_strip_has_hd_texcoords(type);
    }

    [[nodiscard]] bool has_normals() const {
        return check_strip_has_normals(type);
    }

    [[nodiscard]] bool has_colors() const {
        return check_strip_has_colors(type);
    }

    [[nodiscard]] std::uint32_t raw_size() const {
        std::uint32_t result = 2u;
        const int texcoordCount = texcoord_count();
        const bool hasNormals = has_normals();
        const bool hasColors = has_colors();
        for (const auto& strip : strips) {
            result += strip.size(texcoordCount, hasNormals, hasColors, triangle_attribute_count);
        }
        return result / 2u;
    }

    [[nodiscard]] std::uint16_t size() const override {
        const std::uint32_t result = raw_size();
        if (result > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("strip chunk size exceeds maximum ushort size");
        }
        return static_cast<std::uint16_t>(result);
    }

    [[nodiscard]] bool ignore_light() const { return (attributes & 0x01u) != 0; }
    [[nodiscard]] bool ignore_specular() const { return (attributes & 0x02u) != 0; }
    [[nodiscard]] bool ignore_ambient() const { return (attributes & 0x04u) != 0; }
    [[nodiscard]] bool use_alpha() const { return (attributes & 0x08u) != 0; }
    [[nodiscard]] bool double_side() const { return (attributes & 0x10u) != 0; }
    [[nodiscard]] bool flat_shading() const { return (attributes & 0x20u) != 0; }
    [[nodiscard]] bool environment_mapping() const { return (attributes & 0x40u) != 0; }
    [[nodiscard]] bool no_alpha_test() const { return (attributes & 0x80u) != 0; }

    [[nodiscard]] static StripChunk read(const Sa3Dport::Structs::EndianStackReader& reader,
                                         std::uint32_t& address) {
        const std::uint16_t header = reader.read_u16(address);
        const std::uint16_t header2 = reader.read_u16(address + 4u);
        const auto readType = static_cast<PolyChunkType>(header & 0xFFu);
        const auto polyCount = static_cast<std::uint16_t>(header2 & 0x3FFFu);
        const auto triangleAttributeCount = static_cast<std::uint8_t>(header2 >> 14u);

        StripChunk result(readType);
        result.attributes = static_cast<std::uint8_t>(header >> 8u);
        result.triangle_attribute_count = triangleAttributeCount;
        result.strips.resize(polyCount);

        address += 6u;
        const int texcoordCount = result.texcoord_count();
        const bool hdTexcoord = result.has_hd_texcoords();
        const bool hasNormals = result.has_normals();
        const bool hasColors = result.has_colors();

        for (auto& strip : result.strips) {
            strip = Sa3Dport::Mesh::Chunk::Structs::ChunkStrip::read(
                reader, address, texcoordCount, hdTexcoord, hasNormals, hasColors, triangleAttributeCount);
        }

        return result;
    }
};

} // namespace Sa3Dport::Mesh::Chunk::PolyChunks
