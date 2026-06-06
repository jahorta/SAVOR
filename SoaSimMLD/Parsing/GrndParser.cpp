#include "GrndParser.h"

#include "../common/ByteUtils.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace soasim::mld::parsing {
namespace {

constexpr std::uint32_t kGrndTag = 0x47524E44U; // "GRND"

struct TriangleSet {
    std::size_t headerOffset = 0;
    std::size_t vertexBlockOffset = 0;
    std::size_t triangleStreamOffset = 0;
    std::size_t streamEntryCount = 0;
    std::uint32_t declaredTriangleCount = 0;
};

struct StreamEntry {
    std::uint16_t floatIndex = 0;
    std::int16_t flags = 0;
};

struct TriangleRef {
    std::uint16_t triangleSet = 0;
    std::uint16_t triangleIndex = 0;

    [[nodiscard]] bool operator==(const TriangleRef&) const = default;
};

struct TriangleRefHash {
    [[nodiscard]] std::size_t operator()(const TriangleRef& ref) const noexcept {
        return (static_cast<std::size_t>(ref.triangleSet) << 16U) ^ ref.triangleIndex;
    }
};

[[nodiscard]] std::optional<std::uint16_t> readU16BE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    if (offset + 2U > bytes.size()) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) |
        static_cast<std::uint16_t>(bytes[offset + 1U]));
}

[[nodiscard]] std::optional<std::int16_t> readI16BE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    const auto value = readU16BE(bytes, offset);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<std::int16_t>(*value);
}

[[nodiscard]] std::optional<std::int32_t> readI32BE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    const auto value = common::readU32AtBE(bytes, offset);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(*value);
}

[[nodiscard]] std::optional<std::size_t> addRelativeOffset(
    const std::size_t base,
    const std::int32_t relative,
    const std::size_t size) {
    const auto target = static_cast<std::int64_t>(base) + static_cast<std::int64_t>(relative);
    if (target < 0 || static_cast<std::uint64_t>(target) > static_cast<std::uint64_t>(size)) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(target);
}

[[nodiscard]] std::string hexOffset(const std::size_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "0x00000000";
    auto v = static_cast<std::uint64_t>(value);
    for (int i = 9; i >= 2; --i) {
        result[static_cast<std::size_t>(i)] = digits[v & 0xFU];
        v >>= 4U;
    }
    return result;
}

[[nodiscard]] std::optional<TriangleSet> readTriangleSet(
    std::span<const std::uint8_t> bytes,
    const std::size_t setOffset,
    const std::size_t nextSetOrBlockEnd,
    std::vector<std::string>& diagnostics) {
    if (setOffset + 0x18U > bytes.size()) {
        diagnostics.push_back("GRND triangle set header out of bounds at " + hexOffset(setOffset) + ".");
        return std::nullopt;
    }

    const auto vertexRel = readI32BE(bytes, setOffset + 0x0CU);
    const auto streamRel = readI32BE(bytes, setOffset + 0x10U);
    const auto triangleCount = common::readU32AtBE(bytes, setOffset + 0x14U);
    if (!vertexRel.has_value() || !streamRel.has_value() || !triangleCount.has_value()) {
        diagnostics.push_back("GRND triangle set header is truncated at " + hexOffset(setOffset) + ".");
        return std::nullopt;
    }

    const auto vertexBlockOffset = addRelativeOffset(setOffset + 0x0CU, *vertexRel, bytes.size());
    const auto triangleStreamOffset = addRelativeOffset(setOffset + 0x10U, *streamRel, bytes.size());
    if (!vertexBlockOffset.has_value() || !triangleStreamOffset.has_value()) {
        diagnostics.push_back("GRND triangle set has an out-of-bounds relative pointer at " + hexOffset(setOffset) + ".");
        return std::nullopt;
    }

    TriangleSet out{};
    out.headerOffset = setOffset;
    out.vertexBlockOffset = *vertexBlockOffset;
    out.triangleStreamOffset = *triangleStreamOffset;
    out.declaredTriangleCount = *triangleCount;

    if (out.vertexBlockOffset > out.triangleStreamOffset) {
        out.streamEntryCount = (out.vertexBlockOffset - out.triangleStreamOffset) / 4U;
    } else if (out.declaredTriangleCount == 0U && nextSetOrBlockEnd >= out.triangleStreamOffset) {
        out.streamEntryCount = (nextSetOrBlockEnd - out.triangleStreamOffset) / 4U;
    }

    return out;
}

[[nodiscard]] std::optional<StreamEntry> readStreamEntry(
    std::span<const std::uint8_t> bytes,
    const TriangleSet& set,
    const std::size_t index) {
    if (index >= set.streamEntryCount) {
        return std::nullopt;
    }
    const auto offset = set.triangleStreamOffset + (index * 4U);
    const auto floatIndex = readU16BE(bytes, offset);
    const auto flags = readI16BE(bytes, offset + 2U);
    if (!floatIndex.has_value() || !flags.has_value()) {
        return std::nullopt;
    }
    return StreamEntry{
        .floatIndex = *floatIndex,
        .flags = *flags,
    };
}

[[nodiscard]] std::optional<model::MeshVertex> readVertexForFloatIndex(
    std::span<const std::uint8_t> bytes,
    const TriangleSet& set,
    const std::uint16_t floatIndex) {
    const std::size_t offset = set.vertexBlockOffset + (static_cast<std::size_t>(floatIndex) * 4U);
    if (offset + 24U > bytes.size()) {
        return std::nullopt;
    }

    const auto px = common::readF32AtBE(bytes, offset);
    const auto py = common::readF32AtBE(bytes, offset + 4U);
    const auto pz = common::readF32AtBE(bytes, offset + 8U);
    const auto nx = common::readF32AtBE(bytes, offset + 12U);
    const auto ny = common::readF32AtBE(bytes, offset + 16U);
    const auto nz = common::readF32AtBE(bytes, offset + 20U);
    if (!px.has_value() || !py.has_value() || !pz.has_value() ||
        !nx.has_value() || !ny.has_value() || !nz.has_value()) {
        return std::nullopt;
    }

    model::MeshVertex out{};
    out.position = model::Vec3{ *px, *py, *pz };
    out.normal = model::Vec3{ *nx, *ny, *nz };
    return out;
}

[[nodiscard]] std::uint32_t getOrCreateVertex(
    std::span<const std::uint8_t> bytes,
    const TriangleSet& set,
    const std::uint16_t floatIndex,
    std::unordered_map<std::uint64_t, std::uint32_t>& vertexIndexByKey,
    model::MeshData& mesh,
    std::size_t& skippedReferenceCount) {
    const std::uint64_t key = (static_cast<std::uint64_t>(set.headerOffset) << 16U) ^ floatIndex;
    if (const auto found = vertexIndexByKey.find(key); found != vertexIndexByKey.end()) {
        return found->second;
    }

    const auto vertex = readVertexForFloatIndex(bytes, set, floatIndex);
    if (!vertex.has_value()) {
        ++skippedReferenceCount;
        return std::numeric_limits<std::uint32_t>::max();
    }

    const auto index = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(*vertex);
    vertexIndexByKey.emplace(key, index);
    return index;
}

} // namespace

GrndDecodeResult GrndParser::decode(std::span<const std::uint8_t> blockBytes, const std::uint32_t sourceOffset) const {
    GrndDecodeResult result{};
    if (blockBytes.size() < 0x2CU) {
        result.diagnostics.push_back("GRND block at " + hexOffset(sourceOffset) + " is too small.");
        return result;
    }

    if (common::readU32AtBE(blockBytes, 0U).value_or(0U) != kGrndTag) {
        result.diagnostics.push_back("GRND block at " + hexOffset(sourceOffset) + " does not start with GRND magic.");
        return result;
    }

    const auto declaredSize = common::readU32AtBE(blockBytes, 4U);
    if (!declaredSize.has_value() || *declaredSize < 0x2CU || *declaredSize > blockBytes.size()) {
        result.diagnostics.push_back("GRND block at " + hexOffset(sourceOffset) + " has an invalid declared size.");
        return result;
    }

    const auto bytes = blockBytes.first(static_cast<std::size_t>(*declaredSize));
    constexpr std::size_t innerHeader = 0x10U;
    const auto relTriangleSets = readI32BE(bytes, innerHeader);
    const auto relQuadRegistry = readI32BE(bytes, innerHeader + 4U);
    const auto gridX = readU16BE(bytes, innerHeader + 0x10U);
    const auto gridZ = readU16BE(bytes, innerHeader + 0x12U);
    const auto cellSizeX = readU16BE(bytes, innerHeader + 0x14U);
    const auto cellSizeZ = readU16BE(bytes, innerHeader + 0x16U);
    const auto triangleSetCount = readU16BE(bytes, innerHeader + 0x18U);
    const auto quadCellCount = readU16BE(bytes, innerHeader + 0x1AU);

    if (!relTriangleSets.has_value() || !relQuadRegistry.has_value() ||
        !gridX.has_value() || !gridZ.has_value() || !cellSizeX.has_value() || !cellSizeZ.has_value() ||
        !triangleSetCount.has_value() || !quadCellCount.has_value()) {
        result.diagnostics.push_back("GRND block at " + hexOffset(sourceOffset) + " has a truncated inner header.");
        return result;
    }

    result.gridX = *gridX;
    result.gridZ = *gridZ;
    result.cellSizeX = *cellSizeX;
    result.cellSizeZ = *cellSizeZ;
    result.triangleSetCount = *triangleSetCount;
    result.quadCellCount = *quadCellCount;

    const auto triangleSetsOffset = addRelativeOffset(innerHeader, *relTriangleSets, bytes.size());
    const auto quadRegistryOffset = addRelativeOffset(innerHeader, *relQuadRegistry, bytes.size());
    if (!triangleSetsOffset.has_value() || !quadRegistryOffset.has_value()) {
        result.diagnostics.push_back("GRND block at " + hexOffset(sourceOffset) + " has out-of-bounds registry pointers.");
        return result;
    }

    const std::size_t quadTableOffset = *quadRegistryOffset + 4U;
    if (quadTableOffset + (static_cast<std::size_t>(*quadCellCount) * 8U) > bytes.size()) {
        result.diagnostics.push_back("GRND block at " + hexOffset(sourceOffset) + " has an out-of-bounds quad table.");
        return result;
    }

    std::vector<std::optional<TriangleSet>> triangleSets(static_cast<std::size_t>(*triangleSetCount));
    for (std::size_t setIndex = 0; setIndex < *triangleSetCount; ++setIndex) {
        const std::size_t setOffset = *triangleSetsOffset + (setIndex * 0x18U);
        const std::size_t nextSetOrBlockEnd = (setIndex + 1U < *triangleSetCount)
            ? *triangleSetsOffset + ((setIndex + 1U) * 0x18U)
            : bytes.size();
        auto set = readTriangleSet(bytes, setOffset, nextSetOrBlockEnd, result.diagnostics);
        if (!set.has_value()) {
            continue;
        }
        triangleSets[setIndex] = *set;
    }

    std::unordered_set<TriangleRef, TriangleRefHash> uniqueReferences{};
    std::unordered_map<std::uint64_t, std::uint32_t> vertexIndexByKey{};

    for (std::size_t quadIndex = 0; quadIndex < *quadCellCount; ++quadIndex) {
        const std::size_t quadOffset = quadTableOffset + (quadIndex * 8U);
        const auto refCount = common::readU32AtBE(bytes, quadOffset);
        const auto relRefList = readI32BE(bytes, quadOffset + 4U);
        if (!refCount.has_value() || !relRefList.has_value()) {
            ++result.skippedReferenceCount;
            continue;
        }
        if (*refCount == 0U) {
            continue;
        }

        const auto refListOffset = addRelativeOffset(quadOffset + 4U, *relRefList, bytes.size());
        if (!refListOffset.has_value() ||
            *refListOffset + (static_cast<std::size_t>(*refCount) * 4U) > bytes.size()) {
            result.skippedReferenceCount += *refCount;
            continue;
        }

        for (std::size_t refIndex = 0; refIndex < *refCount; ++refIndex) {
            const std::size_t refOffset = *refListOffset + (refIndex * 4U);
            const auto setIndex = readU16BE(bytes, refOffset);
            const auto triangleIndex = readU16BE(bytes, refOffset + 2U);
            if (!setIndex.has_value() || !triangleIndex.has_value()) {
                ++result.skippedReferenceCount;
                continue;
            }

            const TriangleRef ref{
                .triangleSet = *setIndex,
                .triangleIndex = *triangleIndex,
            };
            if (!uniqueReferences.insert(ref).second) {
                ++result.duplicateReferenceCount;
                continue;
            }
            if (*setIndex >= triangleSets.size() || !triangleSets[*setIndex].has_value()) {
                ++result.skippedReferenceCount;
                continue;
            }

            const auto& set = *triangleSets[*setIndex];
            const std::size_t streamIndex = *triangleIndex;
            const auto e0 = readStreamEntry(bytes, set, streamIndex);
            const auto e1 = readStreamEntry(bytes, set, streamIndex + 1U);
            const auto e2 = readStreamEntry(bytes, set, streamIndex + 2U);
            if (!e0.has_value() || !e1.has_value() || !e2.has_value()) {
                ++result.skippedReferenceCount;
                continue;
            }

            const auto i0 = getOrCreateVertex(bytes, set, e0->floatIndex, vertexIndexByKey, result.mesh, result.skippedReferenceCount);
            const auto i1 = getOrCreateVertex(bytes, set, e1->floatIndex, vertexIndexByKey, result.mesh, result.skippedReferenceCount);
            const auto i2 = getOrCreateVertex(bytes, set, e2->floatIndex, vertexIndexByKey, result.mesh, result.skippedReferenceCount);
            if (i0 == std::numeric_limits<std::uint32_t>::max() ||
                i1 == std::numeric_limits<std::uint32_t>::max() ||
                i2 == std::numeric_limits<std::uint32_t>::max()) {
                continue;
            }

            if (e2->flags < 0) {
                result.mesh.indices.push_back(i2);
                result.mesh.indices.push_back(i1);
                result.mesh.indices.push_back(i0);
            } else {
                result.mesh.indices.push_back(i0);
                result.mesh.indices.push_back(i1);
                result.mesh.indices.push_back(i2);
            }
            ++result.referencedTriangleCount;
        }
    }

    result.decoded = !result.mesh.vertices.empty() && !result.mesh.indices.empty();
    result.diagnostics.push_back("GRND decoded at " + hexOffset(sourceOffset) +
        ": vertices=" + std::to_string(result.mesh.vertices.size()) +
        ", triangles=" + std::to_string(result.mesh.indices.size() / 3U) +
        ", duplicateRefs=" + std::to_string(result.duplicateReferenceCount) +
        ", skippedRefs=" + std::to_string(result.skippedReferenceCount) + ".");
    return result;
}

} // namespace soasim::mld::parsing
