#pragma once

#include "Mesh/Chunk/Structs/ChunkCorner.h"
#include "Structs/ColorIOType.h"
#include "Structs/EndianIOExtensions.h"
#include "Structs/EndianStackReader.h"
#include "Structs/FloatIOType.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace Sa3Dport::Mesh::Chunk::Structs {

struct ChunkStrip {
    static constexpr std::uint32_t MaxByteSize = (UINT16_MAX * 2u) - 2u;

    std::vector<ChunkCorner> corners;
    bool reversed = false;

    [[nodiscard]] std::uint32_t size(int texcoordCount,
                                     bool hasNormal,
                                     bool hasColor,
                                     int triangleAttributeCount) const {
        const std::uint32_t structSize = static_cast<std::uint32_t>(
            2u + (texcoordCount * 4u) + (hasNormal ? 12u : 0u) + (hasColor ? 4u : 0u));
        const auto triangleCount = corners.size() > 2u ? corners.size() - 2u : 0u;
        return static_cast<std::uint32_t>(
            2u + (corners.size() * structSize) + (triangleCount * triangleAttributeCount * 2u));
    }

    [[nodiscard]] static ChunkStrip read(const Sa3Dport::Structs::EndianStackReader& reader,
                                         std::uint32_t& address,
                                         int texcoordCount,
                                         bool hdTexcoord,
                                         bool hasNormal,
                                         bool hasColor,
                                         int triangleAttributeCount) {
        constexpr float normalFactor = 1.0f / 32767.0f;
        const std::int16_t header = reader.read_i16(address);
        ChunkStrip result;
        result.reversed = header < 0;
        result.corners.resize(static_cast<std::size_t>(std::abs(header)));
        address += 2u;

        const bool hasUv = texcoordCount > 0;
        const bool hasUv2 = texcoordCount > 1;
        const float uvMultiplier = hdTexcoord ? (1.0f / 1024.0f) : (1.0f / 256.0f);
        const bool flag1 = triangleAttributeCount > 0;
        const bool flag2 = triangleAttributeCount > 1;
        const bool flag3 = triangleAttributeCount > 2;

        for (std::size_t i = 0; i < result.corners.size(); ++i) {
            ChunkCorner corner;
            corner.index = reader.read_u16(address);
            address += 2u;

            if (hasUv) {
                corner.texcoord = Sa3Dport::Structs::EndianIOExtensions::read_vector2(
                    reader, address, Sa3Dport::Structs::FloatIOType::Short) * uvMultiplier;
                if (hasUv2) {
                    corner.texcoord2 = Sa3Dport::Structs::EndianIOExtensions::read_vector2(
                        reader, address, Sa3Dport::Structs::FloatIOType::Short) * uvMultiplier;
                }
            }

            if (hasNormal) {
                corner.normal = Sa3Dport::Structs::EndianIOExtensions::read_vector3(
                    reader, address, Sa3Dport::Structs::FloatIOType::Short) * normalFactor;
            } else if (hasColor) {
                corner.color = Sa3Dport::Structs::EndianIOExtensions::read_color(
                    reader, address, Sa3Dport::Structs::ColorIOType::ARGB8_16);
            }

            if (flag1 && i > 1) {
                corner.attributes1 = reader.read_u16(address);
                address += 2u;
                if (flag2) {
                    corner.attributes2 = reader.read_u16(address);
                    address += 2u;
                    if (flag3) {
                        corner.attributes3 = reader.read_u16(address);
                        address += 2u;
                    }
                }
            }

            result.corners[i] = corner;
        }

        return result;
    }
};

} // namespace Sa3Dport::Mesh::Chunk::Structs
