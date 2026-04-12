#pragma once

#include "Types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace soasim::mld::model {

struct NjVertexChunkRecord {
    std::size_t offset = 0;
    std::uint8_t type = 0;
    std::uint16_t sizeWords32 = 0;
    std::uint16_t vertexCount = 0;
};

struct NjPolyChunkRecord {
    std::size_t offset = 0;
    std::uint8_t type = 0;
    std::uint16_t sizeWords16 = 0;
    std::size_t estimatedTriangleCount = 0;
    std::vector<std::uint16_t> rawIndexWords{};
};

struct NjSemanticVertex {
    Vec3 position{};
    bool hasPosition = false;
};

struct NjSemanticPolygon {
    std::uint8_t type = 0;
    std::vector<std::uint32_t> indices{};
    std::size_t estimatedTriangleCount = 0;
};

struct NjAttachRecord {
    std::size_t offset = 0;
    std::size_t vertexListOffset = 0;
    std::size_t polyListOffset = 0;
    std::vector<NjVertexChunkRecord> vertexChunks{};
    std::vector<NjPolyChunkRecord> polyChunks{};
    std::vector<NjSemanticVertex> semanticVertices{};
    std::vector<NjSemanticPolygon> semanticPolygons{};
    std::size_t decodedVertexCount = 0;
    std::size_t decodedTriangleCount = 0;
};

struct NjObjectRecord {
    std::size_t offset = 0;
    std::size_t attachOffset = 0;
    std::size_t childOffset = 0;
    std::size_t siblingOffset = 0;
    bool hasAttach = false;
    bool hasChild = false;
    bool hasSibling = false;
};

struct NjcmDecodedChunk {
    std::size_t chunkOffset = 0;
    std::size_t chunkDataSize = 0;
    bool chunkSizeLittleEndian = false;
    bool payloadLittleEndian = false;
    std::uint32_t imageBase = 0;
    bool sawPof0Chunk = false;
    bool usedPof0Fixup = false;
    bool parsedWithHeuristicFallback = false;
    bool parseSucceeded = false;
    std::vector<NjObjectRecord> objects{};
    std::vector<NjAttachRecord> attaches{};
    std::vector<std::string> diagnostics{};
};

} // namespace soasim::mld::model
