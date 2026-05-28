#pragma once

#include <cstdint>

namespace Sa3Dport::Mesh::Chunk {

enum class PolyChunkType : std::uint8_t {
    Null = 0,

    BlendAlpha = 1,
    MipmapDistanceMultiplier = 2,
    SpecularExponent = 3,
    CacheList = 4,
    DrawList = 5,

    TextureID = 8,
    TextureID2 = 9,

    Material_Diffuse = 17,
    Material_Ambient = 18,
    Material_DiffuseAmbient = 19,
    Material_Specular = 20,
    Material_DiffuseSpecular = 21,
    Material_AmbientSpecular = 22,
    Material_DiffuseAmbientSpecular = 23,
    Material_Bump = 24,
    Material_Diffuse2 = 25,
    Material_Ambient2 = 26,
    Material_DiffuseAmbient2 = 27,
    Material_Specular2 = 28,
    Material_DiffuseSpecular2 = 29,
    Material_AmbientSpecular2 = 30,
    Material_DiffuseAmbientSpecular2 = 31,

    Volume_Polygon3 = 56,
    Volume_Polygon4 = 57,
    Volume_Strip = 58,

    Strip_Blank = 64,
    Strip_Tex = 65,
    Strip_HDTex = 66,
    Strip_Normal = 67,
    Strip_TexNormal = 68,
    Strip_HDTexNormal = 69,
    Strip_Color = 70,
    Strip_TexColor = 71,
    Strip_HDTexColor = 72,
    Strip_BlankDouble = 73,
    Strip_TexDouble = 74,
    Strip_HDTexDouble = 75,

    End = 255,
};

} // namespace Sa3Dport::Mesh::Chunk
