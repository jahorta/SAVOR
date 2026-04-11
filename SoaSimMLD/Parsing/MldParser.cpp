#include "MldParser.h"

#include "../../Compression/Aklz.h"
#include "../Model/IndexEntry.h"
#include "../common/ByteUtils.h"
#include "MldBinaryReader.h"

#include <algorithm>
#include <array>
#include <memory>
#include <sstream>
#include <unordered_map>

namespace soasim::mld::parsing {

namespace {

using soasim::mld::model::EncounterOrTriggerRegion;
using soasim::mld::model::GrndSurface;
using soasim::mld::model::MeshVertex;
using soasim::mld::model::UnknownEntry;
using soasim::mld::model::Vec3;
using soasim::mld::model::WalkSurfaceNode;

constexpr std::uint32_t makeTag(const char a, const char b, const char c, const char d) {
    return static_cast<std::uint32_t>(a) |
        (static_cast<std::uint32_t>(b) << 8) |
        (static_cast<std::uint32_t>(c) << 16) |
        (static_cast<std::uint32_t>(d) << 24);
}

[[nodiscard]] std::string tagToString(const std::uint32_t tag) {
    std::array<char, 5> out{};
    out[0] = static_cast<char>(tag & 0xFFU);
    out[1] = static_cast<char>((tag >> 8) & 0xFFU);
    out[2] = static_cast<char>((tag >> 16) & 0xFFU);
    out[3] = static_cast<char>((tag >> 24) & 0xFFU);
    out[4] = '\0';

    for (std::size_t i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(out[i]);
        if (c < 32U || c > 126U) {
            out[i] = '?';
        }
    }
    return std::string(out.data());
}

[[nodiscard]] Vec3 applyCoordinates(const Vec3& value, const CoordinatePolicy& policy) {
    Vec3 out = value;
    if (policy.swapYZ) {
        std::swap(out.y, out.z);
    }
    if (policy.negateX) {
        out.x = -out.x;
    }
    if (policy.negateY) {
        out.y = -out.y;
    }
    if (policy.negateZ) {
        out.z = -out.z;
    }
    out.x *= policy.uniformScale;
    out.y *= policy.uniformScale;
    out.z *= policy.uniformScale;
    return out;
}

[[nodiscard]] bool readVec3(MldBinaryReader& reader, Vec3& out) {
    const auto x = reader.readF32LE();
    const auto y = reader.readF32LE();
    const auto z = reader.readF32LE();
    if (!x.has_value() || !y.has_value() || !z.has_value()) {
        return false;
    }
    out = Vec3{ *x, *y, *z };
    return true;
}

void addHistogram(std::unordered_map<std::uint32_t, std::size_t>& histogram, const ParseOptions& options, std::uint32_t fxn) {
    if (!options.emitFxnHistogram) {
        return;
    }
    ++histogram[fxn];
}

void parseNjChunkStream(std::span<const std::uint8_t> bytes,
    const std::size_t imageBase,
    std::unordered_map<std::uint32_t, std::size_t>& chunkTypeCounts,
    std::vector<NjcmChunkSummary>& njcmChunks) {
    constexpr std::uint32_t tagNjcm = makeTag('N', 'J', 'C', 'M');
    constexpr std::uint32_t tagNjtl = makeTag('N', 'J', 'T', 'L');
    constexpr std::uint32_t tagPof0 = makeTag('P', 'O', 'F', '0');
    constexpr std::uint32_t tagNmdm = makeTag('N', 'M', 'D', 'M');
    constexpr std::uint32_t tagNcam = makeTag('N', 'C', 'A', 'M');

    struct PendingNjcm {
        std::size_t chunkStart = 0;
        std::size_t chunkDataSize = 0;
        bool chunkSizeLittleEndian = true;
        std::vector<std::uint8_t> data{};
    };
    std::optional<PendingNjcm> pendingNjcm{};

    MldBinaryReader reader(bytes);
    while (reader.remaining() >= 8) {
        const std::size_t relChunkStart = reader.position();
        const auto tag = reader.readU32LE();
        const auto chunkSizeLe = reader.readU32LE();
        if (!tag.has_value() || !chunkSizeLe.has_value()) {
            break;
        }
        ++chunkTypeCounts[*tag];

        const std::size_t dataStart = reader.position();
        std::size_t chunkSize = static_cast<std::size_t>(*chunkSizeLe);
        bool chunkSizeLittleEndian = true;
        std::size_t dataEnd = dataStart + chunkSize;
        if (dataEnd > reader.size()) {
            const std::uint32_t sizeBe = ((*chunkSizeLe & 0x000000FFU) << 24) |
                ((*chunkSizeLe & 0x0000FF00U) << 8) |
                ((*chunkSizeLe & 0x00FF0000U) >> 8) |
                ((*chunkSizeLe & 0xFF000000U) >> 24);
            const std::size_t beEnd = dataStart + static_cast<std::size_t>(sizeBe);
            if (beEnd <= reader.size()) {
                chunkSize = static_cast<std::size_t>(sizeBe);
                dataEnd = beEnd;
                chunkSizeLittleEndian = false;
            }
        }
        if (dataEnd > reader.size()) {
            break;
        }

        const std::size_t absChunkStart = imageBase + relChunkStart;
        if (*tag == tagNjcm) {
            PendingNjcm state{};
            state.chunkStart = absChunkStart;
            state.chunkDataSize = chunkSize;
            state.chunkSizeLittleEndian = chunkSizeLittleEndian;
            state.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(dataStart),
                bytes.begin() + static_cast<std::ptrdiff_t>(dataEnd));
            pendingNjcm = std::move(state);
        } else if (*tag == tagNjtl || *tag == tagPof0 || *tag == tagNmdm || *tag == tagNcam) {
            if (*tag == tagPof0 && pendingNjcm.has_value()) {
                const auto pofSpan = bytes.subspan(dataStart, chunkSize);
                auto fixed = pendingNjcm->data;
                const auto deltas = decodePof0Deltas(pofSpan);
                applyPof0Fixups(fixed, deltas,
                    static_cast<std::uint32_t>(pendingNjcm->chunkStart),
                    true);
                auto summary = analyzeNjcmChunk(std::span<const std::uint8_t>(fixed.data(), fixed.size()),
                    pendingNjcm->chunkStart,
                    pendingNjcm->chunkDataSize,
                    pendingNjcm->chunkSizeLittleEndian,
                    true);
                njcmChunks.push_back(summary);
                pendingNjcm.reset();
            }
        }

        if (!reader.seek(dataEnd)) {
            break;
        }
    }

    if (pendingNjcm.has_value()) {
        auto summary = analyzeNjcmChunk(
            std::span<const std::uint8_t>(pendingNjcm->data.data(), pendingNjcm->data.size()),
            pendingNjcm->chunkStart,
            pendingNjcm->chunkDataSize,
            pendingNjcm->chunkSizeLittleEndian,
            false);
        njcmChunks.push_back(summary);
    }
}

} // namespace

ParseResult MldParser::parse(std::span<const std::uint8_t> mldBytes, const ParseOptions& options) const {
    ParseResult result{};

    std::vector<std::uint8_t> decoded;
    std::span<const std::uint8_t> payload = mldBytes;
    if (soasim::compression::aklz::isAklz(mldBytes)) {
        auto decodedResult = soasim::compression::aklz::decompress(mldBytes);
        if (!decodedResult.ok()) {
            result.diagnostics.push_back(ParseDiagnostic{
                .severity = ParseDiagnostic::Severity::Error,
                .message = "AKLZ decompression failed: " + std::string(soasim::compression::aklz::errorToString(decodedResult.error)),
            });
            return result;
        }

        decoded = std::move(decodedResult.bytes);
        payload = std::span<const std::uint8_t>(decoded.data(), decoded.size());
    }

    if (payload.empty()) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Error,
            .message = "Input buffer is empty.",
        });
        return result;
    }

    std::unordered_map<std::uint32_t, std::size_t> histogram{};
    std::unordered_map<std::uint32_t, std::size_t> chunkTypeCounts{};
    if (payload.size() < 0x14) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Error,
            .message = "MLD header too small.",
        });
        return result;
    }

    const auto nmldCountOpt = common::readU32AtBE(payload, 0x00);
    const auto ptrNmldTableOpt = common::readU32AtBE(payload, 0x04);
    const auto ptrFxnParamsOpt = common::readU32AtBE(payload, 0x08);
    const auto ptrRealDataOpt = common::readU32AtBE(payload, 0x0C);
    const auto ptrTextureTableOpt = common::readU32AtBE(payload, 0x10);
    if (!nmldCountOpt.has_value() || !ptrNmldTableOpt.has_value() || !ptrFxnParamsOpt.has_value() ||
        !ptrRealDataOpt.has_value() || !ptrTextureTableOpt.has_value()) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Error,
            .message = "Failed to read required MLD header pointers.",
        });
        return result;
    }

    constexpr std::size_t entrySize = 0x68;
    const std::size_t nmldCount = static_cast<std::size_t>(*nmldCountOpt);
    const std::size_t entryTableOffset = static_cast<std::size_t>(*ptrNmldTableOpt);
    const std::size_t entryTableEnd = entryTableOffset + (nmldCount * entrySize);
    if (entryTableOffset >= payload.size() || entryTableEnd > payload.size()) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Error,
            .message = "MLD entry table is out of bounds (count=" + std::to_string(nmldCount) +
                ", ptr=" + std::to_string(entryTableOffset) + ").",
        });
        return result;
    }

    result.diagnostics.push_back(ParseDiagnostic{
        .severity = ParseDiagnostic::Severity::Info,
        .message = "Index-based parse: entries=" + std::to_string(nmldCount) +
            ", entryTable=0x" + std::to_string(entryTableOffset) +
            ", fxnParams=0x" + std::to_string(static_cast<std::size_t>(*ptrFxnParamsOpt)) +
            ", realData=0x" + std::to_string(static_cast<std::size_t>(*ptrRealDataOpt)) +
            ", textureTable=0x" + std::to_string(static_cast<std::size_t>(*ptrTextureTableOpt)),
    });

    std::vector<model::IndexEntry> entries{};
    entries.reserve(nmldCount);

    for (std::size_t i = 0; i < nmldCount; ++i) {
        const std::size_t entryOffset = entryTableOffset + (i * entrySize);

        const auto entryOpt = model::parseIndexEntry(payload, i, entryOffset,
            [&](const Vec3& value) {
                return applyCoordinates(value, options.coordinates);
            },
            [&](const std::string& message) {
                result.diagnostics.push_back(ParseDiagnostic{
                    .severity = ParseDiagnostic::Severity::Warning,
                    .message = message,
                });
            });
        if (!entryOpt.has_value()) {
            result.diagnostics.push_back(ParseDiagnostic{
                .severity = ParseDiagnostic::Severity::Warning,
                .message = "Entry " + std::to_string(i) + " malformed or truncated.",
            });
            continue;
        }

        entries.push_back(std::move(*entryOpt));
    }

    for (const auto& entry : entries) {
        addHistogram(histogram, options, entry.tblId);

        if ((entry.tblId & 0xF0000000U) == 0x10000000U) {
            model::CollisionVolume collision{};
            collision.sourceEntryId = entry.entryId;
            collision.transform = entry.transform;
            result.world.collisions.push_back(std::move(collision));
        } else if ((entry.tblId & 0xF0000000U) == 0x20000000U) {
            model::TriggerVolume trigger{};
            trigger.sourceEntryId = entry.entryId;
            trigger.fxn = entry.tblId;
            trigger.transform = entry.transform;
            result.world.triggers.push_back(trigger);
            result.searchWorld.regions.push_back(EncounterOrTriggerRegion{
                .sourceEntryId = entry.entryId,
                .fxn = entry.tblId,
                .transform = entry.transform,
            });
        } else if (options.preserveUnknownEntries) {
            UnknownEntry unknown{};
            unknown.sourceEntryId = entry.entryId;
            unknown.fxn = entry.tblId;
            unknown.transform = entry.transform;
            const std::size_t entryOffset = entryTableOffset + (entry.tableIndex * entrySize);
            unknown.rawPayload.assign(payload.begin() + static_cast<std::ptrdiff_t>(entryOffset),
                payload.begin() + static_cast<std::ptrdiff_t>(entryOffset + entrySize));
            result.world.unknownEntries.push_back(std::move(unknown));
        }

        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Info,
            .message = "Entry " + std::to_string(entry.tableIndex) + ": id=" + std::to_string(entry.entryId) +
                ", tblId=0x" + std::to_string(entry.tblId) +
                ", fxn=\"" + entry.fxnName + "\"" +
                ", groundLinks=" + std::to_string(entry.groundLinks->values.size()) +
                ", params2=" + std::to_string(entry.paramList2->values.size()) +
                ", functionParams=" + std::to_string(entry.functionParameters->values.size()) +
                ", objects=" + std::to_string(entry.objectAddresses->values.size()) +
                ", grounds=" + std::to_string(entry.groundAddresses->values.size()) +
                ", motions=" + std::to_string(entry.motionAddresses->values.size()),
        });

        for (const auto groundAddress : entry.groundAddresses->values) {
            if (groundAddress == 0U) {
                continue;
            }
            GrndSurface surface{};
            surface.id = groundAddress;
            surface.transform = entry.transform;
            surface.linkedGrndIds.reserve(entry.groundLinks->values.size());
            for (const auto link : entry.groundLinks->values) {
                surface.linkedGrndIds.push_back(link);
            }
            result.world.grndSurfaces.push_back(std::move(surface));
        }

        for (const auto objectAddress : entry.objectAddresses->values) {
            if (objectAddress == 0U) {
                continue;
            }
            const std::size_t objectOffset = static_cast<std::size_t>(objectAddress);
            const auto relNjcm = common::readU32AtLE(payload, objectOffset + 0x00);
            const auto objectSizeField = common::readU32AtLE(payload, objectOffset + 0x04);
            const auto relNjtl = common::readU32AtLE(payload, objectOffset + 0x08);
            if (!relNjcm.has_value() || !objectSizeField.has_value() || !relNjtl.has_value()) {
                continue;
            }
            if (*objectSizeField < 16) {
                continue;
            }

            const std::size_t objectPayloadSize = static_cast<std::size_t>(*objectSizeField - 16U);
            const std::size_t startRel = (*relNjtl != 0U) ? static_cast<std::size_t>(*relNjtl) : static_cast<std::size_t>(*relNjcm);
            const std::size_t startAbs = objectOffset + startRel;
            if (startAbs >= payload.size() || startAbs + objectPayloadSize > payload.size()) {
                result.diagnostics.push_back(ParseDiagnostic{
                    .severity = ParseDiagnostic::Severity::Warning,
                    .message = "Object payload out of bounds @ 0x" + std::to_string(objectOffset),
                });
                continue;
            }

            parseNjChunkStream(payload.subspan(startAbs, objectPayloadSize), startAbs, chunkTypeCounts, result.njcmChunks);
        }

        for (const auto groundAddress : entry.groundAddresses->values) {
            if (groundAddress == 0U) {
                continue;
            }
            const std::size_t grndOffset = static_cast<std::size_t>(groundAddress);
            if (grndOffset + 8 > payload.size()) {
                continue;
            }
            if (common::readU32AtLE(payload, grndOffset).value_or(0U) == makeTag('G', 'R', 'N', 'D')) {
                MldBinaryReader chunkReader(payload.subspan(grndOffset + 8));
                const auto grndId = chunkReader.readU32LE();
                const auto vertexCount = chunkReader.readU32LE();
                const auto indexCount = chunkReader.readU32LE();
                if (!grndId.has_value() || !vertexCount.has_value() || !indexCount.has_value()) {
                    continue;
                }
                GrndSurface surface{};
                surface.id = *grndId;
                surface.transform = entry.transform;
                surface.linkedGrndIds.reserve(entry.groundLinks->values.size());
                for (const auto link : entry.groundLinks->values) {
                    surface.linkedGrndIds.push_back(link);
                }

                const std::size_t vtxCount = static_cast<std::size_t>(*vertexCount);
                const std::size_t idxCount = static_cast<std::size_t>(*indexCount);
                surface.mesh.vertices.reserve(vtxCount);
                surface.mesh.indices.reserve(idxCount);
                for (std::size_t vi = 0; vi < vtxCount; ++vi) {
                    Vec3 p{};
                    if (!readVec3(chunkReader, p)) {
                        break;
                    }
                    MeshVertex v{};
                    v.position = applyCoordinates(p, options.coordinates);
                    surface.mesh.vertices.push_back(v);
                }
                for (std::size_t ii = 0; ii < idxCount; ++ii) {
                    const auto idx = chunkReader.readU32LE();
                    if (!idx.has_value()) {
                        break;
                    }
                    surface.mesh.indices.push_back(*idx);
                }
                if (options.coordinates.reverseTriangleWinding && surface.mesh.indices.size() >= 3) {
                    for (std::size_t ii = 0; ii + 2 < surface.mesh.indices.size(); ii += 3) {
                        std::swap(surface.mesh.indices[ii + 1], surface.mesh.indices[ii + 2]);
                    }
                }
                result.world.grndSurfaces.push_back(std::move(surface));
            }
        }

        if (static_cast<std::size_t>(entry.texturesPointer) < payload.size()) {
            ++chunkTypeCounts[makeTag('N', 'J', 'T', 'L')];
        }
    }

    for (const auto& njcm : result.njcmChunks) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Info,
            .message = "NJCM decode summary @ " + std::to_string(njcm.chunkOffset) +
                ": size=" + std::to_string(njcm.chunkDataSize) +
                ", sizeEndian=" + std::string(njcm.chunkSizeLittleEndian ? "LE" : "BE") +
                ", payloadEndian=" + std::string(njcm.payloadLittleEndian ? "LE" : "BE") +
                ", imageBase=" + std::to_string(njcm.imageBase) +
                ", pof0=" + std::string(njcm.usedPof0Fixup ? "yes" : "no") +
                ", objects=" + std::to_string(njcm.objectCount) +
                ", attaches=" + std::to_string(njcm.attachCount) +
                ", vchunks=" + std::to_string(njcm.vertexChunkCount) +
                ", pchunks=" + std::to_string(njcm.polyChunkCount) +
                ", verts=" + std::to_string(njcm.decodedVertexCount) +
                ", triEst=" + std::to_string(njcm.decodedTriangleCount) +
                ", score=" + std::to_string(njcm.score),
        });
    }

    result.searchWorld.surfaces.reserve(result.world.grndSurfaces.size());
    for (const auto& grnd : result.world.grndSurfaces) {
        result.searchWorld.surfaces.push_back(WalkSurfaceNode{
            .grndId = grnd.id,
            .neighborGrndIds = grnd.linkedGrndIds,
            .mesh = grnd.mesh,
        });
    }

    if (options.emitFxnHistogram) {
        result.fxnHistogram.reserve(histogram.size());
        for (const auto& [fxn, count] : histogram) {
            result.fxnHistogram.emplace_back(fxn, count);
        }
        std::sort(result.fxnHistogram.begin(), result.fxnHistogram.end(),
            [](const auto& a, const auto& b) {
                return a.first < b.first;
            });
    }

    result.chunkTypeHistogram.reserve(chunkTypeCounts.size());
    for (const auto& [tag, count] : chunkTypeCounts) {
        result.chunkTypeHistogram.emplace_back(tagToString(tag), count);
    }
    std::sort(result.chunkTypeHistogram.begin(), result.chunkTypeHistogram.end(),
        [](const auto& a, const auto& b) {
            return a.first < b.first;
        });

    result.diagnostics.push_back(ParseDiagnostic{
        .severity = ParseDiagnostic::Severity::Info,
        .message = "Parsed MLD bytes: GRND=" + std::to_string(result.world.grndSurfaces.size()) +
            ", collisions=" + std::to_string(result.world.collisions.size()) +
            ", triggers=" + std::to_string(result.world.triggers.size()) +
            ", unknownEntries=" + std::to_string(result.world.unknownEntries.size()) +
            ", njcmChunks=" + std::to_string(result.njcmChunks.size()) +
            ", chunkTypes=" + std::to_string(result.chunkTypeHistogram.size()),
    });

    if (result.world.grndSurfaces.empty()) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Warning,
            .message = "No GRND records decoded. This likely means container layout assumptions still need refinement.",
        });
    }

    return result;
}

std::string formatParseSummary(const ParseResult& parseResult) {
    std::ostringstream out;
    out << "grndSurfaces=" << parseResult.world.grndSurfaces.size() << '\n';
    out << "collisions=" << parseResult.world.collisions.size() << '\n';
    out << "triggers=" << parseResult.world.triggers.size() << '\n';
    out << "unknownEntries=" << parseResult.world.unknownEntries.size() << '\n';
    out << "searchSurfaces=" << parseResult.searchWorld.surfaces.size() << '\n';
    out << "searchRegions=" << parseResult.searchWorld.regions.size() << '\n';
    out << "njcmChunks=" << parseResult.njcmChunks.size() << '\n';

    if (!parseResult.chunkTypeHistogram.empty()) {
        out << "chunkTypes:" << '\n';
        for (const auto& [tag, count] : parseResult.chunkTypeHistogram) {
            out << "  - " << tag << ": " << count << '\n';
        }
    }

    if (!parseResult.fxnHistogram.empty()) {
        out << "fxnHistogram:" << '\n';
        for (const auto& [fxn, count] : parseResult.fxnHistogram) {
            out << "  - 0x" << std::hex << fxn << std::dec << ": " << count << '\n';
        }
    }

    if (!parseResult.njcmChunks.empty()) {
        out << "njcm:" << '\n';
        for (const auto& chunk : parseResult.njcmChunks) {
            out << "  - offset=" << chunk.chunkOffset
                << " bytes=" << chunk.chunkDataSize
                << " score=" << chunk.score
                << " objects=" << chunk.objectCount
                << " attaches=" << chunk.attachCount
                << " verts=" << chunk.decodedVertexCount
                << " triEst=" << chunk.decodedTriangleCount
                << " pof0=" << (chunk.usedPof0Fixup ? "yes" : "no")
                << '\n';
        }
    }

    if (!parseResult.diagnostics.empty()) {
        out << "diagnostics:" << '\n';
        for (const auto& diagnostic : parseResult.diagnostics) {
            const char* severity = "info";
            switch (diagnostic.severity) {
            case ParseDiagnostic::Severity::Info:
                severity = "info";
                break;
            case ParseDiagnostic::Severity::Warning:
                severity = "warning";
                break;
            case ParseDiagnostic::Severity::Error:
                severity = "error";
                break;
            }
            out << "  - [" << severity << "] " << diagnostic.message << '\n';
        }
    }

    return out.str();
}

} // namespace soasim::mld::parsing
