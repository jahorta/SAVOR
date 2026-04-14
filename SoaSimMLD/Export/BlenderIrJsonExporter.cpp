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
            out << '{'
                << "\"position\":[" << v.position.x << ',' << v.position.y << ',' << v.position.z << "],"
                << "\"hasPosition\":" << (v.hasPosition ? "true" : "false")
                << '}';
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
                << ",\"materialStateKey\":" << material.materialStateKey
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
                out << ts.corners[cIdx].vertexIndex;
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

        out << ",\"meshIndices\":[";
        for (std::size_t mi = 0; mi < entry.meshIndices.size(); ++mi) {
            if (mi != 0) {
                out << ',';
            }
            out << entry.meshIndices[mi];
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
            << "\"sourceOffset\":" << t.sourceOffset
            << ",\"sourceSize\":" << t.sourceSize
            << ",\"encodedFormat\":";
        writeJsonString(out, t.encodedFormat);
        out
            << ",\"textureName\":";
        writeJsonString(out, t.textureName);
        out << ",\"encodedDataBase64\":";
        writeJsonString(out, toBase64(std::span<const std::uint8_t>(t.encodedData.data(), t.encodedData.size())));
        out << ",\"width\":" << t.width
            << ",\"height\":" << t.height
            << ",\"pixelFormat\":";
        writeJsonString(out, t.pixelFormat);
        out << ",\"pixelDataBase64\":";
        writeJsonString(out, toBase64(std::span<const std::uint8_t>(t.pixelData.data(), t.pixelData.size())));
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
