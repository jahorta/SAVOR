#pragma once

#include "../Model/Types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace soasim::mld::parsing {

struct GobjNode {
    std::uint32_t sourceNodeOffset = 0;
    std::uint32_t sourceAttachOffset = 0;
    model::Transform transform{};
    std::optional<std::size_t> parentNodeIndex{};
    std::vector<std::size_t> childNodeIndices{};
    model::MeshData streamMesh{};
};

struct GobjDecodeResult {
    bool decoded = false;
    std::uint32_t sourceOffset = 0;
    std::vector<GobjNode> nodes{};
    std::vector<std::size_t> rootNodeIndices{};
    std::vector<std::string> diagnostics{};
};

class GobjParser {
public:
    [[nodiscard]] GobjDecodeResult decode(std::span<const std::uint8_t> blockBytes,
        std::uint32_t sourceOffset = 0) const;
};

} // namespace soasim::mld::parsing
