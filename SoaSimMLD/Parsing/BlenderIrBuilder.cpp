#include "BlenderIrBuilder.h"

#include "BlenderIrDiagnostics.h"

#include <unordered_map>

namespace soasim::mld::parsing {
namespace {

[[nodiscard]] std::uint64_t hashMaterial(const std::uint8_t polyType,
    const std::uint8_t chunkFlags,
    const bool fromCacheReplay,
    const std::uint32_t materialStateKey,
    const std::uint16_t textureId) {
    std::uint64_t h = static_cast<std::uint64_t>(polyType);
    h = (h << 8U) | static_cast<std::uint64_t>(chunkFlags);
    h = (h << 1U) | static_cast<std::uint64_t>(fromCacheReplay ? 1U : 0U);
    h = (h << 32U) | static_cast<std::uint64_t>(materialStateKey);
    h = (h << 16U) | static_cast<std::uint64_t>(textureId);
    return h;
}

void appendTriangleFromIndices(const std::vector<std::uint32_t>& indices,
    const std::size_t a,
    const std::size_t b,
    const std::size_t c,
    model::BlenderIrTriangleSet& outSet) {
    model::BlenderIrCorner ca{};
    ca.vertexIndex = indices[a];
    model::BlenderIrCorner cb{};
    cb.vertexIndex = indices[b];
    model::BlenderIrCorner cc{};
    cc.vertexIndex = indices[c];
    outSet.corners.push_back(ca);
    outSet.corners.push_back(cb);
    outSet.corners.push_back(cc);
}

void appendTrianglesFromPolygon(const model::NjSemanticPolygon& poly, model::BlenderIrTriangleSet& outSet) {
    if (poly.indices.size() < 3) {
        return;
    }

    for (std::size_t ii = 1; ii + 1 < poly.indices.size(); ++ii) {
        appendTriangleFromIndices(poly.indices, 0, ii, ii + 1, outSet);
    }
}

} // namespace

model::BlenderIrScene BlenderIrBuilder::build(const ParseResult& parseResult) const {
    model::BlenderIrScene out{};
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> meshIndicesByObjectAddress{};

    for (const auto& objectRange : parseResult.decodedObjectChunkRanges) {
        for (std::size_t chunkIdx = objectRange.decodedChunkBegin;
            chunkIdx < objectRange.decodedChunkEnd && chunkIdx < parseResult.decodedNjcmChunks.size();
            ++chunkIdx) {
            const auto& chunk = parseResult.decodedNjcmChunks[chunkIdx];

            for (const auto& attach : chunk.attaches) {
                model::BlenderIrMesh mesh{};
                mesh.label = "NJCM_obj_" + std::to_string(objectRange.objectAddress) + "_attach_" + std::to_string(attach.offset);
                mesh.sourceObjectAddress = objectRange.objectAddress;
                mesh.sourceChunkOffset = chunk.chunkOffset;
                mesh.sourceAttachOffset = attach.offset;

                mesh.vertices.reserve(attach.semanticVertices.size());
                for (const auto& semanticVertex : attach.semanticVertices) {
                    model::BlenderIrVertex irVertex{};
                    irVertex.position = semanticVertex.position;
                    irVertex.hasPosition = semanticVertex.hasPosition;
                    mesh.vertices.push_back(irVertex);
                }

                std::unordered_map<std::uint64_t, std::size_t> materialIndexByHash{};

                for (const auto& poly : attach.semanticPolygons) {
                    const auto materialHash = hashMaterial(poly.type,
                        poly.sourceChunkFlags,
                        poly.fromCacheReplay,
                        poly.materialStateKey,
                        poly.textureId);

                    std::size_t materialIndex = 0;
                    if (const auto found = materialIndexByHash.find(materialHash); found != materialIndexByHash.end()) {
                        materialIndex = found->second;
                    } else {
                        materialIndex = mesh.materials.size();
                        materialIndexByHash.emplace(materialHash, materialIndex);

                        model::BlenderIrMaterial material{};
                        material.polyType = poly.type;
                        material.chunkFlags = poly.sourceChunkFlags;
                        material.fromCacheReplay = poly.fromCacheReplay;
                        material.materialStateKey = poly.materialStateKey;
                        material.textureId = poly.textureId;
                        material.materialHash = materialHash;
                        mesh.materials.push_back(material);
                    }

                    model::BlenderIrTriangleSet triangleSet{};
                    triangleSet.materialIndex = materialIndex;
                    triangleSet.polyType = poly.type;
                    triangleSet.sourceChunkOffset = poly.sourceChunkOffset;
                    triangleSet.fromCacheReplay = poly.fromCacheReplay;
                    appendTrianglesFromPolygon(poly, triangleSet);
                    if (!triangleSet.corners.empty()) {
                        mesh.triangleSets.push_back(std::move(triangleSet));
                    }
                }

                BlenderIrDiagnostics::finalizeMesh(mesh);
                out.meshes.push_back(std::move(mesh));
                meshIndicesByObjectAddress[objectRange.objectAddress].push_back(out.meshes.size() - 1U);
            }
        }
    }

    out.indexEntries.reserve(parseResult.rawEntries.size());
    for (const auto& entry : parseResult.rawEntries) {
        model::BlenderIrInstance instance{};
        instance.sourceEntryId = entry.sourceEntryId;
        instance.tblId = entry.tblId;
        instance.fxnName = entry.fxnName;
        instance.transform = entry.transform;
        instance.objectAddresses = entry.objectAddresses;

        for (const auto objectAddress : entry.objectAddresses) {
            if (const auto found = meshIndicesByObjectAddress.find(objectAddress); found != meshIndicesByObjectAddress.end()) {
                instance.meshIndices.insert(instance.meshIndices.end(), found->second.begin(), found->second.end());
            }
        }

        if (instance.meshIndices.empty() && !instance.objectAddresses.empty()) {
            out.diagnostics.push_back("BlenderIrBuilder entry " + std::to_string(entry.sourceEntryId) +
                " references object(s) with no Blender IR mesh output.");
        }

        out.indexEntries.push_back(std::move(instance));
    }

    out.diagnostics.push_back("BlenderIrBuilder produced " + std::to_string(out.meshes.size()) + " meshes and " +
        std::to_string(out.indexEntries.size()) + " index entries.");
    return out;
}

} // namespace soasim::mld::parsing
