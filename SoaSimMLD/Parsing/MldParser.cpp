#include "MldParser.h"

#include "../../Compression/Aklz.h"
#include "../Model/IndexEntry.h"
#include "../common/ByteUtils.h"
#include "EntryHandlers.h"
#include "MldBinaryReader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iostream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

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

void addHistogram(std::unordered_map<std::string, std::size_t>& histogram, const ParseOptions& options, const std::string& fxnName) {
    if (!options.emitFxnHistogram) {
        return;
    }
    ++histogram[fxnName];
}

[[nodiscard]] std::string normalizeFxnName(std::string_view fxnName) {
    std::string normalized{};
    normalized.reserve(fxnName.size());
    for (const auto ch : fxnName) {
        if (std::isalnum(static_cast<unsigned char>(ch)) == 0) {
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return normalized;
}

[[nodiscard]] std::string toHex(const std::uint32_t value) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << value;
    return oss.str();
}

[[nodiscard]] std::string formatPolyTypeHistogram(const std::unordered_map<std::uint8_t, std::size_t>& counts) {
    if (counts.empty()) {
        return "{}";
    }
    std::vector<std::pair<std::uint8_t, std::size_t>> ordered(counts.begin(), counts.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    std::ostringstream out;
    out << "{";
    for (std::size_t ii = 0; ii < ordered.size(); ++ii) {
        if (ii != 0) {
            out << ", ";
        }
        out << static_cast<unsigned>(ordered[ii].first) << ":" << ordered[ii].second;
    }
    out << "}";
    return out.str();
}

class CollisionEntryHandler final : public EntryHandler {
public:
    [[nodiscard]] bool canHandle(const std::string_view fxnName) const override {
        const auto normalized = normalizeFxnName(fxnName);
        return normalized == "wall" ||
            normalized == "walluv" ||
            normalized == "hasigo1" ||
            normalized == "hasigo2";
    }

    void parse(const RawEntry& entry, model::WorldModel& out) const override {
        model::CollisionVolume collision{};
        collision.sourceEntryId = entry.sourceEntryId;
        collision.fxnName = std::string(entry.fxnName);
        collision.tblId = entry.tblId;
        collision.transform = entry.transform;
        collision.objectAddresses = entry.objectAddresses;
        out.collisions.push_back(std::move(collision));
    }
};

class TriggerEntryHandler final : public EntryHandler {
public:
    [[nodiscard]] bool canHandle(const std::string_view fxnName) const override {
        const auto normalized = normalizeFxnName(fxnName);
        return normalized == "treasure" ||
            normalized == "goscript" ||
            normalized == "wallmot";
    }

    void parse(const RawEntry& entry, model::WorldModel& out) const override {
        model::TriggerVolume trigger{};
        trigger.sourceEntryId = entry.sourceEntryId;
        trigger.fxnName = std::string(entry.fxnName);
        trigger.tblId = entry.tblId;
        trigger.transform = entry.transform;
        trigger.objectAddresses = entry.objectAddresses;
        out.triggers.push_back(std::move(trigger));
    }
};

void parseNjChunkStream(std::span<const std::uint8_t> bytes,
    const std::size_t imageBase,
    std::unordered_map<std::uint32_t, std::size_t>& chunkTypeCounts,
    std::vector<NjcmChunkSummary>& njcmChunks,
    std::vector<model::NjcmDecodedChunk>& decodedNjcmChunks,
    const ParseOptions& options) {
    constexpr std::uint32_t tagNjcm = makeTag('N', 'J', 'C', 'M');
    constexpr std::uint32_t tagNjtl = makeTag('N', 'J', 'T', 'L');
    constexpr std::uint32_t tagPof0 = makeTag('P', 'O', 'F', '0');
    constexpr std::uint32_t tagNmdm = makeTag('N', 'M', 'D', 'M');
    constexpr std::uint32_t tagNcam = makeTag('N', 'C', 'A', 'M');

    struct PendingNjcm {
        std::size_t chunkStart = 0;
        std::size_t chunkDataSize = 0;
        bool chunkSizeLittleEndian = true;
        bool sawPof0Chunk = false;
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
        if (!options.njcmPolicy.chunkSizeLittleEndian) {
            const std::uint32_t sizeBe = ((*chunkSizeLe & 0x000000FFU) << 24) |
                ((*chunkSizeLe & 0x0000FF00U) << 8) |
                ((*chunkSizeLe & 0x00FF0000U) >> 8) |
                ((*chunkSizeLe & 0xFF000000U) >> 24);
            chunkSize = static_cast<std::size_t>(sizeBe);
            chunkSizeLittleEndian = false;
        }
        std::size_t dataEnd = dataStart + chunkSize;
        if (dataEnd > reader.size() && options.njcmPolicy.allowHeuristicFallback) {
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
            state.sawPof0Chunk = false;
            state.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(dataStart),
                bytes.begin() + static_cast<std::ptrdiff_t>(dataEnd));
            pendingNjcm = std::move(state);
        } else if (*tag == tagNjtl || *tag == tagPof0 || *tag == tagNmdm || *tag == tagNcam) {
            if (*tag == tagPof0 && pendingNjcm.has_value()) {
                pendingNjcm->sawPof0Chunk = true;
            }
            if ((*tag == tagPof0 || *tag == tagNjtl || *tag == tagNmdm || *tag == tagNcam) && pendingNjcm.has_value()) {
                auto decoded = decodeNjcmChunkDeterministic(std::span<const std::uint8_t>(pendingNjcm->data.data(), pendingNjcm->data.size()),
                    pendingNjcm->chunkStart,
                    pendingNjcm->chunkDataSize,
                    pendingNjcm->chunkSizeLittleEndian,
                    pendingNjcm->sawPof0Chunk,
                    options.njcmPolicy);
                auto summary = summarizeDecodedNjcmChunk(decoded);
                njcmChunks.push_back(summary);
                decodedNjcmChunks.push_back(std::move(decoded));
                pendingNjcm.reset();
            }
        }

        if (!reader.seek(dataEnd)) {
            break;
        }
    }

    if (pendingNjcm.has_value()) {
        auto decoded = decodeNjcmChunkDeterministic(std::span<const std::uint8_t>(pendingNjcm->data.data(), pendingNjcm->data.size()),
            pendingNjcm->chunkStart,
            pendingNjcm->chunkDataSize,
            pendingNjcm->chunkSizeLittleEndian,
            pendingNjcm->sawPof0Chunk,
            options.njcmPolicy);
        auto summary = summarizeDecodedNjcmChunk(decoded);
        njcmChunks.push_back(summary);
        decodedNjcmChunks.push_back(std::move(decoded));
    }
}

} // namespace

ParseResult MldParser::parse(std::span<const std::uint8_t> mldBytes, const ParseOptions& options) const {
    std::cout << "[SoaSimMLD] Step 1/5: Starting parse (" << mldBytes.size() << " bytes).\n";
    ParseResult result{};

    std::vector<std::uint8_t> decoded;
    std::span<const std::uint8_t> payload = mldBytes;
    if (soasim::compression::aklz::isAklz(mldBytes)) {
        std::cout << "[SoaSimMLD] Step 2/5: Input is AKLZ-compressed, decompressing...\n";
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
    else {
        std::cout << "[SoaSimMLD] Step 2/5: Input is not AKLZ-compressed.\n";
    }

    if (payload.empty()) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Error,
            .message = "Input buffer is empty.",
        });
        return result;
    }

    std::unordered_map<std::string, std::size_t> histogram{};
    std::unordered_map<std::uint32_t, std::size_t> chunkTypeCounts{};
    if (payload.size() < 0x14) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Error,
            .message = "MLD header too small.",
        });
        return result;
    }

    const auto entryCountOpt = common::readU32AtBE(payload, 0x00);
    const auto ptrIndexOpt = common::readU32AtBE(payload, 0x04);
    const auto ptrFxnParamsOpt = common::readU32AtBE(payload, 0x08);
    const auto ptrRealDataOpt = common::readU32AtBE(payload, 0x0C);
    const auto ptrTextureTableOpt = common::readU32AtBE(payload, 0x10);
    if (!entryCountOpt.has_value() || !ptrIndexOpt.has_value() || !ptrFxnParamsOpt.has_value() ||
        !ptrRealDataOpt.has_value() || !ptrTextureTableOpt.has_value()) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Error,
            .message = "Failed to read required MLD header pointers.",
        });
        return result;
    }

    constexpr std::size_t entrySize = 0x68;
    const std::size_t entryCount = static_cast<std::size_t>(*entryCountOpt);
    const std::size_t entryTableOffset = static_cast<std::size_t>(*ptrIndexOpt);
    const std::size_t entryTableEnd = entryTableOffset + (entryCount * entrySize);
    if (entryTableOffset >= payload.size() || entryTableEnd > payload.size()) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Error,
            .message = "MLD entry table is out of bounds (count=" + std::to_string(entryCount) +
                ", ptr=" + std::to_string(entryTableOffset) + ").",
        });
        return result;
    }

    result.diagnostics.push_back(ParseDiagnostic{
        .severity = ParseDiagnostic::Severity::Info,
        .message = "Index-based parse: entries=" + std::to_string(entryCount) +
            ", entryTable=0x" + std::to_string(entryTableOffset) +
            ", fxnParams=0x" + std::to_string(static_cast<std::size_t>(*ptrFxnParamsOpt)) +
            ", realData=0x" + std::to_string(static_cast<std::size_t>(*ptrRealDataOpt)) +
            ", textureTable=0x" + std::to_string(static_cast<std::size_t>(*ptrTextureTableOpt)),
    });

    std::vector<model::IndexEntry> entries{};
    entries.reserve(entryCount);
    std::cout << "[SoaSimMLD] Step 3/5: Reading index entries (" << entryCount << " total)...\n";

    for (std::size_t i = 0; i < entryCount; ++i) {
        const std::size_t entryOffset = entryTableOffset + (i * entrySize);

        auto entryOpt = model::parseIndexEntry(payload, i, entryOffset,
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

    std::cout << "[SoaSimMLD] Step 4/5: Decoding entry payloads/chunks...\n";

    std::unordered_set<std::uint32_t> uniqueGroundAddresses{};
    std::unordered_set<std::uint32_t> uniqueObjectAddresses{};
    std::unordered_set<std::uint32_t> uniqueMotionAddresses{};
    std::unordered_map<std::uint32_t, const model::IndexEntry*> groundAddressOwners{};
    const std::array<std::unique_ptr<EntryHandler>, 2> handlers{
        std::make_unique<CollisionEntryHandler>(),
        std::make_unique<TriggerEntryHandler>(),
    };

    for (const auto& entry : entries) {
        addHistogram(histogram, options, entry.fxnName);
        const auto normalizedFxnName = normalizeFxnName(entry.fxnName);

        const std::size_t entryOffset = entryTableOffset + (entry.tableIndex * entrySize);
        std::vector<std::uint32_t> objectAddresses{};
        objectAddresses.reserve(entry.objectAddresses->values.size());
        for (const auto objectAddress : entry.objectAddresses->values) {
            if (objectAddress == 0U) {
                continue;
            }
            objectAddresses.push_back(objectAddress);
        }
        RawEntry rawEntry{
            .sourceEntryId = entry.entryId,
            .fxnName = entry.fxnName,
            .tblId = entry.tblId,
            .transform = entry.transform,
            .objectAddresses = objectAddresses,
            .payload = std::span<const std::uint8_t>(payload.data() + static_cast<std::ptrdiff_t>(entryOffset), entrySize),
        };
        result.rawEntries.push_back(ParsedRawEntry{
            .sourceEntryId = entry.entryId,
            .fxnName = entry.fxnName,
            .tblId = entry.tblId,
            .transform = entry.transform,
            .objectAddresses = objectAddresses,
            .payload = std::vector<std::uint8_t>(
                payload.begin() + static_cast<std::ptrdiff_t>(entryOffset),
                payload.begin() + static_cast<std::ptrdiff_t>(entryOffset + entrySize)),
        });

        bool classified = false;
        bool classifiedAsTrigger = false;
        for (const auto& handler : handlers) {
            if (!handler->canHandle(rawEntry.fxnName)) {
                continue;
            }
            handler->parse(rawEntry, result.world);
            classified = true;
            classifiedAsTrigger = normalizedFxnName == "treasure" ||
                normalizedFxnName == "goscript" ||
                normalizedFxnName == "wallmot";
            break;
        }

        if (!classified && options.preserveUnknownEntries) {
            UnknownEntry unknown{};
            unknown.sourceEntryId = entry.entryId;
            unknown.fxnName = entry.fxnName;
            unknown.tblId = entry.tblId;
            unknown.transform = entry.transform;
            unknown.rawPayload.assign(payload.begin() + static_cast<std::ptrdiff_t>(entryOffset),
                payload.begin() + static_cast<std::ptrdiff_t>(entryOffset + entrySize));
            result.world.unknownEntries.push_back(std::move(unknown));
        }

        if (classifiedAsTrigger) {
            result.searchWorld.regions.push_back(EncounterOrTriggerRegion{
                .sourceEntryId = entry.entryId,
                .fxnName = entry.fxnName,
                .tblId = entry.tblId,
                .transform = entry.transform,
            });
        }

        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Info,
            .message = "Entry " + std::to_string(entry.tableIndex) + ": id=" + std::to_string(entry.entryId) +
                ", tblId=0x" + std::to_string(entry.tblId) +
                ", fxn=\"" + entry.fxnName + "\"" +
                ", groundLinks=" + std::to_string(entry.groundLinks->values.size()) +
                ", params2=" + std::to_string(entry.paramList2->values.size()) +
                ", functionParams=" + std::to_string(entry.functionParameters->values.size()) +
                ", objects=" + std::to_string(entry.objectCount) +
                ", grounds=" + std::to_string(entry.groundCount) +
                ", motions=" + std::to_string(entry.motionCount),
        });

        for (const auto objectAddress : entry.objectAddresses->values) {
            if (objectAddress == 0U) {
                continue;
            }
            uniqueObjectAddresses.insert(objectAddress);
        }

        for (const auto groundAddress : entry.groundAddresses->values) {
            if (groundAddress == 0U) {
                continue;
            }
            if (uniqueGroundAddresses.insert(groundAddress).second) {
                groundAddressOwners.emplace(groundAddress, &entry);
            }
        }

        for (const auto motionAddress : entry.motionAddresses->values) {
            if (motionAddress == 0U) {
                continue;
            }
            uniqueMotionAddresses.insert(motionAddress);
        }

        if (static_cast<std::size_t>(entry.texturesPointer) < payload.size()) {
            ++chunkTypeCounts[makeTag('N', 'J', 'T', 'L')];
        }
    }

    for (const auto groundAddress : uniqueGroundAddresses) {
        GrndSurface surface{};
        surface.id = groundAddress;
        if (const auto ownerIt = groundAddressOwners.find(groundAddress); ownerIt != groundAddressOwners.end()) {
            const auto* owner = ownerIt->second;
            surface.transform = owner->transform;
            surface.linkedGrndIds.reserve(owner->groundLinks->values.size());
            for (const auto link : owner->groundLinks->values) {
                surface.linkedGrndIds.push_back(link);
            }
        }
        result.world.grndSurfaces.push_back(std::move(surface));
    }

    for (const auto objectAddress : uniqueObjectAddresses) {
        const std::size_t objectOffset = static_cast<std::size_t>(objectAddress);
        const auto relNjcm = common::readU32AtBE(payload, objectOffset + 0x00);
        const auto objectSizeField = common::readU32AtBE(payload, objectOffset + 0x04);
        const auto relNjtl = common::readU32AtBE(payload, objectOffset + 0x08);
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

        const auto decodedChunkBegin = result.decodedNjcmChunks.size();
        parseNjChunkStream(payload.subspan(startAbs, objectPayloadSize),
            startAbs,
            chunkTypeCounts,
            result.njcmChunks,
            result.decodedNjcmChunks,
            options);
        const auto decodedChunkEnd = result.decodedNjcmChunks.size();
        result.decodedObjectChunkRanges.push_back(DecodedObjectChunkRange{
            .objectAddress = objectAddress,
            .decodedChunkBegin = decodedChunkBegin,
            .decodedChunkEnd = decodedChunkEnd,
        });
    }

    for (const auto groundAddress : uniqueGroundAddresses) {
        const std::size_t grndOffset = static_cast<std::size_t>(groundAddress);
        if (grndOffset + 8 > payload.size()) {
            continue;
        }
        if (common::readU32AtBE(payload, grndOffset).value_or(0U) == makeTag('G', 'R', 'N', 'D')) {
            MldBinaryReader chunkReader(payload.subspan(grndOffset + 8));
            const auto grndId = chunkReader.readU32BE();
            const auto vertexCount = chunkReader.readU32BE();
            const auto indexCount = chunkReader.readU32BE();
            if (!grndId.has_value() || !vertexCount.has_value() || !indexCount.has_value()) {
                continue;
            }
            GrndSurface surface{};
            surface.id = *grndId;
            if (const auto ownerIt = groundAddressOwners.find(groundAddress); ownerIt != groundAddressOwners.end()) {
                const auto* owner = ownerIt->second;
                surface.transform = owner->transform;
                surface.linkedGrndIds.reserve(owner->groundLinks->values.size());
                for (const auto link : owner->groundLinks->values) {
                    surface.linkedGrndIds.push_back(link);
                }
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
                const auto idx = chunkReader.readU32BE();
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

    result.diagnostics.push_back(ParseDiagnostic{
        .severity = ParseDiagnostic::Severity::Info,
        .message = "Unique chunk address counts: grounds=" + std::to_string(uniqueGroundAddresses.size()) +
            ", objects=" + std::to_string(uniqueObjectAddresses.size()) +
            ", motions=" + std::to_string(uniqueMotionAddresses.size()),
    });

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
    for (const auto& decoded : result.decodedNjcmChunks) {
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = decoded.parseSucceeded ? ParseDiagnostic::Severity::Info : ParseDiagnostic::Severity::Warning,
            .message = "NJCM deterministic decode @ " + std::to_string(decoded.chunkOffset) +
                ": parseOk=" + std::string(decoded.parseSucceeded ? "yes" : "no") +
                ", fallback=" + std::string(decoded.parsedWithHeuristicFallback ? "yes" : "no") +
                ", diagnostics=" + std::to_string(decoded.diagnostics.size()),
        });

        std::size_t decodedAttachTriangles = 0;
        std::size_t decodedAttachVertices = 0;
        std::size_t emptySemanticPolyChunks = 0;
        std::size_t nonEmptySemanticPolyChunks = 0;
        std::size_t outOfRangeIndexCount = 0;
        std::size_t totalSemanticIndexCount = 0;
        std::size_t highestSemanticIndex = 0;
        bool sawAnyIndex = false;
        std::unordered_map<std::uint8_t, std::size_t> polyTypeCounts{};
        std::unordered_map<std::uint8_t, std::size_t> polyTypeWithTriangles{};

        for (std::size_t attachIdx = 0; attachIdx < decoded.attaches.size(); ++attachIdx) {
            const auto& attach = decoded.attaches[attachIdx];
            decodedAttachTriangles += attach.decodedTriangleCount;
            decodedAttachVertices += attach.semanticVertices.size();

            for (const auto& poly : attach.semanticPolygons) {
                ++polyTypeCounts[poly.type];
                if (poly.indices.empty()) {
                    ++emptySemanticPolyChunks;
                } else {
                    ++nonEmptySemanticPolyChunks;
                    ++polyTypeWithTriangles[poly.type];
                }

                for (const auto idx : poly.indices) {
                    sawAnyIndex = true;
                    ++totalSemanticIndexCount;
                    highestSemanticIndex = std::max(highestSemanticIndex, static_cast<std::size_t>(idx));
                    if (idx >= attach.semanticVertices.size()) {
                        ++outOfRangeIndexCount;
                    }
                }
            }

            if (attach.decodedTriangleCount == 0 && !attach.polyChunks.empty()) {
                result.diagnostics.push_back(ParseDiagnostic{
                    .severity = ParseDiagnostic::Severity::Warning,
                    .message = "NJCM attach @ " + std::to_string(attach.offset) +
                        " emitted zero triangles despite polyChunks=" + std::to_string(attach.polyChunks.size()) +
                        " and semanticVertices=" + std::to_string(attach.semanticVertices.size()),
                });
            }
        }

        result.diagnostics.push_back(ParseDiagnostic{
            .severity = ParseDiagnostic::Severity::Info,
            .message = "NJCM semantic summary @ " + std::to_string(decoded.chunkOffset) +
                ": attachVerts=" + std::to_string(decodedAttachVertices) +
                ", attachTriEst=" + std::to_string(decodedAttachTriangles) +
                ", polyChunksWithTriangles=" + std::to_string(nonEmptySemanticPolyChunks) +
                ", polyChunksEmpty=" + std::to_string(emptySemanticPolyChunks) +
                ", semanticIndices=" + std::to_string(totalSemanticIndexCount) +
                ", polyTypes=" + formatPolyTypeHistogram(polyTypeCounts) +
                ", polyTypesWithTriangles=" + formatPolyTypeHistogram(polyTypeWithTriangles),
        });

        if (sawAnyIndex) {
            const auto severity = (outOfRangeIndexCount > 0) ? ParseDiagnostic::Severity::Warning : ParseDiagnostic::Severity::Info;
            result.diagnostics.push_back(ParseDiagnostic{
                .severity = severity,
                .message = "NJCM semantic index bounds @ " + std::to_string(decoded.chunkOffset) +
                    ": outOfRangeIndices=" + std::to_string(outOfRangeIndexCount) +
                    ", highestIndex=" + std::to_string(highestSemanticIndex),
            });
        }
    }

    for (const auto& objectRange : result.decodedObjectChunkRanges) {
        std::size_t objectAttachCount = 0;
        std::size_t objectVertices = 0;
        std::size_t objectTriangles = 0;
        std::size_t objectEmptyPolyChunks = 0;
        std::size_t objectNonEmptyPolyChunks = 0;
        std::size_t objectOutOfRangeIndices = 0;
        std::unordered_map<std::uint8_t, std::size_t> objectPolyTypes{};

        for (std::size_t chunkIdx = objectRange.decodedChunkBegin;
            chunkIdx < objectRange.decodedChunkEnd && chunkIdx < result.decodedNjcmChunks.size();
            ++chunkIdx) {
            const auto& decoded = result.decodedNjcmChunks[chunkIdx];
            objectAttachCount += decoded.attaches.size();
            for (const auto& attach : decoded.attaches) {
                objectVertices += attach.semanticVertices.size();
                objectTriangles += attach.decodedTriangleCount;
                for (const auto& poly : attach.semanticPolygons) {
                    ++objectPolyTypes[poly.type];
                    if (poly.indices.empty()) {
                        ++objectEmptyPolyChunks;
                    } else {
                        ++objectNonEmptyPolyChunks;
                    }
                    for (const auto idx : poly.indices) {
                        if (idx >= attach.semanticVertices.size()) {
                            ++objectOutOfRangeIndices;
                        }
                    }
                }
            }
        }

        const auto severity = (objectOutOfRangeIndices > 0 || (objectTriangles == 0 && objectAttachCount > 0))
            ? ParseDiagnostic::Severity::Warning
            : ParseDiagnostic::Severity::Info;
        result.diagnostics.push_back(ParseDiagnostic{
            .severity = severity,
            .message = "NJCM object summary " + toHex(objectRange.objectAddress) +
                ": chunks=" + std::to_string(objectRange.decodedChunkEnd - objectRange.decodedChunkBegin) +
                ", attaches=" + std::to_string(objectAttachCount) +
                ", verts=" + std::to_string(objectVertices) +
                ", triEst=" + std::to_string(objectTriangles) +
                ", polyChunksWithTriangles=" + std::to_string(objectNonEmptyPolyChunks) +
                ", polyChunksEmpty=" + std::to_string(objectEmptyPolyChunks) +
                ", outOfRangeIndices=" + std::to_string(objectOutOfRangeIndices) +
                ", polyTypes=" + formatPolyTypeHistogram(objectPolyTypes),
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

    std::cout << "[SoaSimMLD] Step 5/5: Parse complete. Entries=" << entries.size()
              << ", GRND=" << result.world.grndSurfaces.size()
              << ", NJCM=" << result.njcmChunks.size() << ".\n";

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
            out << "  - " << fxn << ": " << count << '\n';
        }
    }

    if (!parseResult.njcmChunks.empty()) {
        out << "njcm:" << '\n';
        for (const auto& chunk : parseResult.njcmChunks) {
            out << "  - offset=" << chunk.chunkOffset
                << " bytes=" << chunk.chunkDataSize
                << " score=" << chunk.score
                << " payloadEndian=" << (chunk.payloadLittleEndian ? "LE" : "BE")
                << " sizeEndian=" << (chunk.chunkSizeLittleEndian ? "LE" : "BE")
                << " imageBase=" << chunk.imageBase
                << " objects=" << chunk.objectCount
                << " attaches=" << chunk.attachCount
                << " verts=" << chunk.decodedVertexCount
                << " triEst=" << chunk.decodedTriangleCount
                << " pof0=" << (chunk.usedPof0Fixup ? "yes" : "no")
                << '\n';
        }
    }

    if (!parseResult.decodedNjcmChunks.empty()) {
        out << "decodedNjcm:" << '\n';
        for (const auto& decoded : parseResult.decodedNjcmChunks) {
            out << "  - offset=" << decoded.chunkOffset
                << " objects=" << decoded.objects.size()
                << " attaches=" << decoded.attaches.size()
                << " parseOk=" << (decoded.parseSucceeded ? "yes" : "no")
                << " fallback=" << (decoded.parsedWithHeuristicFallback ? "yes" : "no")
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
