#pragma once

#include <cstdint>

namespace Sa3Dport::Mesh::Chunk {

enum class VertexChunkType : std::uint8_t {
    Null = 0,
    BlankVec4 = 32,
    NormalVec4 = 33,
    Blank = 34,
    Diffuse = 35,
    UserAttributes = 36,
    Attributes = 37,
    DiffuseSpecular5 = 38,
    DiffuseSpecular4 = 39,
    Intensity = 40,
    Normal = 41,
    NormalDiffuse = 42,
    NormalUserAttributes = 43,
    NormalAttributes = 44,
    NormalDiffuseSpecular5 = 45,
    NormalDiffuseSpecular4 = 46,
    NormalIntensity = 47,
    Normal32 = 48,
    Normal32Diffuse = 49,
    Normal32UserAttributes = 50,
    End = 255,
};

} // namespace Sa3Dport::Mesh::Chunk
