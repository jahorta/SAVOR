#pragma once

#include "../Model/Types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace soasim::mld::parsing {

struct GrndDecodeResult {
    bool decoded = false;
    model::MeshData mesh{};
    std::size_t gridX = 0;
    std::size_t gridZ = 0;
    std::size_t cellSizeX = 0;
    std::size_t cellSizeZ = 0;
    std::size_t triangleSetCount = 0;
    std::size_t quadCellCount = 0;
    std::size_t referencedTriangleCount = 0;
    std::size_t duplicateReferenceCount = 0;
    std::size_t skippedReferenceCount = 0;
    std::vector<std::string> diagnostics{};
};

class GrndParser {
public:
    [[nodiscard]] GrndDecodeResult decode(std::span<const std::uint8_t> blockBytes,
        std::uint32_t sourceOffset = 0) const;
};

} // namespace soasim::mld::parsing
