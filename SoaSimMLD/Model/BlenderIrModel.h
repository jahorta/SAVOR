#pragma once

#include "Types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace soasim::mld::model {

struct BlenderIrWeight {
    std::uint32_t boneOrNodeIndex = 0;
    float weight = 0.0f;
};

struct BlenderIrVertex {
    Vec3 position{};
    Vec3 normal{};
    bool hasPosition = false;
    bool hasNormal = false;
    std::vector<BlenderIrWeight> weights{};
};

struct BlenderIrCorner {
    std::uint32_t vertexIndex = 0;
    float u = 0.0f;
    float v = 0.0f;
    bool hasUv = false;
    float colorR = 0.0f;
    float colorG = 0.0f;
    float colorB = 0.0f;
    float colorA = 0.0f;
    bool hasColor = false;
};

struct BlenderIrMaterial {
    std::uint8_t polyType = 0;
    std::uint8_t chunkFlags = 0;
    bool fromCacheReplay = false;
    bool flatShading = false;
    std::uint32_t materialStateKey = 0;
    bool useTexture = true;
    bool useAlpha = false;
    bool noAlphaTest = false;
    bool doubleSided = false;
    bool clampU = false;
    bool clampV = false;
    bool mirrorU = false;
    bool mirrorV = false;
    bool normalMapping = false;
    bool noLighting = false;
    bool noAmbient = false;
    bool noSpecular = false;
    bool anisotropicFiltering = false;
    std::uint8_t textureFiltering = 1;
    std::uint8_t sourceAlpha = 5;
    std::uint8_t destinationAlpha = 6;
    float mipmapDistanceMultiplier = 1.0f;
    std::uint16_t textureId = 0xFFFFU;
    std::string textureName{};
    std::uint64_t materialHash = 0;
};

struct BlenderIrTexture {
    std::uint32_t textureId = 0xFFFFFFFFU;
    bool hasTextureId = false;
    std::string textureName{};
    std::size_t sourceOffset = 0;
    std::size_t sourceSize = 0;
    std::string encodedFormat{}; // e.g. "gvr", "png", "dds"
    std::vector<std::uint8_t> encodedData{};
    std::string sourceContainer{};
    std::string sourceTextureFormat{};
    std::string sourcePaletteFormat{};
    bool hasDecodedPixels = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string pixelFormat{}; // canonical target, e.g. "rgba8"
    std::vector<std::uint8_t> pixelData{};
    std::vector<std::string> decodeWarnings{};
};

struct BlenderIrTriangleSet {
    std::size_t materialIndex = 0;
    std::vector<BlenderIrCorner> corners{}; // packed triplets
    std::uint8_t polyType = 0;
    std::size_t sourceChunkOffset = 0;
    bool fromCacheReplay = false;
};

struct BlenderIrMeshDiagnostics {
    std::size_t degenerateTriangleCount = 0;
    std::size_t outOfRangeIndexCount = 0;
    std::size_t cacheReplayTriangleCount = 0;
};

struct BlenderIrMesh {
    std::string label{};
    std::uint32_t sourceObjectAddress = 0;
    std::size_t sourceChunkOffset = 0;
    std::size_t sourceAttachOffset = 0;
    std::vector<BlenderIrVertex> vertices{};
    std::vector<BlenderIrMaterial> materials{};
    std::vector<BlenderIrTriangleSet> triangleSets{};
    BlenderIrMeshDiagnostics diagnostics{};
};

struct BlenderIrNode {
    std::size_t sourceNodeOffset = 0;
    std::uint32_t sourceEvalFlags = 0;
    std::size_t sourceAttachOffset = 0;
    bool hasAttach = false;
    std::optional<std::size_t> meshIndex{};
    Transform localTransform{};
    std::optional<std::size_t> parentNodeIndex{};
    std::vector<std::size_t> childNodeIndices{};
};

struct BlenderIrObjectTree {
    std::string label{};
    std::uint32_t sourceObjectAddress = 0;
    std::size_t sourceChunkOffset = 0;
    std::vector<BlenderIrNode> nodes{};
    std::vector<std::size_t> rootNodeIndices{};
};

struct BlenderIrInstance {
    std::uint32_t sourceEntryId = 0;
    std::uint32_t tblId = 0;
    std::string fxnName{};
    Transform transform{};
    std::vector<std::uint32_t> objectAddresses{};
    std::vector<std::uint32_t> groundAddresses{};
    std::vector<std::size_t> meshIndices{};
    std::vector<std::size_t> objectTreeIndices{};
};

struct BlenderIrScene {
    std::vector<BlenderIrMesh> meshes{};
    std::vector<BlenderIrObjectTree> objectTrees{};
    std::vector<BlenderIrInstance> indexEntries{};
    std::vector<BlenderIrTexture> textures{};
    std::vector<std::string> diagnostics{};
};

} // namespace soasim::mld::model
