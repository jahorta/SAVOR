#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace soasim::mld::parsing {

struct NjcmChunkSummary {
    std::size_t chunkOffset = 0;
    std::size_t chunkDataSize = 0;
    bool chunkSizeLittleEndian = true;
    bool payloadLittleEndian = true;
    std::uint32_t imageBase = 0;
    bool usedPof0Fixup = false;
    std::size_t objectCount = 0;
    std::size_t attachCount = 0;
    std::size_t vertexChunkCount = 0;
    std::size_t polyChunkCount = 0;
    std::size_t decodedVertexCount = 0;
    std::size_t decodedTriangleCount = 0;
    std::size_t score = 0;
};

[[nodiscard]] std::vector<std::size_t> decodePof0Deltas(std::span<const std::uint8_t> pofData);

void applyPof0Fixups(std::vector<std::uint8_t>& target,
    const std::vector<std::size_t>& deltas,
    std::uint32_t imageBase,
    bool littleEndian);

[[nodiscard]] NjcmChunkSummary analyzeNjcmChunk(std::span<const std::uint8_t> njcmData,
    std::size_t chunkOffset,
    std::size_t chunkDataSize,
    bool chunkSizeLittleEndian,
    bool usedPof0Fixup);

} // namespace soasim::mld::parsing
