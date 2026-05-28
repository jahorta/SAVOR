#pragma once

#include "Structs/EndianStackReader.h"

#include <cstdint>

namespace Sa3Dport::File::Structs {

struct MetaWeight {
    static constexpr std::uint32_t StructSize = 12;

    std::uint32_t node_pointer = 0;
    std::uint32_t vertex_index = 0;
    float weight = 0.0f;

    [[nodiscard]] static MetaWeight Read(const Sa3Dport::Structs::EndianStackReader& reader,
                                         std::uint32_t address) {
        return {
            reader.read_u32(address),
            reader.read_u32(address + 4u),
            reader.read_float(address + 8u),
        };
    }

    [[nodiscard]] bool operator==(const MetaWeight&) const = default;
};

} // namespace Sa3Dport::File::Structs
