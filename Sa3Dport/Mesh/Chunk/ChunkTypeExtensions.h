#pragma once

#include "Mesh/Chunk/PolyChunkType.h"
#include "Mesh/Chunk/VertexChunkType.h"

#include <stdexcept>

namespace Sa3Dport::Mesh::Chunk {

[[nodiscard]] inline constexpr bool is_valid(VertexChunkType type) {
    const auto value = static_cast<unsigned>(type);
    return value == 0 || (value >= 32 && value <= 50) || value == 255;
}

[[nodiscard]] inline constexpr bool check_is_vec4(VertexChunkType type) {
    return type == VertexChunkType::BlankVec4 || type == VertexChunkType::NormalVec4;
}

[[nodiscard]] inline constexpr bool check_is_normal32(VertexChunkType type) {
    return type == VertexChunkType::Normal32 ||
           type == VertexChunkType::Normal32Diffuse ||
           type == VertexChunkType::Normal32UserAttributes;
}

[[nodiscard]] inline constexpr bool check_has_normal(VertexChunkType type) {
    return type == VertexChunkType::NormalVec4 ||
           type == VertexChunkType::Normal ||
           type == VertexChunkType::NormalDiffuse ||
           type == VertexChunkType::NormalUserAttributes ||
           type == VertexChunkType::NormalAttributes ||
           type == VertexChunkType::NormalDiffuseSpecular5 ||
           type == VertexChunkType::NormalDiffuseSpecular4 ||
           type == VertexChunkType::NormalIntensity ||
           type == VertexChunkType::Normal32 ||
           type == VertexChunkType::Normal32Diffuse ||
           type == VertexChunkType::Normal32UserAttributes;
}

[[nodiscard]] inline constexpr bool check_has_diffuse_color(VertexChunkType type) {
    return type == VertexChunkType::Diffuse ||
           type == VertexChunkType::DiffuseSpecular5 ||
           type == VertexChunkType::DiffuseSpecular4 ||
           type == VertexChunkType::Intensity ||
           type == VertexChunkType::NormalDiffuse ||
           type == VertexChunkType::NormalDiffuseSpecular5 ||
           type == VertexChunkType::NormalDiffuseSpecular4 ||
           type == VertexChunkType::NormalIntensity ||
           type == VertexChunkType::Normal32Diffuse;
}

[[nodiscard]] inline constexpr bool check_has_specular_color(VertexChunkType type) {
    return type == VertexChunkType::DiffuseSpecular5 ||
           type == VertexChunkType::DiffuseSpecular4 ||
           type == VertexChunkType::Intensity ||
           type == VertexChunkType::NormalDiffuseSpecular5 ||
           type == VertexChunkType::NormalDiffuseSpecular4 ||
           type == VertexChunkType::NormalIntensity;
}

[[nodiscard]] inline constexpr bool check_has_weights(VertexChunkType type) {
    return type == VertexChunkType::Attributes || type == VertexChunkType::NormalAttributes;
}

[[nodiscard]] inline unsigned integer_size(VertexChunkType type) {
    switch (type) {
    case VertexChunkType::Blank:
        return 3;
    case VertexChunkType::BlankVec4:
    case VertexChunkType::Diffuse:
    case VertexChunkType::UserAttributes:
    case VertexChunkType::Attributes:
    case VertexChunkType::DiffuseSpecular5:
    case VertexChunkType::DiffuseSpecular4:
    case VertexChunkType::Intensity:
    case VertexChunkType::Normal32:
        return 4;
    case VertexChunkType::Normal32Diffuse:
    case VertexChunkType::Normal32UserAttributes:
        return 5;
    case VertexChunkType::Normal:
        return 6;
    case VertexChunkType::NormalDiffuse:
    case VertexChunkType::NormalUserAttributes:
    case VertexChunkType::NormalAttributes:
    case VertexChunkType::NormalDiffuseSpecular5:
    case VertexChunkType::NormalDiffuseSpecular4:
    case VertexChunkType::NormalIntensity:
        return 7;
    case VertexChunkType::NormalVec4:
        return 8;
    default:
        throw std::invalid_argument("invalid vertex chunk type");
    }
}

[[nodiscard]] inline constexpr bool check_is_strip(PolyChunkType type) {
    const auto value = static_cast<unsigned>(type);
    return value >= 64u && value <= 75u;
}

[[nodiscard]] inline int get_strip_texcoord_count(PolyChunkType type) {
    if (!check_is_strip(type)) {
        throw std::invalid_argument("poly chunk type is not a strip chunk type");
    }

    switch (type) {
    case PolyChunkType::Strip_Tex:
    case PolyChunkType::Strip_HDTex:
    case PolyChunkType::Strip_TexNormal:
    case PolyChunkType::Strip_HDTexNormal:
    case PolyChunkType::Strip_TexColor:
    case PolyChunkType::Strip_HDTexColor:
        return 1;
    case PolyChunkType::Strip_TexDouble:
    case PolyChunkType::Strip_HDTexDouble:
        return 2;
    default:
        return 0;
    }
}

[[nodiscard]] inline bool check_strip_has_hd_texcoords(PolyChunkType type) {
    if (!check_is_strip(type)) {
        throw std::invalid_argument("poly chunk type is not a strip chunk type");
    }

    return type == PolyChunkType::Strip_HDTex ||
           type == PolyChunkType::Strip_HDTexNormal ||
           type == PolyChunkType::Strip_HDTexColor ||
           type == PolyChunkType::Strip_HDTexDouble;
}

[[nodiscard]] inline bool check_strip_has_colors(PolyChunkType type) {
    if (!check_is_strip(type)) {
        throw std::invalid_argument("poly chunk type is not a strip chunk type");
    }

    return type == PolyChunkType::Strip_Color ||
           type == PolyChunkType::Strip_TexColor ||
           type == PolyChunkType::Strip_HDTexColor;
}

[[nodiscard]] inline bool check_strip_has_normals(PolyChunkType type) {
    if (!check_is_strip(type)) {
        throw std::invalid_argument("poly chunk type is not a strip chunk type");
    }

    return type == PolyChunkType::Strip_Normal ||
           type == PolyChunkType::Strip_TexNormal ||
           type == PolyChunkType::Strip_HDTexNormal;
}

} // namespace Sa3Dport::Mesh::Chunk
