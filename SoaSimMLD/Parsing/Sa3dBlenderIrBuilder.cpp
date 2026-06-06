#include "Sa3dBlenderIrBuilder.h"

#include "BlenderIrDiagnostics.h"
#include "GobjParser.h"
#include "GvrTextureDecoder.h"

#include "../../Sa3Dport/Sa3Dport.h"
#include "../common/ByteUtils.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace soasim::mld::parsing {
namespace {

using Sa3Dport::Mesh::Buffer::BufferMesh;
using Sa3Dport::Mesh::Converters::buffer_chunk_attach_with_active_poly_chunks;
using Sa3Dport::Mesh::Converters::get_active_poly_chunks;
using Sa3Dport::ObjectData::NodePtr;

[[nodiscard]] std::string hexOffset(const std::uint32_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "0x00000000";
    auto v = value;
    for (int i = 9; i >= 2; --i) {
        result[static_cast<std::size_t>(i)] = digits[v & 0xFU];
        v >>= 4U;
    }
    return result;
}

[[nodiscard]] model::Vec3 toVec3(const Sa3Dport::Structs::Vector3& value) {
    return model::Vec3{ value.x, value.y, value.z };
}

[[nodiscard]] model::Quat toQuat(const Sa3Dport::Structs::Quaternion& value) {
    return model::Quat{ value.x, value.y, value.z, value.w };
}

[[nodiscard]] model::Transform toTransform(const NodePtr& node) {
    model::Transform result{};
    result.position = toVec3(node->position);
    result.rotationRaw = toVec3(node->euler_rotation);
    result.rotation = toQuat(node->quaternion_rotation);
    result.scale = toVec3(node->scale);
    return result;
}

[[nodiscard]] std::uint64_t hashMaterial(const Sa3Dport::Mesh::Buffer::BufferMaterial& material,
    const bool strippified,
    const bool hasColors,
    const bool flatShading) {
    std::uint64_t h = static_cast<std::uint64_t>(material.texture_index);
    auto mix = [&h](const std::uint64_t value) {
        h ^= value + 0x9E3779B97F4A7C15ULL + (h << 6U) + (h >> 2U);
    };

    std::uint64_t attrib = 0U;
    attrib |= static_cast<std::uint64_t>(material.use_texture ? 1U : 0U) << 0U;
    attrib |= static_cast<std::uint64_t>(material.anisotropic_filtering ? 1U : 0U) << 1U;
    attrib |= static_cast<std::uint64_t>(material.clamp_u ? 1U : 0U) << 2U;
    attrib |= static_cast<std::uint64_t>(material.clamp_v ? 1U : 0U) << 3U;
    attrib |= static_cast<std::uint64_t>(material.mirror_u ? 1U : 0U) << 4U;
    attrib |= static_cast<std::uint64_t>(material.mirror_v ? 1U : 0U) << 5U;
    attrib |= static_cast<std::uint64_t>(material.normal_mapping ? 1U : 0U) << 6U;
    attrib |= static_cast<std::uint64_t>(material.no_lighting ? 1U : 0U) << 7U;
    attrib |= static_cast<std::uint64_t>(material.no_ambient ? 1U : 0U) << 8U;
    attrib |= static_cast<std::uint64_t>(material.no_specular ? 1U : 0U) << 9U;
    attrib |= static_cast<std::uint64_t>(material.flat ? 1U : 0U) << 10U;
    attrib |= static_cast<std::uint64_t>(material.use_alpha ? 1U : 0U) << 11U;
    attrib |= static_cast<std::uint64_t>(material.backface_culling ? 1U : 0U) << 12U;
    attrib |= static_cast<std::uint64_t>(material.no_alpha_test ? 1U : 0U) << 13U;
    attrib |= static_cast<std::uint64_t>(material.source_blend_mode & 0x7U) << 16U;
    attrib |= static_cast<std::uint64_t>(material.destination_blend_mode & 0x7U) << 19U;
    attrib |= static_cast<std::uint64_t>(material.texture_filtering & 0x3U) << 22U;
    mix(attrib);
    mix(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(material.mipmap_distance_multiplier)));
    mix(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(material.specular_exponent)));
    mix(static_cast<std::uint64_t>(strippified ? 1U : 0U));
    mix(static_cast<std::uint64_t>(hasColors ? 1U : 0U));
    mix(static_cast<std::uint64_t>(flatShading ? 1U : 0U));
    return h;
}

[[nodiscard]] std::uint32_t packMaterialStateKey(const Sa3Dport::Mesh::Buffer::BufferMaterial& material) {
    std::uint32_t value = 0U;
    value |= static_cast<std::uint32_t>(material.use_texture ? 1U : 0U) << 0U;
    value |= static_cast<std::uint32_t>(material.anisotropic_filtering ? 1U : 0U) << 1U;
    value |= static_cast<std::uint32_t>(material.clamp_u ? 1U : 0U) << 2U;
    value |= static_cast<std::uint32_t>(material.clamp_v ? 1U : 0U) << 3U;
    value |= static_cast<std::uint32_t>(material.mirror_u ? 1U : 0U) << 4U;
    value |= static_cast<std::uint32_t>(material.mirror_v ? 1U : 0U) << 5U;
    value |= static_cast<std::uint32_t>(material.normal_mapping ? 1U : 0U) << 6U;
    value |= static_cast<std::uint32_t>(material.no_lighting ? 1U : 0U) << 7U;
    value |= static_cast<std::uint32_t>(material.no_ambient ? 1U : 0U) << 8U;
    value |= static_cast<std::uint32_t>(material.no_specular ? 1U : 0U) << 9U;
    value |= static_cast<std::uint32_t>(material.flat ? 1U : 0U) << 10U;
    value |= static_cast<std::uint32_t>(material.use_alpha ? 1U : 0U) << 11U;
    value |= static_cast<std::uint32_t>(material.backface_culling ? 1U : 0U) << 12U;
    value |= static_cast<std::uint32_t>(material.no_alpha_test ? 1U : 0U) << 13U;
    value |= static_cast<std::uint32_t>(material.source_blend_mode & 0x7U) << 16U;
    value |= static_cast<std::uint32_t>(material.destination_blend_mode & 0x7U) << 19U;
    value |= static_cast<std::uint32_t>(material.texture_filtering & 0x3U) << 22U;
    return value;
}

[[nodiscard]] std::string readFixedAsciiName(std::span<const std::uint8_t> bytes,
    const std::size_t offset,
    const std::size_t maxLength) {
    std::string out{};
    if (offset >= bytes.size()) {
        return out;
    }
    const auto end = std::min(bytes.size(), offset + maxLength);
    for (std::size_t i = offset; i < end; ++i) {
        const auto ch = bytes[i];
        if (ch == 0U) {
            break;
        }
        if (std::isprint(static_cast<unsigned char>(ch)) == 0) {
            break;
        }
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

[[nodiscard]] std::vector<std::string> parseNjtlTextureNames(const ExtractedNjBlock& block) {
    std::vector<std::string> names{};
    if (block.bytes.size() < 16U) {
        return names;
    }

    std::size_t njtlOffset = 0U;
    auto tag = common::readU32AtBE(block.bytes, njtlOffset).value_or(0U);
    if (tag != 0x4E4A544CU && tag != 0x474A544CU && block.bytes.size() >= 0x10U) {
        const auto wrappedNjtlPointer = common::readU32AtBE(block.bytes, 0x08U).value_or(0U);
        if (wrappedNjtlPointer == 0U || wrappedNjtlPointer >= block.bytes.size()) {
            return names;
        }
        njtlOffset = wrappedNjtlPointer;
        tag = common::readU32AtBE(block.bytes, njtlOffset).value_or(0U);
    }
    if (tag != 0x4E4A544CU && tag != 0x474A544CU) {
        return names;
    }
    const auto blockSize = common::readU32AtBE(block.bytes, njtlOffset + 4U);
    const auto count = common::readU32AtBE(block.bytes, njtlOffset + 12U);
    if (!blockSize.has_value() || !count.has_value() || *count > 4096U) {
        return names;
    }

    constexpr std::size_t blockHeaderSize = 8U;
    constexpr std::size_t textureRecordTableOffset = 8U;
    constexpr std::size_t textureRecordSize = 12U;
    const std::size_t payloadStart = njtlOffset + blockHeaderSize;
    const std::size_t payloadSize = std::min<std::size_t>(*blockSize, block.bytes.size() - payloadStart);
    const auto payload = std::span<const std::uint8_t>(block.bytes.data() + static_cast<std::ptrdiff_t>(payloadStart), payloadSize);
    names.reserve(*count);
    for (std::uint32_t i = 0; i < *count; ++i) {
        const std::size_t recordOffset = textureRecordTableOffset + (static_cast<std::size_t>(i) * textureRecordSize);
        const auto namePointer = common::readU32AtBE(payload, recordOffset);
        if (!namePointer.has_value() || *namePointer >= payload.size()) {
            names.push_back({});
            continue;
        }
        names.push_back(readFixedAsciiName(payload, *namePointer, payload.size() - *namePointer));
    }
    return names;
}

[[nodiscard]] std::string textureNameForLocalIndex(const std::uint32_t textureIndex,
    const std::vector<std::string>& localTextureNames) {
    if (textureIndex == 0xFFFFFFFFu || textureIndex == 0xFFFFu) {
        return {};
    }
    if (textureIndex < localTextureNames.size() && !localTextureNames[textureIndex].empty()) {
        return localTextureNames[textureIndex];
    }
    return "texture_" + std::to_string(textureIndex);
}

[[nodiscard]] std::uint32_t resolveObjectAddress(const ExtractedNjBlock& block, const ParseResult& parseResult) {
    std::vector<std::uint32_t> candidates{};
    for (const auto& entry : parseResult.rawEntries) {
        for (const auto address : entry.objectAddresses) {
            if (address >= block.offset && static_cast<std::uint64_t>(address) < static_cast<std::uint64_t>(block.offset) + block.size) {
                candidates.push_back(address);
            }
        }
    }

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    if (!candidates.empty()) {
        return candidates.front();
    }

    return block.offset;
}

[[nodiscard]] std::span<const std::byte> asByteSpan(const std::vector<std::uint8_t>& bytes) {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(bytes.data()),
        bytes.size());
}

struct ParsedSa3dModel {
    Sa3Dport::File::ModelFile model{};
    std::size_t byteTrim = 0;
};

[[nodiscard]] std::optional<ParsedSa3dModel> tryReadModel(const ExtractedNjBlock& block,
    std::vector<std::string>& diagnostics) {
    if (block.bytes.empty()) {
        diagnostics.push_back("SA3D adapter skipped empty NJ block at " + std::to_string(block.offset) + ".");
        return std::nullopt;
    }

    auto tryRead = [&](const std::size_t trim) -> std::optional<ParsedSa3dModel> {
        if (trim >= block.bytes.size()) {
            return std::nullopt;
        }
        const auto bytes = asByteSpan(block.bytes).subspan(trim);
        try {
            ParsedSa3dModel parsed{};
            parsed.model = Sa3Dport::File::ModelFile::read_from_bytes(bytes);
            parsed.byteTrim = trim;
            return parsed;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };

    if (auto parsed = tryRead(0U); parsed.has_value()) {
        return parsed;
    }

    if (block.kind == ExtractedNjBlock::Kind::Object) {
        constexpr std::size_t kMldObjectHeaderSize = 0x10U;
        if (auto parsed = tryRead(kMldObjectHeaderSize); parsed.has_value()) {
            diagnostics.push_back("SA3D adapter stripped 0x10-byte MLD object wrapper at " + std::to_string(block.offset) + ".");
            return parsed;
        }
    }

    diagnostics.push_back("SA3D adapter could not parse NJ model block at " + std::to_string(block.offset) + ".");
    return std::nullopt;
}

[[nodiscard]] std::uint32_t vertexKey(const Sa3Dport::Mesh::Buffer::BufferVertex& vertex,
    const BufferMesh& mesh) {
    return static_cast<std::uint32_t>(vertex.index) + static_cast<std::uint32_t>(mesh.vertex_write_offset);
}

[[nodiscard]] std::uint32_t cornerKey(const Sa3Dport::Mesh::Buffer::BufferCorner& corner,
    const BufferMesh& mesh) {
    return static_cast<std::uint32_t>(corner.vertex_index) + static_cast<std::uint32_t>(mesh.vertex_read_offset);
}

struct SplitVertexKey {
    std::uint32_t sourceIndex = 0;
    std::uint32_t normalXBits = 0;
    std::uint32_t normalYBits = 0;
    std::uint32_t normalZBits = 0;

    [[nodiscard]] bool operator==(const SplitVertexKey&) const = default;
};

struct SplitVertexKeyHash {
    [[nodiscard]] std::size_t operator()(const SplitVertexKey& key) const noexcept {
        std::size_t h = static_cast<std::size_t>(key.sourceIndex);
        h = (h * 1315423911u) ^ key.normalXBits;
        h = (h * 1315423911u) ^ key.normalYBits;
        h = (h * 1315423911u) ^ key.normalZBits;
        return h;
    }
};

[[nodiscard]] SplitVertexKey makeSplitVertexKey(const std::uint32_t sourceIndex,
    const Sa3Dport::Structs::Vector3& normal) {
    return SplitVertexKey{
        .sourceIndex = sourceIndex,
        .normalXBits = std::bit_cast<std::uint32_t>(normal.x),
        .normalYBits = std::bit_cast<std::uint32_t>(normal.y),
        .normalZBits = std::bit_cast<std::uint32_t>(normal.z),
    };
}

[[nodiscard]] model::BlenderIrVertex toIrVertex(const Sa3Dport::Mesh::Buffer::BufferVertex& sourceVertex,
    const Sa3Dport::Structs::Vector3& normal,
    const bool hasNormal,
    const BufferMesh& bufferMesh) {
    model::BlenderIrVertex vertex{};
    vertex.position = toVec3(sourceVertex.position);
    vertex.normal = toVec3(normal);
    vertex.hasPosition = true;
    vertex.hasNormal = hasNormal;
    if (sourceVertex.weight != 1.0f || bufferMesh.continue_weight) {
        vertex.weights.push_back(model::BlenderIrWeight{
            .boneOrNodeIndex = 0,
            .weight = sourceVertex.weight,
        });
    }
    return vertex;
}

void appendBufferMeshGeometry(const BufferMesh& bufferMesh,
    const std::vector<std::string>& localTextureNames,
    model::BlenderIrMesh& outMesh,
    std::unordered_map<std::uint32_t, std::uint32_t>& vertexIndexByKey) {
    std::unordered_map<std::uint32_t, const Sa3Dport::Mesh::Buffer::BufferVertex*> sourceVertexByKey{};
    sourceVertexByKey.reserve(bufferMesh.vertices.size());
    for (const auto& sourceVertex : bufferMesh.vertices) {
        const auto key = vertexKey(sourceVertex, bufferMesh);
        sourceVertexByKey[key] = &sourceVertex;
        if (vertexIndexByKey.find(key) != vertexIndexByKey.end()) {
            continue;
        }

        auto vertex = toIrVertex(sourceVertex, sourceVertex.normal, bufferMesh.has_normals, bufferMesh);
        vertexIndexByKey[key] = static_cast<std::uint32_t>(outMesh.vertices.size());
        outMesh.vertices.push_back(std::move(vertex));
    }

    if (!bufferMesh.has_corners()) {
        return;
    }

    const auto materialHash = hashMaterial(bufferMesh.material, bufferMesh.strippified, bufferMesh.has_colors, bufferMesh.flat_shading);
    std::size_t materialIndex = outMesh.materials.size();
    for (std::size_t i = 0; i < outMesh.materials.size(); ++i) {
        if (outMesh.materials[i].materialHash == materialHash) {
            materialIndex = i;
            break;
        }
    }
    if (materialIndex == outMesh.materials.size()) {
        model::BlenderIrMaterial material{};
        material.polyType = bufferMesh.strippified ? 5U : 3U;
        material.chunkFlags =
            static_cast<std::uint8_t>((bufferMesh.material.mirror_u ? 0x1U : 0U) |
                                      (bufferMesh.material.mirror_v ? 0x2U : 0U) |
                                      (bufferMesh.material.clamp_u ? 0x4U : 0U) |
                                      (bufferMesh.material.clamp_v ? 0x8U : 0U));
        material.flatShading = bufferMesh.flat_shading;
        material.materialStateKey = packMaterialStateKey(bufferMesh.material);
        material.useTexture = bufferMesh.material.use_texture;
        material.useAlpha = bufferMesh.material.use_alpha;
        material.noAlphaTest = bufferMesh.material.no_alpha_test;
        material.doubleSided = !bufferMesh.material.backface_culling;
        material.clampU = bufferMesh.material.clamp_u;
        material.clampV = bufferMesh.material.clamp_v;
        material.mirrorU = bufferMesh.material.mirror_u;
        material.mirrorV = bufferMesh.material.mirror_v;
        material.normalMapping = bufferMesh.material.normal_mapping;
        material.noLighting = bufferMesh.material.no_lighting;
        material.noAmbient = bufferMesh.material.no_ambient;
        material.noSpecular = bufferMesh.material.no_specular;
        material.anisotropicFiltering = bufferMesh.material.anisotropic_filtering;
        material.textureFiltering = bufferMesh.material.texture_filtering;
        material.sourceAlpha = bufferMesh.material.source_blend_mode;
        material.destinationAlpha = bufferMesh.material.destination_blend_mode;
        material.mipmapDistanceMultiplier = bufferMesh.material.mipmap_distance_multiplier;
        material.textureId = static_cast<std::uint16_t>(std::min<std::uint32_t>(bufferMesh.material.texture_index, 0xFFFFU));
        material.textureName = textureNameForLocalIndex(bufferMesh.material.texture_index, localTextureNames);
        material.materialHash = materialHash;
        outMesh.materials.push_back(std::move(material));
    }

    model::BlenderIrTriangleSet triangleSet{};
    triangleSet.materialIndex = materialIndex;
    triangleSet.polyType = bufferMesh.strippified ? 5U : 3U;
    triangleSet.fromCacheReplay = false;

    const auto corners = bufferMesh.corner_triangle_list();
    std::unordered_map<SplitVertexKey, std::uint32_t, SplitVertexKeyHash> splitVertexIndexByKey{};
    triangleSet.corners.reserve(corners.size());
    for (const auto& sourceCorner : corners) {
        const auto key = cornerKey(sourceCorner, bufferMesh);
        model::BlenderIrCorner corner{};
        if (sourceCorner.has_normal) {
            const auto sourceVertex = sourceVertexByKey.find(key);
            if (sourceVertex != sourceVertexByKey.end()) {
                const auto splitKey = makeSplitVertexKey(key, sourceCorner.normal);
                if (const auto splitFound = splitVertexIndexByKey.find(splitKey); splitFound != splitVertexIndexByKey.end()) {
                    corner.vertexIndex = splitFound->second;
                } else {
                    auto vertex = toIrVertex(*sourceVertex->second, sourceCorner.normal, true, bufferMesh);
                    corner.vertexIndex = static_cast<std::uint32_t>(outMesh.vertices.size());
                    outMesh.vertices.push_back(std::move(vertex));
                    splitVertexIndexByKey.emplace(splitKey, corner.vertexIndex);
                }
            } else if (const auto found = vertexIndexByKey.find(key); found != vertexIndexByKey.end()) {
                corner.vertexIndex = found->second;
            } else {
                corner.vertexIndex = key;
            }
        } else if (const auto found = vertexIndexByKey.find(key); found != vertexIndexByKey.end()) {
            corner.vertexIndex = found->second;
        } else {
            corner.vertexIndex = key;
        }
        corner.u = sourceCorner.texcoord.x;
        corner.v = sourceCorner.texcoord.y;
        corner.hasUv = true;
        corner.colorR = sourceCorner.color.red_f();
        corner.colorG = sourceCorner.color.green_f();
        corner.colorB = sourceCorner.color.blue_f();
        corner.colorA = sourceCorner.color.alpha_f();
        corner.hasColor = bufferMesh.has_colors;
        triangleSet.corners.push_back(corner);
    }

    if (!triangleSet.corners.empty()) {
        outMesh.triangleSets.push_back(std::move(triangleSet));
    }
}

[[nodiscard]] std::optional<std::size_t> appendAttachMesh(const NodePtr& node,
    const std::size_t nodeIndex,
    const std::uint32_t objectAddress,
    const std::size_t sourceChunkOffset,
    const std::optional<Sa3Dport::Mesh::Converters::ActivePolyChunkList>& activePolyChunks,
    const std::vector<std::string>& localTextureNames,
    model::BlenderIrScene& out) {
    const auto chunkAttach = std::dynamic_pointer_cast<Sa3Dport::Mesh::Chunk::ChunkAttach>(node->attach);
    if (!chunkAttach) {
        return std::nullopt;
    }

    const auto bufferMeshes = buffer_chunk_attach_with_active_poly_chunks(*chunkAttach, activePolyChunks);
    if (bufferMeshes.empty()) {
        return std::nullopt;
    }

    model::BlenderIrMesh mesh{};
    mesh.label = "SA3D_obj_" + std::to_string(objectAddress) + "_node_" + std::to_string(nodeIndex) +
        "_attach_" + std::to_string(chunkAttach->source_address);
    mesh.sourceObjectAddress = objectAddress;
    mesh.sourceChunkOffset = sourceChunkOffset;
    mesh.sourceAttachOffset = chunkAttach->source_address;

    std::unordered_map<std::uint32_t, std::uint32_t> vertexIndexByKey{};
    for (const auto& bufferMesh : bufferMeshes) {
        appendBufferMeshGeometry(bufferMesh, localTextureNames, mesh, vertexIndexByKey);
    }

    BlenderIrDiagnostics::finalizeMesh(mesh);
    out.meshes.push_back(std::move(mesh));
    return out.meshes.size() - 1U;
}

[[nodiscard]] std::optional<std::size_t> appendGobjAttachMesh(
    const GobjNode& node,
    const std::size_t nodeIndex,
    const std::uint32_t sourceAddress,
    model::BlenderIrScene& out) {
    model::BlenderIrMesh mesh{};
    mesh.label = "GOBJ_" + hexOffset(sourceAddress) + "_node_" + std::to_string(nodeIndex) +
        "_attach_" + hexOffset(node.sourceAttachOffset);
    mesh.sourceObjectAddress = sourceAddress;
    mesh.sourceChunkOffset = sourceAddress;
    mesh.sourceAttachOffset = node.sourceAttachOffset;

    if (!node.streamMesh.vertices.empty() && !node.streamMesh.indices.empty()) {
        mesh.vertices.reserve(node.streamMesh.vertices.size());
        for (const auto& sourceVertex : node.streamMesh.vertices) {
            model::BlenderIrVertex vertex{};
            vertex.position = sourceVertex.position;
            vertex.normal = sourceVertex.normal;
            vertex.hasPosition = true;
            vertex.hasNormal = true;
            mesh.vertices.push_back(std::move(vertex));
        }

        model::BlenderIrMaterial material{};
        material.polyType = 3U;
        material.useTexture = false;
        material.doubleSided = true;
        material.flatShading = false;
        material.textureId = 0xFFFFU;
        material.textureFiltering = 1U;
        material.materialHash = 0x474F424AULL ^ static_cast<std::uint64_t>(sourceAddress) ^ nodeIndex;
        mesh.materials.push_back(std::move(material));

        model::BlenderIrTriangleSet triangleSet{};
        triangleSet.materialIndex = 0U;
        triangleSet.polyType = 3U;
        triangleSet.sourceChunkOffset = sourceAddress;
        triangleSet.corners.reserve(node.streamMesh.indices.size());
        for (const auto index : node.streamMesh.indices) {
            model::BlenderIrCorner corner{};
            corner.vertexIndex = index;
            triangleSet.corners.push_back(corner);
        }
        mesh.triangleSets.push_back(std::move(triangleSet));
    }

    if (mesh.vertices.empty() || mesh.triangleSets.empty()) {
        return std::nullopt;
    }

    BlenderIrDiagnostics::finalizeMesh(mesh);
    out.meshes.push_back(std::move(mesh));
    return out.meshes.size() - 1U;
}

void appendTextureArchive(const ParseResult& parseResult, model::BlenderIrScene& out) {
    if (!parseResult.textureArchive.has_value()) {
        return;
    }

    for (std::size_t i = 0; i < parseResult.textureArchive->entries.size(); ++i) {
        const auto& tx = parseResult.textureArchive->entries[i];
        const std::uint32_t globalIndex = tx.hasGlobalIndex ? tx.globalIndex : static_cast<std::uint32_t>(i);

        model::BlenderIrTexture outTexture{};
        outTexture.textureId = globalIndex;
        outTexture.hasTextureId = true;
        outTexture.sourceOffset = tx.gvrDataOffset;
        outTexture.sourceSize = tx.gvrDataSize;
        outTexture.encodedFormat = "gvr";
        outTexture.sourceContainer = "gvm";
        outTexture.sourceTextureFormat = tx.sourceFormat;
        outTexture.sourcePaletteFormat = tx.sourcePaletteFormat;
        outTexture.decodeWarnings = tx.diagnostics;
        outTexture.width = tx.width;
        outTexture.height = tx.height;
        outTexture.pixelFormat = "rgba8";
        if (!tx.textureName.empty()) {
            outTexture.textureName = tx.textureName;
        } else {
            outTexture.textureName = "texture_" + std::to_string(globalIndex);
        }

        const auto decoded = decodeGvrToRgba8(tx);
        if (decoded.decoded) {
            outTexture.width = decoded.width;
            outTexture.height = decoded.height;
            outTexture.pixelData = decoded.rgba8;
            outTexture.hasDecodedPixels = !decoded.rgba8.empty();
        } else {
            out.diagnostics.push_back("SA3D adapter texture decode warning: " +
                (decoded.diagnostics.empty() ? std::string("unknown decode failure.") : decoded.diagnostics.front()));
        }

        out.textures.push_back(std::move(outTexture));
    }
}

void appendGrndMeshes(
    const ParseResult& parseResult,
    model::BlenderIrScene& out,
    std::unordered_map<std::uint32_t, std::vector<std::size_t>>& meshIndicesByGroundAddress) {
    for (const auto& grnd : parseResult.world.grndSurfaces) {
        if (grnd.mesh.vertices.empty() || grnd.mesh.indices.empty()) {
            continue;
        }

        model::BlenderIrMesh mesh{};
        mesh.label = "GRND_" + hexOffset(grnd.sourceOffset);
        mesh.sourceObjectAddress = grnd.id;
        mesh.sourceChunkOffset = grnd.sourceOffset;

        mesh.vertices.reserve(grnd.mesh.vertices.size());
        for (const auto& sourceVertex : grnd.mesh.vertices) {
            model::BlenderIrVertex vertex{};
            vertex.position = sourceVertex.position;
            vertex.normal = sourceVertex.normal;
            vertex.hasPosition = true;
            vertex.hasNormal = true;
            mesh.vertices.push_back(std::move(vertex));
        }

        model::BlenderIrMaterial material{};
        material.polyType = 3U;
        material.useTexture = false;
        material.doubleSided = true;
        material.flatShading = false;
        material.textureId = 0xFFFFU;
        material.textureFiltering = 1U;
        material.materialHash = 0x47524E44ULL ^ static_cast<std::uint64_t>(grnd.sourceOffset);
        mesh.materials.push_back(std::move(material));

        model::BlenderIrTriangleSet triangleSet{};
        triangleSet.materialIndex = 0U;
        triangleSet.polyType = 3U;
        triangleSet.sourceChunkOffset = grnd.sourceOffset;
        triangleSet.corners.reserve(grnd.mesh.indices.size());
        for (const auto index : grnd.mesh.indices) {
            model::BlenderIrCorner corner{};
            corner.vertexIndex = index;
            triangleSet.corners.push_back(corner);
        }
        mesh.triangleSets.push_back(std::move(triangleSet));

        BlenderIrDiagnostics::finalizeMesh(mesh);
        out.meshes.push_back(std::move(mesh));
        meshIndicesByGroundAddress[grnd.id].push_back(out.meshes.size() - 1U);
    }
}

void appendGobjTrees(
    const ParseResult& parseResult,
    model::BlenderIrScene& out,
    std::unordered_map<std::uint32_t, std::vector<std::size_t>>& treeIndicesByGroundAddress) {
    GobjParser parser{};
    for (const auto& block : parseResult.extractedSpatialBlocks) {
        if (block.kind != ExtractedMldSpatialBlock::Kind::Gobj) {
            continue;
        }

        auto decoded = parser.decode(block.bytes, block.offset);
        for (const auto& diagnostic : decoded.diagnostics) {
            out.diagnostics.push_back(diagnostic);
        }
        if (!decoded.decoded) {
            continue;
        }

        model::BlenderIrObjectTree tree{};
        tree.label = "GOBJ_" + hexOffset(block.offset);
        tree.sourceObjectAddress = block.offset;
        tree.sourceChunkOffset = block.offset;
        tree.rootNodeIndices = decoded.rootNodeIndices;
        tree.nodes.reserve(decoded.nodes.size());

        for (std::size_t nodeIndex = 0; nodeIndex < decoded.nodes.size(); ++nodeIndex) {
            const auto& sourceNode = decoded.nodes[nodeIndex];
            model::BlenderIrNode irNode{};
            irNode.sourceNodeOffset = sourceNode.sourceNodeOffset;
            irNode.sourceAttachOffset = sourceNode.sourceAttachOffset;
            irNode.hasAttach = sourceNode.sourceAttachOffset != 0U;
            irNode.meshIndex = appendGobjAttachMesh(sourceNode, nodeIndex, block.offset, out);
            irNode.localTransform = sourceNode.transform;
            irNode.parentNodeIndex = sourceNode.parentNodeIndex;
            irNode.childNodeIndices = sourceNode.childNodeIndices;
            tree.nodes.push_back(std::move(irNode));
        }

        out.objectTrees.push_back(std::move(tree));
        treeIndicesByGroundAddress[block.offset].push_back(out.objectTrees.size() - 1U);
    }
}

} // namespace

model::BlenderIrScene Sa3dBlenderIrBuilder::build(const ParseResult& parseResult) const {
    model::BlenderIrScene out{};
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> meshIndicesByObjectAddress{};
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> meshIndicesByGroundAddress{};
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> treeIndicesByGroundAddress{};
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> treeIndicesByObjectAddress{};

    appendGrndMeshes(parseResult, out, meshIndicesByGroundAddress);
    appendGobjTrees(parseResult, out, treeIndicesByGroundAddress);

    for (const auto& block : parseResult.extractedNjBlocks) {
        if (block.kind != ExtractedNjBlock::Kind::Object) {
            continue;
        }

        auto parsed = tryReadModel(block, out.diagnostics);
        if (!parsed.has_value() || !parsed->model.model) {
            continue;
        }

        const auto objectAddress = resolveObjectAddress(block, parseResult);
        const auto localTextureNames = parseNjtlTextureNames(block);
        if (!localTextureNames.empty()) {
            out.diagnostics.push_back("SA3D adapter parsed " + std::to_string(localTextureNames.size()) +
                " NJTL texture name(s) for object " + std::to_string(objectAddress) + ".");
        }
        const auto nodes = parsed->model.model->tree_nodes();
        const auto activePolyChunks = get_active_poly_chunks(nodes);

        model::BlenderIrObjectTree tree{};
        tree.label = "SA3D_obj_" + std::to_string(objectAddress);
        tree.sourceObjectAddress = objectAddress;
        tree.sourceChunkOffset = block.offset + parsed->byteTrim;
        tree.nodes.reserve(nodes.size());

        std::unordered_map<const Sa3Dport::ObjectData::Node*, std::size_t> nodeIndexByPtr{};
        std::vector<std::optional<std::size_t>> meshIndexByNodeIndex(nodes.size());
        for (std::size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
            const auto& node = nodes[nodeIndex];
            nodeIndexByPtr[node.get()] = nodeIndex;

            meshIndexByNodeIndex[nodeIndex] = appendAttachMesh(
                node,
                nodeIndex,
                objectAddress,
                tree.sourceChunkOffset,
                nodeIndex < activePolyChunks.size() ? activePolyChunks[nodeIndex] : std::nullopt,
                localTextureNames,
                out);
            if (meshIndexByNodeIndex[nodeIndex].has_value()) {
                meshIndicesByObjectAddress[objectAddress].push_back(*meshIndexByNodeIndex[nodeIndex]);
            }

            model::BlenderIrNode irNode{};
            irNode.sourceNodeOffset = node->source_address;
            irNode.sourceEvalFlags = static_cast<std::uint32_t>(node->attributes);
            irNode.sourceAttachOffset = node->attach_address;
            irNode.hasAttach = node->attach != nullptr;
            irNode.meshIndex = meshIndexByNodeIndex[nodeIndex];
            irNode.localTransform = toTransform(node);
            tree.nodes.push_back(std::move(irNode));
        }

        for (std::size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
            const auto& node = nodes[nodeIndex];
            if (const auto parent = node->parent()) {
                if (const auto found = nodeIndexByPtr.find(parent.get()); found != nodeIndexByPtr.end()) {
                    tree.nodes[nodeIndex].parentNodeIndex = found->second;
                }
            }

            for (const auto& child : node->direct_children()) {
                if (const auto found = nodeIndexByPtr.find(child.get()); found != nodeIndexByPtr.end()) {
                    tree.nodes[nodeIndex].childNodeIndices.push_back(found->second);
                }
            }

            if (!tree.nodes[nodeIndex].parentNodeIndex.has_value()) {
                tree.rootNodeIndices.push_back(nodeIndex);
            }
        }

        out.objectTrees.push_back(std::move(tree));
        treeIndicesByObjectAddress[objectAddress].push_back(out.objectTrees.size() - 1U);
    }

    out.indexEntries.reserve(parseResult.rawEntries.size());
    for (const auto& entry : parseResult.rawEntries) {
        model::BlenderIrInstance instance{};
        instance.sourceEntryId = entry.sourceEntryId;
        instance.tblId = entry.tblId;
        instance.fxnName = entry.fxnName;
        instance.transform = entry.transform;
        instance.objectAddresses = entry.objectAddresses;
        instance.groundAddresses = entry.groundAddresses;

        for (const auto objectAddress : entry.objectAddresses) {
            if (const auto found = meshIndicesByObjectAddress.find(objectAddress); found != meshIndicesByObjectAddress.end()) {
                instance.meshIndices.insert(instance.meshIndices.end(), found->second.begin(), found->second.end());
            }
            if (const auto found = treeIndicesByObjectAddress.find(objectAddress); found != treeIndicesByObjectAddress.end()) {
                instance.objectTreeIndices.insert(instance.objectTreeIndices.end(), found->second.begin(), found->second.end());
            }
        }
        for (const auto groundAddress : entry.groundAddresses) {
            if (const auto found = meshIndicesByGroundAddress.find(groundAddress); found != meshIndicesByGroundAddress.end()) {
                instance.meshIndices.insert(instance.meshIndices.end(), found->second.begin(), found->second.end());
            }
            if (const auto found = treeIndicesByGroundAddress.find(groundAddress); found != treeIndicesByGroundAddress.end()) {
                instance.objectTreeIndices.insert(instance.objectTreeIndices.end(), found->second.begin(), found->second.end());
            }
        }

        if (instance.objectTreeIndices.empty() && !instance.objectAddresses.empty()) {
            out.diagnostics.push_back("SA3D adapter entry " + std::to_string(entry.sourceEntryId) +
                " references object(s) with no parsed SA3D object tree.");
        }

        out.indexEntries.push_back(std::move(instance));
    }

    appendTextureArchive(parseResult, out);
    out.diagnostics.push_back("SA3D adapter produced " + std::to_string(out.meshes.size()) + " meshes, " +
        std::to_string(out.objectTrees.size()) + " object trees and " +
        std::to_string(out.indexEntries.size()) + " index entries.");
    return out;
}

} // namespace soasim::mld::parsing
