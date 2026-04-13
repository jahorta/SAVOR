#include "NJCMDecodeContext.h"

#include <cstring>

namespace soasim::mld::parsing {

std::optional<std::uint16_t> readU16At(std::span<const std::uint8_t> bytes, const std::size_t off, const bool littleEndian) {
    if (off + 2 > bytes.size()) {
        return std::nullopt;
    }
    if (littleEndian) {
        return static_cast<std::uint16_t>(bytes[off]) |
            (static_cast<std::uint16_t>(bytes[off + 1]) << 8);
    }
    return (static_cast<std::uint16_t>(bytes[off]) << 8) |
        static_cast<std::uint16_t>(bytes[off + 1]);
}

std::optional<std::uint32_t> readU32At(std::span<const std::uint8_t> bytes, const std::size_t off, const bool littleEndian) {
    if (off + 4 > bytes.size()) {
        return std::nullopt;
    }
    if (littleEndian) {
        return static_cast<std::uint32_t>(bytes[off]) |
            (static_cast<std::uint32_t>(bytes[off + 1]) << 8) |
            (static_cast<std::uint32_t>(bytes[off + 2]) << 16) |
            (static_cast<std::uint32_t>(bytes[off + 3]) << 24);
    }
    return (static_cast<std::uint32_t>(bytes[off]) << 24) |
        (static_cast<std::uint32_t>(bytes[off + 1]) << 16) |
        (static_cast<std::uint32_t>(bytes[off + 2]) << 8) |
        static_cast<std::uint32_t>(bytes[off + 3]);
}

std::optional<float> readF32At(std::span<const std::uint8_t> bytes, const std::size_t off, const bool littleEndian) {
    const auto u = readU32At(bytes, off, littleEndian);
    if (!u.has_value()) {
        return std::nullopt;
    }
    float out = 0.0f;
    const auto raw = *u;
    std::memcpy(&out, &raw, sizeof(float));
    return out;
}

std::optional<std::size_t> resolvePointer(const std::uint32_t rawPtr, const std::uint32_t imageBase, const std::size_t payloadSize) {
    if (rawPtr == 0) {
        return std::nullopt;
    }
    if (rawPtr < imageBase) {
        return std::nullopt;
    }
    const std::size_t off = static_cast<std::size_t>(rawPtr - imageBase);
    if (off >= payloadSize) {
        return std::nullopt;
    }
    return off;
}

std::size_t vertexWordsPerVertexByType(const std::uint8_t type) {
    switch (type) {
    case 32U: // Vertex_VertexSH
        return 4;
    case 33U: // Vertex_VertexNormalSH
        return 8;
    case 34U: // Vertex_Vertex
        return 3;
    case 35U: // Vertex_VertexDiffuse8
    case 36U: // Vertex_VertexUserFlags
    case 37U: // Vertex_VertexNinjaFlags
    case 38U: // Vertex_VertexDiffuseSpecular5
    case 39U: // Vertex_VertexDiffuseSpecular4
        return 4;
    case 40U: // Vertex_VertexDiffuseSpecular16
    case 41U: // Vertex_VertexNormal
    case 42U: // Vertex_VertexNormalDiffuse8
    case 43U: // Vertex_VertexNormalUserFlags
    case 44U: // Vertex_VertexNormalNinjaFlags
    case 45U: // Vertex_VertexNormalDiffuseSpecular5
    case 46U: // Vertex_VertexNormalDiffuseSpecular4
    case 47U: // Vertex_VertexNormalDiffuseSpecular16
        return 7;
    case 48U: // Vertex_VertexNormalX
        return 4;
    case 49U: // Vertex_VertexNormalXDiffuse8
    case 50U: // Vertex_VertexNormalXUserFlags
        return 5;
    default:
        return 0;
    }
}

} // namespace soasim::mld::parsing
