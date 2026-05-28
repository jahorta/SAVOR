#pragma once

#include "Structs/Color.h"
#include "Structs/Vector3.h"

#include <cstdint>

namespace Sa3Dport::Mesh::Chunk::Structs {

struct ChunkVertex {
    Sa3Dport::Structs::Vector3 position {};
    Sa3Dport::Structs::Vector3 normal = Sa3Dport::Structs::Vector3::unit_y();
    Sa3Dport::Structs::Color diffuse = Sa3Dport::Structs::Color(0xFF, 0xFF, 0xFF, 0xFF);
    Sa3Dport::Structs::Color specular = Sa3Dport::Structs::Color(0xFF, 0xFF, 0xFF, 0xFF);
    std::uint32_t attributes = 0;

    [[nodiscard]] std::uint16_t index() const {
        return static_cast<std::uint16_t>(attributes & 0xFFFFu);
    }

    void set_index(std::uint16_t value) {
        attributes = (attributes & ~0xFFFFu) | value;
    }

    [[nodiscard]] float weight() const {
        return static_cast<float>(attributes >> 16) / 255.0f;
    }

    [[nodiscard]] bool operator==(const ChunkVertex&) const = default;
};

} // namespace Sa3Dport::Mesh::Chunk::Structs
