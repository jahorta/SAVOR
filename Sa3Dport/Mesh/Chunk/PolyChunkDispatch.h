#pragma once

#include "Mesh/Chunk/PolyChunk.h"
#include "Mesh/Chunk/PolyChunks/BitsChunk.h"
#include "Mesh/Chunk/PolyChunks/MaterialBumpChunk.h"
#include "Mesh/Chunk/PolyChunks/MaterialChunk.h"
#include "Mesh/Chunk/PolyChunks/StripChunk.h"
#include "Mesh/Chunk/PolyChunks/TextureChunk.h"
#include "Mesh/Chunk/PolyChunks/VolumeChunk.h"

#include <stdexcept>

namespace Sa3Dport::Mesh::Chunk {

[[nodiscard]] inline bool is_valid(PolyChunkType type) {
    const auto value = static_cast<unsigned>(type);
    return value == 0u ||
           (value >= 1u && value <= 5u) ||
           (value >= 8u && value <= 9u) ||
           (value >= 17u && value <= 31u) ||
           (value >= 56u && value <= 58u) ||
           (value >= 64u && value <= 75u) ||
           value == 255u;
}

[[nodiscard]] inline PolyChunkPtr PolyChunk::read(const Sa3Dport::Structs::EndianStackReader& reader,
                                                  std::uint32_t& address) {
    const std::uint32_t chunkAddress = address;
    const std::uint16_t header = reader.read_u16(address);
    const auto type = static_cast<PolyChunkType>(header & 0xFFu);
    const auto attribs = static_cast<std::uint8_t>(header >> 8u);

    if (!is_valid(type) || type == PolyChunkType::End || type == PolyChunkType::Null) {
        throw std::runtime_error("invalid poly chunk type");
    }

    PolyChunkPtr chunk;
    switch (type) {
    case PolyChunkType::BlendAlpha:
    case PolyChunkType::MipmapDistanceMultiplier:
    case PolyChunkType::SpecularExponent:
    case PolyChunkType::CacheList:
    case PolyChunkType::DrawList:
        chunk = std::make_shared<PolyChunks::BitsChunk>(type);
        address += chunk->byte_size();
        break;
    case PolyChunkType::TextureID:
    case PolyChunkType::TextureID2:
        chunk = std::make_shared<PolyChunks::TextureChunk>(PolyChunks::TextureChunk::read(reader, address));
        address += chunk->byte_size();
        break;
    case PolyChunkType::Material_Diffuse:
    case PolyChunkType::Material_Ambient:
    case PolyChunkType::Material_DiffuseAmbient:
    case PolyChunkType::Material_Specular:
    case PolyChunkType::Material_DiffuseSpecular:
    case PolyChunkType::Material_AmbientSpecular:
    case PolyChunkType::Material_DiffuseAmbientSpecular:
    case PolyChunkType::Material_Diffuse2:
    case PolyChunkType::Material_Ambient2:
    case PolyChunkType::Material_DiffuseAmbient2:
    case PolyChunkType::Material_Specular2:
    case PolyChunkType::Material_DiffuseSpecular2:
    case PolyChunkType::Material_AmbientSpecular2:
    case PolyChunkType::Material_DiffuseAmbientSpecular2:
        chunk = std::make_shared<PolyChunks::MaterialChunk>(PolyChunks::MaterialChunk::read(reader, address));
        break;
    case PolyChunkType::Material_Bump:
        chunk = std::make_shared<PolyChunks::MaterialBumpChunk>(PolyChunks::MaterialBumpChunk::read(reader, address));
        address += chunk->byte_size();
        break;
    case PolyChunkType::Strip_Blank:
    case PolyChunkType::Strip_Tex:
    case PolyChunkType::Strip_HDTex:
    case PolyChunkType::Strip_Normal:
    case PolyChunkType::Strip_TexNormal:
    case PolyChunkType::Strip_HDTexNormal:
    case PolyChunkType::Strip_Color:
    case PolyChunkType::Strip_TexColor:
    case PolyChunkType::Strip_HDTexColor:
    case PolyChunkType::Strip_BlankDouble:
    case PolyChunkType::Strip_TexDouble:
    case PolyChunkType::Strip_HDTexDouble:
        chunk = std::make_shared<PolyChunks::StripChunk>(PolyChunks::StripChunk::read(reader, address));
        break;
    case PolyChunkType::Volume_Polygon3:
    case PolyChunkType::Volume_Polygon4:
    case PolyChunkType::Volume_Strip:
        throw std::runtime_error("volume chunks not implemented for this slice");
    default:
        throw std::runtime_error("invalid poly chunk type");
    }

    if (chunkAddress == address) {
        throw std::runtime_error("poly chunk reader did not advance");
    }

    chunk->attributes = attribs;
    return chunk;
}

[[nodiscard]] inline std::vector<std::optional<PolyChunkPtr>> PolyChunk::read_array(
    const Sa3Dport::Structs::EndianStackReader& reader,
    std::uint32_t address) {
    std::vector<std::optional<PolyChunkPtr>> result;
    auto readType = [&]() {
        return static_cast<PolyChunkType>(reader.read_u16(address) & 0xFFu);
    };

    for (PolyChunkType type = readType(); type != PolyChunkType::End; type = readType()) {
        if (type == PolyChunkType::Null) {
            result.push_back(std::nullopt);
            address += 2u;
            continue;
        }

        result.push_back(read(reader, address));
    }

    return result;
}

} // namespace Sa3Dport::Mesh::Chunk
