#pragma once

#include "File/Structs/MetaWeight.h"
#include "Structs/EndianStackReader.h"

#include <cstdint>
#include <vector>

namespace Sa3Dport::File::Structs {

struct MetaWeightVertex {
    std::uint32_t destination_vertex_index = 0;
    std::vector<MetaWeight> weights;

    [[nodiscard]] static MetaWeightVertex Read(const Sa3Dport::Structs::EndianStackReader& reader,
                                               std::uint32_t& address) {
        MetaWeightVertex result;
        result.destination_vertex_index = reader.read_u32(address);
        const std::int32_t weightCount = reader.read_i32(address + 4u);
        address += 8u;

        if (weightCount <= 0) {
            return result;
        }

        result.weights.reserve(static_cast<std::size_t>(weightCount));
        for (std::int32_t i = 0; i < weightCount; ++i) {
            result.weights.push_back(MetaWeight::Read(reader, address));
            address += MetaWeight::StructSize;
        }

        return result;
    }

    [[nodiscard]] bool operator==(const MetaWeightVertex&) const = default;
};

} // namespace Sa3Dport::File::Structs
