#include "MldParser.h"

#include "../../Compression/Aklz.h"
#include "MldBinaryReader.h"

#include <algorithm>
#include <array>
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

    MldBinaryReader reader(payload);
    std::unordered_map<std::uint32_t, std::size_t> histogram{};
    std::unordered_map<std::uint32_t, std::size_t> chunkTypeCounts{};
    std::size_t unknownChunkCount = 0;

    constexpr std::uint32_t tagGrnd = makeTag('G', 'R', 'N', 'D');
    constexpr std::uint32_t tagGobj = makeTag('G', 'O', 'B', 'J');
    constexpr std::uint32_t tagNjcm = makeTag('N', 'J', 'C', 'M');
    constexpr std::uint32_t tagNjtl = makeTag('N', 'J', 'T', 'L');
    constexpr std::uint32_t tagPof0 = makeTag('P', 'O', 'F', '0');
    constexpr std::uint32_t tagNmdm = makeTag('N', 'M', 'D', 'M');
    constexpr std::uint32_t tagNcam = makeTag('N', 'C', 'A', 'M');

    while (reader.remaining() >= 8) {
        const std::size_t chunkStart = reader.position();
        const auto tag = reader.readU32LE();
        const auto chunkSize = reader.readU32LE();
        if (!tag.has_value() || !chunkSize.has_value()) {
            break;
        }
        ++chunkTypeCounts[*tag];

        const std::size_t dataStart = reader.position();
        const std::size_t dataEnd = dataStart + static_cast<std::size_t>(*chunkSize);
        if (dataEnd > reader.size()) {
            result.diagnostics.push_back(ParseDiagnostic{
                .severity = ParseDiagnostic::Severity::Error,
                .message = "Chunk size overflow at offset " + std::to_string(chunkStart),
            });
            break;
        }

        if (*tag == tagGrnd) {
            MldBinaryReader chunkReader(payload.subspan(dataStart, *chunkSize));
            const auto grndId = chunkReader.readU32LE();
            const auto vertexCount = chunkReader.readU32LE();
            const auto indexCount = chunkReader.readU32LE();
            if (!grndId.has_value() || !vertexCount.has_value() || !indexCount.has_value()) {
                result.diagnostics.push_back(ParseDiagnostic{
                    .severity = ParseDiagnostic::Severity::Warning,
                    .message = "GRND chunk too small at offset " + std::to_string(chunkStart),
                });
            } else {
                GrndSurface surface{};
                surface.id = *grndId;

                Vec3 position{};
                if (readVec3(chunkReader, position)) {
                    surface.transform.position = applyCoordinates(position, options.coordinates);
                }

                const std::size_t vtxCount = static_cast<std::size_t>(*vertexCount);
                surface.mesh.vertices.reserve(vtxCount);
                for (std::size_t i = 0; i < vtxCount; ++i) {
                    Vec3 p{};
                    if (!readVec3(chunkReader, p)) {
                        result.diagnostics.push_back(ParseDiagnostic{
                            .severity = ParseDiagnostic::Severity::Warning,
                            .message = "GRND vertex parse short-read (id=" + std::to_string(surface.id) + ")",
                        });
                        break;
                    }
                    MeshVertex v{};
                    v.position = applyCoordinates(p, options.coordinates);
                    surface.mesh.vertices.push_back(v);
                }

                const std::size_t idxCount = static_cast<std::size_t>(*indexCount);
                surface.mesh.indices.reserve(idxCount);
                for (std::size_t i = 0; i < idxCount; ++i) {
                    const auto idx = chunkReader.readU32LE();
                    if (!idx.has_value()) {
                        result.diagnostics.push_back(ParseDiagnostic{
                            .severity = ParseDiagnostic::Severity::Warning,
                            .message = "GRND index parse short-read (id=" + std::to_string(surface.id) + ")",
                        });
                        break;
                    }
                    surface.mesh.indices.push_back(*idx);
                }

                if (options.coordinates.reverseTriangleWinding && surface.mesh.indices.size() >= 3) {
                    for (std::size_t i = 0; i + 2 < surface.mesh.indices.size(); i += 3) {
                        std::swap(surface.mesh.indices[i + 1], surface.mesh.indices[i + 2]);
                    }
                }

                result.world.grndSurfaces.push_back(std::move(surface));
            }
        } else if (*tag == tagGobj) {
            MldBinaryReader chunkReader(payload.subspan(dataStart, *chunkSize));
            const auto entryId = chunkReader.readU32LE();
            const auto fxn = chunkReader.readU32LE();
            if (!entryId.has_value() || !fxn.has_value()) {
                result.diagnostics.push_back(ParseDiagnostic{
                    .severity = ParseDiagnostic::Severity::Warning,
                    .message = "Entry chunk too small at offset " + std::to_string(chunkStart),
                });
            } else {
                addHistogram(histogram, options, *fxn);

                Vec3 position{};
                model::Transform transform{};
                if (readVec3(chunkReader, position)) {
                    transform.position = applyCoordinates(position, options.coordinates);
                }
                const auto paramsOffset = chunkReader.readU32LE();
                const auto paramsCount = chunkReader.readU32LE();
                if (paramsOffset.has_value() && paramsCount.has_value() && *paramsCount > 0) {
                    result.diagnostics.push_back(ParseDiagnostic{
                        .severity = ParseDiagnostic::Severity::Info,
                        .message = "GOBJ has parameter list (offset=" + std::to_string(*paramsOffset) +
                            ", count=" + std::to_string(*paramsCount) + ") at offset " + std::to_string(chunkStart) +
                            "; ground-link decode is deferred until MLD entry layout is confirmed.",
                    });
                }

                if ((*fxn & 0xF0000000U) == 0x10000000U) {
                    model::CollisionVolume collision{};
                    collision.sourceEntryId = *entryId;
                    collision.transform = transform;
                    result.world.collisions.push_back(std::move(collision));
                } else if ((*fxn & 0xF0000000U) == 0x20000000U) {
                    model::TriggerVolume trigger{};
                    trigger.sourceEntryId = *entryId;
                    trigger.fxn = *fxn;
                    trigger.transform = transform;
                    result.world.triggers.push_back(trigger);

                    result.searchWorld.regions.push_back(EncounterOrTriggerRegion{
                        .sourceEntryId = *entryId,
                        .fxn = *fxn,
                        .transform = transform,
                    });
                } else if (options.preserveUnknownEntries) {
                    UnknownEntry unknown{};
                    unknown.sourceEntryId = *entryId;
                    unknown.fxn = *fxn;
                    unknown.transform = transform;
                    unknown.rawPayload.assign(payload.begin() + static_cast<std::ptrdiff_t>(dataStart),
                        payload.begin() + static_cast<std::ptrdiff_t>(dataEnd));
                    result.world.unknownEntries.push_back(std::move(unknown));
                }
            }
        } else if (*tag == tagNjcm) {
            result.diagnostics.push_back(ParseDiagnostic{
                .severity = ParseDiagnostic::Severity::Info,
                .message = "NJCM chunk encountered at offset " + std::to_string(chunkStart) +
                    "; decode is deferred until chunk structure is mirrored from SoAMLDs reference.",
            });
        } else if (*tag == tagNjtl || *tag == tagPof0 || *tag == tagNmdm || *tag == tagNcam) {
            // Known Ninja chunk families for MLD content. Keep counted for diagnostics;
            // decode for these tags can be added incrementally.
        } else {
            ++unknownChunkCount;
        }

        if (!reader.seek(dataEnd)) {
            break;
        }
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
            ", unknownChunks=" + std::to_string(unknownChunkCount) +
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

} // namespace soasim::mld::parsing
