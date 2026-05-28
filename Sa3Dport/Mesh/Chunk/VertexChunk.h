#pragma once

#include "Mesh/Chunk/ChunkTypeExtensions.h"
#include "Mesh/Chunk/Structs/ChunkVertex.h"
#include "Mesh/Chunk/VertexChunkType.h"
#include "Mesh/Chunk/WeightStatus.h"
#include "Structs/Color.h"
#include "Structs/ColorIOType.h"
#include "Structs/EndianIOExtensions.h"
#include "Structs/EndianStackReader.h"
#include "Structs/FloatIOType.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace Sa3Dport::Mesh::Chunk {

class VertexChunk {
public:
    VertexChunkType type = VertexChunkType::Null;
    std::uint8_t attributes = 0;
    std::uint16_t index_offset = 0;
    std::vector<Structs::ChunkVertex> vertices;

    [[nodiscard]] WeightStatus weight_status() const {
        return static_cast<WeightStatus>(attributes & 3u);
    }

    [[nodiscard]] bool has_weight() const { return check_has_weights(type); }
    [[nodiscard]] bool has_normals() const { return check_has_normal(type); }
    [[nodiscard]] bool has_diffuse_colors() const { return check_has_diffuse_color(type); }
    [[nodiscard]] bool has_specular_colors() const { return check_has_specular_color(type); }

    [[nodiscard]] static VertexChunk read(const Sa3Dport::Structs::EndianStackReader& reader,
                                          std::uint32_t& address) {
        const std::uint32_t header1 = reader.read_u32(address);
        const auto attribs = static_cast<std::uint8_t>((header1 >> 8u) & 0xFFu);
        const auto type = static_cast<VertexChunkType>(header1 & 0xFFu);
        if (!is_valid(type) || type == VertexChunkType::End || type == VertexChunkType::Null) {
            throw std::runtime_error("invalid vertex chunk type");
        }

        const std::uint32_t header2 = reader.read_u32(address + 4u);
        VertexChunk result;
        result.type = type;
        result.attributes = attribs;
        result.index_offset = static_cast<std::uint16_t>(header2 & 0xFFFFu);
        result.vertices.resize(static_cast<std::uint16_t>(header2 >> 16u));

        address += 8u;
        const std::uint32_t vec4 = check_is_vec4(type) ? 4u : 0u;
        const bool hasNormal = check_has_normal(type);
        const bool normal32 = check_is_normal32(type);

        for (auto& vertex : result.vertices) {
            vertex.position = Sa3Dport::Structs::EndianIOExtensions::read_vector3(reader, address);
            address += vec4;

            if (!hasNormal) {
                vertex.normal = Sa3Dport::Structs::Vector3::unit_y();
            } else if (normal32) {
                constexpr float componentFactor = 1.0f / 65535.0f;
                const std::uint32_t composed = reader.read_u32(address);
                const auto x = static_cast<std::uint16_t>((composed >> 20u) & 0x3FFu);
                const auto y = static_cast<std::uint16_t>((composed >> 10u) & 0x3FFu);
                const auto z = static_cast<std::uint16_t>(composed & 0x3FFu);
                vertex.normal = {
                    (x * componentFactor) - 1.0f,
                    (y * componentFactor) - 1.0f,
                    (z * componentFactor) - 1.0f,
                };
                address += 4u;
            } else {
                vertex.normal = Sa3Dport::Structs::EndianIOExtensions::read_vector3(reader, address);
                address += vec4;
            }

            switch (type) {
            case VertexChunkType::Diffuse:
            case VertexChunkType::NormalDiffuse:
            case VertexChunkType::Normal32Diffuse:
                vertex.diffuse = Sa3Dport::Structs::EndianIOExtensions::read_color(
                    reader, address, Sa3Dport::Structs::ColorIOType::ARGB8_32);
                break;
            case VertexChunkType::DiffuseSpecular5:
            case VertexChunkType::NormalDiffuseSpecular5:
                vertex.diffuse = Sa3Dport::Structs::EndianIOExtensions::read_color(
                    reader, address, Sa3Dport::Structs::ColorIOType::RGB565);
                vertex.specular = Sa3Dport::Structs::EndianIOExtensions::read_color(
                    reader, address, Sa3Dport::Structs::ColorIOType::RGB565);
                break;
            case VertexChunkType::DiffuseSpecular4:
            case VertexChunkType::NormalDiffuseSpecular4:
                vertex.diffuse = Sa3Dport::Structs::EndianIOExtensions::read_color(
                    reader, address, Sa3Dport::Structs::ColorIOType::ARGB4);
                vertex.specular = Sa3Dport::Structs::EndianIOExtensions::read_color(
                    reader, address, Sa3Dport::Structs::ColorIOType::RGB565);
                break;
            case VertexChunkType::Intensity:
            case VertexChunkType::NormalIntensity:
                vertex.diffuse = Sa3Dport::Structs::Color(0, 0, 0);
                vertex.specular = Sa3Dport::Structs::Color(0, 0, 0);
                address += 4u;
                break;
            case VertexChunkType::Attributes:
            case VertexChunkType::UserAttributes:
            case VertexChunkType::NormalAttributes:
            case VertexChunkType::NormalUserAttributes:
            case VertexChunkType::Normal32UserAttributes:
                vertex.attributes = reader.read_u32(address);
                address += 4u;
                break;
            default:
                break;
            }
        }

        return result;
    }

    [[nodiscard]] static std::vector<std::optional<VertexChunk>> read_array(
        const Sa3Dport::Structs::EndianStackReader& reader,
        std::uint32_t address) {
        std::vector<std::optional<VertexChunk>> result;
        auto readType = [&]() {
            return static_cast<VertexChunkType>(reader.read_u32(address) & 0xFFu);
        };

        for (VertexChunkType type = readType(); type != VertexChunkType::End; type = readType()) {
            if (type == VertexChunkType::Null) {
                result.push_back(std::nullopt);
                address += 8u;
                continue;
            }

            result.push_back(read(reader, address));
        }

        return result;
    }
};

} // namespace Sa3Dport::Mesh::Chunk
