#pragma once

#include "Structs/Color.h"
#include "Structs/Vector2.h"
#include "Structs/Vector3.h"

#include <cstdint>

namespace Sa3Dport::Mesh::Chunk::Structs {

struct ChunkCorner {
    std::uint16_t index = 0;
    Sa3Dport::Structs::Vector2 texcoord {};
    Sa3Dport::Structs::Vector2 texcoord2 {};
    Sa3Dport::Structs::Vector3 normal = Sa3Dport::Structs::Vector3::unit_y();
    Sa3Dport::Structs::Color color = Sa3Dport::Structs::Color(0xFF, 0xFF, 0xFF, 0xFF);
    std::uint16_t attributes1 = 0;
    std::uint16_t attributes2 = 0;
    std::uint16_t attributes3 = 0;

    [[nodiscard]] bool operator==(const ChunkCorner&) const = default;
};

} // namespace Sa3Dport::Mesh::Chunk::Structs
