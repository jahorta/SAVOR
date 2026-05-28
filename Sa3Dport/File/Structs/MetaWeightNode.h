#pragma once

#include "File/Structs/MetaWeightVertex.h"
#include "Structs/EndianStackReader.h"

#include <cstdint>
#include <vector>

namespace Sa3Dport::File::Structs {

struct MetaWeightNode {
    std::uint32_t node_pointer = 0;
    std::vector<MetaWeightVertex> vertex_weights;

    [[nodiscard]] static MetaWeightNode Read(const Sa3Dport::Structs::EndianStackReader& reader,
                                             std::uint32_t& address) {
        MetaWeightNode result;
        result.node_pointer = reader.read_u32(address);
        const std::int32_t vertexCount = reader.read_i32(address + 4u);
        address += 8u;

        if (vertexCount <= 0) {
            return result;
        }

        result.vertex_weights.reserve(static_cast<std::size_t>(vertexCount));
        for (std::int32_t i = 0; i < vertexCount; ++i) {
            result.vertex_weights.push_back(MetaWeightVertex::Read(reader, address));
        }

        return result;
    }

    [[nodiscard]] bool operator==(const MetaWeightNode&) const = default;
};

} // namespace Sa3Dport::File::Structs
