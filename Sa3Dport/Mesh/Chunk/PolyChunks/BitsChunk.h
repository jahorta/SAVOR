#pragma once

#include "Mesh/Chunk/PolyChunk.h"

#include <cstdint>

namespace Sa3Dport::Mesh::Chunk::PolyChunks {

class BitsChunk : public PolyChunk {
public:
    explicit BitsChunk(PolyChunkType chunkType) : PolyChunk(chunkType) {}

    [[nodiscard]] std::uint32_t byte_size() const override {
        return 2u;
    }

    [[nodiscard]] std::uint8_t source_alpha() const {
        return static_cast<std::uint8_t>((attributes >> 3u) & 7u);
    }

    [[nodiscard]] std::uint8_t destination_alpha() const {
        return static_cast<std::uint8_t>(attributes & 7u);
    }

    [[nodiscard]] float mipmap_distance_multiplier() const {
        return static_cast<float>(attributes & 0xFu) * 0.25f;
    }

    [[nodiscard]] std::uint8_t specular_exponent() const {
        return static_cast<std::uint8_t>(attributes & 0x1Fu);
    }

    [[nodiscard]] std::uint8_t list() const {
        return attributes;
    }
};

} // namespace Sa3Dport::Mesh::Chunk::PolyChunks
