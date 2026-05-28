#pragma once

#include "Mesh/Chunk/PolyChunk.h"

#include <cstdint>

namespace Sa3Dport::Mesh::Chunk::PolyChunks {

class TextureChunk : public PolyChunk {
public:
    std::uint16_t data = 0;

    explicit TextureChunk(bool second = false)
        : PolyChunk(second ? PolyChunkType::TextureID2 : PolyChunkType::TextureID) {}

    [[nodiscard]] std::uint32_t byte_size() const override {
        return 4u;
    }

    [[nodiscard]] bool second() const {
        return type == PolyChunkType::TextureID2;
    }

    [[nodiscard]] float mipmap_distance_multiplier() const {
        return static_cast<float>(attributes & 0xFu) * 0.25f;
    }

    [[nodiscard]] bool clamp_v() const { return (attributes & 0x10u) != 0; }
    [[nodiscard]] bool clamp_u() const { return (attributes & 0x20u) != 0; }
    [[nodiscard]] bool mirror_v() const { return (attributes & 0x40u) != 0; }
    [[nodiscard]] bool mirror_u() const { return (attributes & 0x80u) != 0; }

    [[nodiscard]] std::uint16_t texture_id() const {
        return static_cast<std::uint16_t>(data & 0x1FFFu);
    }

    [[nodiscard]] bool super_sample() const {
        return (data & 0x2000u) != 0;
    }

    [[nodiscard]] std::uint8_t filter_mode() const {
        return static_cast<std::uint8_t>(data >> 14u);
    }

    [[nodiscard]] static TextureChunk read(const Sa3Dport::Structs::EndianStackReader& reader,
                                           std::uint32_t address) {
        const std::uint16_t header = reader.read_u16(address);
        TextureChunk result(static_cast<PolyChunkType>(header & 0xFFu) == PolyChunkType::TextureID2);
        result.attributes = static_cast<std::uint8_t>(header >> 8u);
        result.data = reader.read_u16(address + 2u);
        return result;
    }
};

} // namespace Sa3Dport::Mesh::Chunk::PolyChunks
