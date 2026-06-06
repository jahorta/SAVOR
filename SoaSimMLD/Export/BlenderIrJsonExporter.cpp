#include "BlenderIrJsonExporter.h"

#include <cstdint>
#include <span>
#include <sstream>

namespace soasim::mld::exporting {
namespace {

void writeJsonString(std::ostringstream& out, const std::string& value) {
    out << '"';
    for (const auto ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default: out << ch; break;
        }
    }
    out << '"';
}

void writeVec3(std::ostringstream& out, const model::Vec3& v) {
    out << '[' << v.x << ',' << v.y << ',' << v.z << ']';
}

void writeQuat(std::ostringstream& out, const model::Quat& q) {
    out << '[' << q.x << ',' << q.y << ',' << q.z << ',' << q.w << ']';
}

void writeTransform(std::ostringstream& out, const model::Transform& tx) {
    out << "{\"position\":";
    writeVec3(out, tx.position);
    out << ",\"rotationRaw\":";
    writeVec3(out, tx.rotationRaw);
    out << ",\"rotation\":";
    writeQuat(out, tx.rotation);
    out << ",\"scale\":";
    writeVec3(out, tx.scale);
    out << '}';
}

[[nodiscard]] std::string toBase64(std::span<const std::uint8_t> data) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out{};
    out.reserve(((data.size() + 2U) / 3U) * 4U);

    std::size_t i = 0;
    while (i + 2U < data.size()) {
        const std::uint32_t chunk = (static_cast<std::uint32_t>(data[i]) << 16U) |
            (static_cast<std::uint32_t>(data[i + 1U]) << 8U) |
            static_cast<std::uint32_t>(data[i + 2U]);
        out.push_back(kTable[(chunk >> 18U) & 0x3FU]);
        out.push_back(kTable[(chunk >> 12U) & 0x3FU]);
        out.push_back(kTable[(chunk >> 6U) & 0x3FU]);
        out.push_back(kTable[chunk & 0x3FU]);
        i += 3U;
    }

    const auto remaining = data.size() - i;
    if (remaining == 1U) {
        const std::uint32_t chunk = static_cast<std::uint32_t>(data[i]) << 16U;
        out.push_back(kTable[(chunk >> 18U) & 0x3FU]);
        out.push_back(kTable[(chunk >> 12U) & 0x3FU]);
        out.push_back('=');
        out.push_back('=');
    } else if (remaining == 2U) {
        const std::uint32_t chunk = (static_cast<std::uint32_t>(data[i]) << 16U) |
            (static_cast<std::uint32_t>(data[i + 1U]) << 8U);
        out.push_back(kTable[(chunk >> 18U) & 0x3FU]);
        out.push_back(kTable[(chunk >> 12U) & 0x3FU]);
        out.push_back(kTable[(chunk >> 6U) & 0x3FU]);
        out.push_back('=');
    }

    return out;
}

} // namespace

std::string BlenderIrJsonExporter::toJson(const model::BlenderIrScene& scene) const {
    std::ostringstream out;
    out << "{\"meshes\":[";

    for (std::size_t meshIdx = 0; meshIdx < scene.meshes.size(); ++meshIdx) {
        if (meshIdx != 0) {
            out << ',';
        }

        const auto& mesh = scene.meshes[meshIdx];
        out << '{';
        out << "\"label\":";
        writeJsonString(out, mesh.label);
        out << ",\"sourceObjectAddress\":" << mesh.sourceObjectAddress;
        out << ",\"sourceChunkOffset\":" << mesh.sourceChunkOffset;
        out << ",\"sourceAttachOffset\":" << mesh.sourceAttachOffset;

        out << ",\"vertices\":[";
        for (std::size_t vIdx = 0; vIdx < mesh.vertices.size(); ++vIdx) {
            if (vIdx != 0) {
                out << ',';
            }
            const auto& v = mesh.vertices[vIdx];
            out << '{';
            out << "\"position\":[" << v.position.x << ',' << v.position.y << ',' << v.position.z << ']';
            out << ",\"hasPosition\":" << (v.hasPosition ? "true" : "false");
            out << ",\"normal\":[" << v.normal.x << ',' << v.normal.y << ',' << v.normal.z << ']';
            out << ",\"hasNormal\":" << (v.hasNormal ? "true" : "false");
            out << ",\"weights\":[";
            for (std::size_t wi = 0; wi < v.weights.size(); ++wi) {
                if (wi != 0) {
                    out << ',';
                }
                out << "{\"boneOrNodeIndex\":" << v.weights[wi].boneOrNodeIndex
                    << ",\"weight\":" << v.weights[wi].weight << '}';
            }
            out << "]}";
        }
        out << ']';

        out << ",\"materials\":[";
        for (std::size_t matIdx = 0; matIdx < mesh.materials.size(); ++matIdx) {
            if (matIdx != 0) {
                out << ',';
            }
            const auto& material = mesh.materials[matIdx];
            out << '{'
                << "\"polyType\":" << static_cast<unsigned>(material.polyType)
                << ",\"chunkFlags\":" << static_cast<unsigned>(material.chunkFlags)
                << ",\"fromCacheReplay\":" << (material.fromCacheReplay ? "true" : "false")
                << ",\"flatShading\":" << (material.flatShading ? "true" : "false")
                << ",\"materialStateKey\":" << material.materialStateKey
                << ",\"useTexture\":" << (material.useTexture ? "true" : "false")
                << ",\"useAlpha\":" << (material.useAlpha ? "true" : "false")
                << ",\"noAlphaTest\":" << (material.noAlphaTest ? "true" : "false")
                << ",\"doubleSided\":" << (material.doubleSided ? "true" : "false")
                << ",\"clampU\":" << (material.clampU ? "true" : "false")
                << ",\"clampV\":" << (material.clampV ? "true" : "false")
                << ",\"mirrorU\":" << (material.mirrorU ? "true" : "false")
                << ",\"mirrorV\":" << (material.mirrorV ? "true" : "false")
                << ",\"normalMapping\":" << (material.normalMapping ? "true" : "false")
                << ",\"noLighting\":" << (material.noLighting ? "true" : "false")
                << ",\"noAmbient\":" << (material.noAmbient ? "true" : "false")
                << ",\"noSpecular\":" << (material.noSpecular ? "true" : "false")
                << ",\"anisotropicFiltering\":" << (material.anisotropicFiltering ? "true" : "false")
                << ",\"textureFiltering\":" << static_cast<unsigned>(material.textureFiltering)
                << ",\"sourceAlpha\":" << static_cast<unsigned>(material.sourceAlpha)
                << ",\"destinationAlpha\":" << static_cast<unsigned>(material.destinationAlpha)
                << ",\"mipmapDistanceMultiplier\":" << material.mipmapDistanceMultiplier
                << ",\"textureId\":" << material.textureId
                << ",\"textureName\":";
            writeJsonString(out, material.textureName);
            out
                << ",\"materialHash\":" << material.materialHash
                << '}';
        }
        out << ']';

        out << ",\"triangleSets\":[";
        for (std::size_t tsIdx = 0; tsIdx < mesh.triangleSets.size(); ++tsIdx) {
            if (tsIdx != 0) {
                out << ',';
            }
            const auto& ts = mesh.triangleSets[tsIdx];
            out << '{'
                << "\"materialIndex\":" << ts.materialIndex
                << ",\"polyType\":" << static_cast<unsigned>(ts.polyType)
                << ",\"sourceChunkOffset\":" << ts.sourceChunkOffset
                << ",\"fromCacheReplay\":" << (ts.fromCacheReplay ? "true" : "false")
                << ",\"corners\":[";
            for (std::size_t cIdx = 0; cIdx < ts.corners.size(); ++cIdx) {
                if (cIdx != 0) {
                    out << ',';
                }
                const auto& corner = ts.corners[cIdx];
                out << '{'
                    << "\"vertexIndex\":" << corner.vertexIndex
                    << ",\"u\":" << corner.u
                    << ",\"v\":" << corner.v
                    << ",\"hasUv\":" << (corner.hasUv ? "true" : "false")
                    << ",\"color\":[" << corner.colorR << ',' << corner.colorG << ',' << corner.colorB << ',' << corner.colorA << ']'
                    << ",\"hasColor\":" << (corner.hasColor ? "true" : "false")
                    << '}';
            }
            out << "]}";
        }
        out << ']';

        out << ",\"diagnostics\":{"
            << "\"degenerateTriangleCount\":" << mesh.diagnostics.degenerateTriangleCount << ','
            << "\"outOfRangeIndexCount\":" << mesh.diagnostics.outOfRangeIndexCount << ','
            << "\"cacheReplayTriangleCount\":" << mesh.diagnostics.cacheReplayTriangleCount
            << "}";

        out << '}';
    }

    out << "],\"objectTrees\":[";
    for (std::size_t treeIdx = 0; treeIdx < scene.objectTrees.size(); ++treeIdx) {
        if (treeIdx != 0) {
            out << ',';
        }
        const auto& tree = scene.objectTrees[treeIdx];
        out << '{';
        out << "\"label\":";
        writeJsonString(out, tree.label);
        out << ",\"sourceObjectAddress\":" << tree.sourceObjectAddress;
        out << ",\"sourceChunkOffset\":" << tree.sourceChunkOffset;
        out << ",\"rootNodeIndices\":[";
        for (std::size_t ri = 0; ri < tree.rootNodeIndices.size(); ++ri) {
            if (ri != 0) {
                out << ',';
            }
            out << tree.rootNodeIndices[ri];
        }
        out << ']';

        out << ",\"nodes\":[";
        for (std::size_t nodeIdx = 0; nodeIdx < tree.nodes.size(); ++nodeIdx) {
            if (nodeIdx != 0) {
                out << ',';
            }
            const auto& node = tree.nodes[nodeIdx];
            out << '{'
                << "\"sourceNodeOffset\":" << node.sourceNodeOffset
                << ",\"sourceEvalFlags\":" << node.sourceEvalFlags
                << ",\"sourceAttachOffset\":" << node.sourceAttachOffset
                << ",\"hasAttach\":" << (node.hasAttach ? "true" : "false")
                << ",\"meshIndex\":";
            if (node.meshIndex.has_value()) {
                out << *node.meshIndex;
            } else {
                out << "null";
            }
            out << ",\"localTransform\":";
            writeTransform(out, node.localTransform);
            out << ",\"parentNodeIndex\":";
            if (node.parentNodeIndex.has_value()) {
                out << *node.parentNodeIndex;
            } else {
                out << "null";
            }
            out << ",\"childNodeIndices\":[";
            for (std::size_t ci = 0; ci < node.childNodeIndices.size(); ++ci) {
                if (ci != 0) {
                    out << ',';
                }
                out << node.childNodeIndices[ci];
            }
            out << "]}";
        }
        out << "]}";
    }

    out << "],\"indexEntries\":[";
    for (std::size_t idx = 0; idx < scene.indexEntries.size(); ++idx) {
        if (idx != 0) {
            out << ',';
        }

        const auto& entry = scene.indexEntries[idx];
        out << '{';
        out << "\"sourceEntryId\":" << entry.sourceEntryId;
        out << ",\"tblId\":" << entry.tblId;
        out << ",\"fxnName\":";
        writeJsonString(out, entry.fxnName);
        out << ",\"transform\":";
        writeTransform(out, entry.transform);

        out << ",\"objectAddresses\":[";
        for (std::size_t oi = 0; oi < entry.objectAddresses.size(); ++oi) {
            if (oi != 0) {
                out << ',';
            }
            out << entry.objectAddresses[oi];
        }
        out << ']';

        out << ",\"groundAddresses\":[";
        for (std::size_t gi = 0; gi < entry.groundAddresses.size(); ++gi) {
            if (gi != 0) {
                out << ',';
            }
            out << entry.groundAddresses[gi];
        }
        out << ']';

        out << ",\"meshIndices\":[";
        for (std::size_t mi = 0; mi < entry.meshIndices.size(); ++mi) {
            if (mi != 0) {
                out << ',';
            }
            out << entry.meshIndices[mi];
        }
        out << ']';
        out << ",\"objectTreeIndices\":[";
        for (std::size_t oi = 0; oi < entry.objectTreeIndices.size(); ++oi) {
            if (oi != 0) {
                out << ',';
            }
            out << entry.objectTreeIndices[oi];
        }
        out << ']';

        out << '}';
    }

    out << "],\"textures\":[";
    for (std::size_t ti = 0; ti < scene.textures.size(); ++ti) {
        if (ti != 0) {
            out << ',';
        }
        const auto& t = scene.textures[ti];
        out << '{'
            << "\"textureId\":";
        if (t.hasTextureId) {
            out << t.textureId;
        } else {
            out << "null";
        }
        out
            << ",\"hasTextureId\":" << (t.hasTextureId ? "true" : "false")
            << ",\"sourceOffset\":" << t.sourceOffset
            << ",\"sourceSize\":" << t.sourceSize
            << ",\"encodedFormat\":";
        writeJsonString(out, t.encodedFormat);
        out
            << ",\"textureName\":";
        writeJsonString(out, t.textureName);
        out << ",\"sourceContainer\":";
        writeJsonString(out, t.sourceContainer);
        out << ",\"sourceTextureFormat\":";
        writeJsonString(out, t.sourceTextureFormat);
        out << ",\"sourcePaletteFormat\":";
        writeJsonString(out, t.sourcePaletteFormat);
        out << ",\"hasDecodedPixels\":" << (t.hasDecodedPixels ? "true" : "false");
        out << ",\"encodedDataBase64\":";
        writeJsonString(out, toBase64(std::span<const std::uint8_t>(t.encodedData.data(), t.encodedData.size())));
        out << ",\"width\":" << t.width
            << ",\"height\":" << t.height
            << ",\"pixelFormat\":";
        writeJsonString(out, t.pixelFormat);
        out << ",\"pixelDataBase64\":";
        writeJsonString(out, toBase64(std::span<const std::uint8_t>(t.pixelData.data(), t.pixelData.size())));
        out << ",\"decodeWarnings\":[";
        for (std::size_t wi = 0; wi < t.decodeWarnings.size(); ++wi) {
            if (wi != 0) {
                out << ',';
            }
            writeJsonString(out, t.decodeWarnings[wi]);
        }
        out << ']';
        out
            << '}';
    }

    out << "],\"diagnostics\":[";
    for (std::size_t ii = 0; ii < scene.diagnostics.size(); ++ii) {
        if (ii != 0) {
            out << ',';
        }
        writeJsonString(out, scene.diagnostics[ii]);
    }
    out << "]}";

    return out.str();
}

} // namespace soasim::mld::exporting
