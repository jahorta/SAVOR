#pragma once

#include "../Model/NjcmModel.h"

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

struct NjcmParsePolicy {
    bool payloadLittleEndian = false;
    bool chunkSizeLittleEndian = false;
    std::uint32_t imageBase = 0;
    bool applyPof0Fixups = false;
    bool useSaToolsParityPath = false;
    bool allowHeuristicFallback = true;
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
    bool usedPof0Fixup,
    std::span<const std::uint8_t> pof0Data = {});

[[nodiscard]] model::NjcmDecodedChunk decodeNjcmChunkDeterministic(std::span<const std::uint8_t> njcmData,
    std::size_t chunkOffset,
    std::size_t chunkDataSize,
    bool chunkSizeLittleEndian,
    bool sawPof0Chunk,
    const NjcmParsePolicy& policy,
    std::span<const std::uint8_t> pof0Data = {});

[[nodiscard]] NjcmChunkSummary summarizeDecodedNjcmChunk(const model::NjcmDecodedChunk& decodedChunk);

} // namespace soasim::mld::parsing
