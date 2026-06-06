#include "GobjParser.h"

#include "../common/ByteUtils.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace soasim::mld::parsing {
namespace {

constexpr std::uint32_t kGobjTag = 0x474F424AU; // "GOBJ"
constexpr std::uint32_t kRootNodeOffset = 0x10U;
constexpr std::uint32_t kNodeSize = 0x34U;
constexpr std::uint32_t kAttachPayloadOffset = 0x10U;
constexpr std::uint32_t kAttachPolyOffsetFromPayload = 76U;
constexpr float kPi = 3.14159265358979323846F;
constexpr float kTau = 2.0F * kPi;

struct GobjAttachLayout {
    std::uint32_t attachOffset = 0;
    std::uint32_t payloadOffset = 0;
    std::uint32_t vertexOffset = 0;
    std::uint32_t polyOffset = 0;
};

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

[[nodiscard]] std::optional<std::int32_t> readI32BE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    const auto value = common::readU32AtBE(bytes, offset);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(*value);
}

[[nodiscard]] std::optional<std::uint32_t> addRelativeTarget(
    const std::uint32_t base,
    const std::int32_t relative,
    const std::size_t size) {
    if (relative == 0) {
        return std::nullopt;
    }
    const auto target = static_cast<std::int64_t>(base) + static_cast<std::int64_t>(relative);
    if (target < 0 || static_cast<std::uint64_t>(target) >= static_cast<std::uint64_t>(size)) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(target);
}

[[nodiscard]] std::optional<std::uint16_t> readU16BE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    if (offset + 2U > bytes.size()) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8U) |
        static_cast<std::uint16_t>(bytes[offset + 1U]));
}

[[nodiscard]] std::optional<GobjAttachLayout> readGobjAttachLayout(
    std::span<const std::uint8_t> bytes,
    const std::uint32_t attachOffset,
    std::vector<std::string>& diagnostics) {
    if (static_cast<std::size_t>(attachOffset) + kAttachPayloadOffset + 4U > bytes.size()) {
        diagnostics.push_back("GOBJ attach is truncated at " + hexOffset(attachOffset) + ".");
        return std::nullopt;
    }

    const std::uint32_t payloadOffset = attachOffset + kAttachPayloadOffset;
    const auto vertexRel = readI32BE(bytes, payloadOffset);
    if (!vertexRel.has_value()) {
        diagnostics.push_back("GOBJ attach has no vertex pointer at " + hexOffset(attachOffset) + ".");
        return std::nullopt;
    }

    const auto vertexOffset = addRelativeTarget(payloadOffset, *vertexRel, bytes.size());
    if (!vertexOffset.has_value()) {
        diagnostics.push_back("GOBJ attach vertex pointer is out of bounds at " + hexOffset(attachOffset) + ".");
        return std::nullopt;
    }

    const std::uint32_t polyOffset = payloadOffset + kAttachPolyOffsetFromPayload;
    if (static_cast<std::size_t>(polyOffset) >= bytes.size() || polyOffset >= *vertexOffset) {
        diagnostics.push_back("GOBJ attach poly chunks are out of bounds at " + hexOffset(attachOffset) + ".");
        return std::nullopt;
    }

    return GobjAttachLayout{
        .attachOffset = attachOffset,
        .payloadOffset = payloadOffset,
        .vertexOffset = *vertexOffset,
        .polyOffset = polyOffset,
    };
}

[[nodiscard]] float readBams32RadiansBE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return static_cast<float>(readI32BE(bytes, offset).value_or(0)) * (kTau / 65536.0F);
}

[[nodiscard]] model::Vec3 readVec3F32BE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return model::Vec3{
        common::readF32AtBE(bytes, offset + 0U).value_or(0.0F),
        common::readF32AtBE(bytes, offset + 4U).value_or(0.0F),
        common::readF32AtBE(bytes, offset + 8U).value_or(0.0F),
    };
}

[[nodiscard]] model::Vec3 readVec3Bams32BE(std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return model::Vec3{
        readBams32RadiansBE(bytes, offset + 0U),
        readBams32RadiansBE(bytes, offset + 4U),
        readBams32RadiansBE(bytes, offset + 8U),
    };
}

[[nodiscard]] model::Quat eulerRadiansToQuaternionXYZ(const model::Vec3& radians) {
    const float cx = std::cos(radians.x * 0.5f);
    const float sx = std::sin(radians.x * 0.5f);
    const float cy = std::cos(radians.y * 0.5f);
    const float sy = std::sin(radians.y * 0.5f);
    const float cz = std::cos(radians.z * 0.5f);
    const float sz = std::sin(radians.z * 0.5f);

    return model::Quat{
        sx * cy * cz + cx * sy * sz,
        cx * sy * cz - sx * cy * sz,
        cx * cy * sz + sx * sy * cz,
        cx * cy * cz - sx * sy * sz,
    };
}

[[nodiscard]] model::Transform readNodeTransform(
    std::span<const std::uint8_t> bytes,
    const std::uint32_t nodeOffset) {
    model::Transform result{};
    result.position = readVec3F32BE(bytes, nodeOffset + 8U);
    result.rotationRaw = readVec3Bams32BE(bytes, nodeOffset + 0x14U);
    result.rotation = eulerRadiansToQuaternionXYZ(result.rotationRaw);
    result.scale = readVec3F32BE(bytes, nodeOffset + 0x20U);
    return result;
}

[[nodiscard]] bool isAlignedGobjFloatIndex(const std::uint16_t floatIndex, const std::uint16_t vertexCount) {
    if (floatIndex < 2U || ((floatIndex - 2U) % 6U) != 0U) {
        return false;
    }
    return ((floatIndex - 2U) / 6U) < vertexCount;
}

[[nodiscard]] std::optional<model::MeshVertex> readGobjStreamVertex(
    std::span<const std::uint8_t> bytes,
    const std::uint32_t vertexOffset,
    const std::uint16_t floatIndex,
    const std::uint16_t vertexCount) {
    if (!isAlignedGobjFloatIndex(floatIndex, vertexCount)) {
        return std::nullopt;
    }

    const std::size_t positionOffset = static_cast<std::size_t>(vertexOffset) + static_cast<std::size_t>(floatIndex) * 4U;
    const auto bucket = static_cast<std::size_t>((floatIndex - 2U) / 6U);
    const std::size_t normalOffset = static_cast<std::size_t>(vertexOffset) + ((bucket * 6U) + 5U) * 4U;
    if (positionOffset + 12U > bytes.size() || normalOffset + 12U > bytes.size()) {
        return std::nullopt;
    }

    model::MeshVertex vertex{};
    vertex.position = model::Vec3{
        common::readF32AtBE(bytes, positionOffset + 0U).value_or(0.0F),
        common::readF32AtBE(bytes, positionOffset + 4U).value_or(0.0F),
        common::readF32AtBE(bytes, positionOffset + 8U).value_or(0.0F),
    };
    vertex.normal = model::Vec3{
        common::readF32AtBE(bytes, normalOffset + 0U).value_or(0.0F),
        common::readF32AtBE(bytes, normalOffset + 4U).value_or(1.0F),
        common::readF32AtBE(bytes, normalOffset + 8U).value_or(0.0F),
    };
    return vertex;
}

[[nodiscard]] model::MeshData readGobjTriangleStreamMesh(
    std::span<const std::uint8_t> bytes,
    const GobjAttachLayout& layout,
    std::vector<std::string>& diagnostics) {
    model::MeshData mesh{};
    if (static_cast<std::size_t>(layout.vertexOffset) + 8U > bytes.size()) {
        return mesh;
    }

    const auto header1 = common::readU32AtBE(bytes, layout.vertexOffset);
    const auto header2 = common::readU32AtBE(bytes, static_cast<std::size_t>(layout.vertexOffset) + 4U);
    if (!header1.has_value() || !header2.has_value() || (*header1 & 0xFFU) != 0x29U) {
        diagnostics.push_back("GOBJ stream parser skipped unsupported vertex chunk at " + hexOffset(layout.vertexOffset) + ".");
        return mesh;
    }

    const auto vertexCount = static_cast<std::uint16_t>(*header2 >> 16U);
    struct Entry {
        std::uint16_t floatIndex = 0;
        std::int16_t flags = 0;
        model::MeshVertex vertex{};
    };

    std::vector<Entry> run{};
    std::size_t runCount = 0;
    std::size_t separatorCount = 0;
    std::size_t controlCount = 0;
    const auto appendRunTriangles = [&]() {
        if (run.size() < 3U) {
            run.clear();
            return;
        }
        ++runCount;
        for (std::size_t i = 0; i + 2U < run.size(); ++i) {
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
            if (run[i + 2U].flags < 0) {
                mesh.vertices.push_back(run[i + 2U].vertex);
                mesh.vertices.push_back(run[i + 1U].vertex);
                mesh.vertices.push_back(run[i + 0U].vertex);
            } else {
                mesh.vertices.push_back(run[i + 0U].vertex);
                mesh.vertices.push_back(run[i + 1U].vertex);
                mesh.vertices.push_back(run[i + 2U].vertex);
            }
            mesh.indices.push_back(base + 0U);
            mesh.indices.push_back(base + 1U);
            mesh.indices.push_back(base + 2U);
        }
        run.clear();
    };

    for (std::size_t offset = layout.polyOffset; offset + 4U <= layout.vertexOffset; offset += 4U) {
        const auto floatIndex = readU16BE(bytes, offset);
        const auto flags = readU16BE(bytes, offset + 2U);
        if (!floatIndex.has_value() || !flags.has_value()) {
            break;
        }
        if (*floatIndex == 0xFFFFU || *flags == 0xFFFFU) {
            ++separatorCount;
            appendRunTriangles();
            continue;
        }

        const auto vertex = readGobjStreamVertex(bytes, layout.vertexOffset, *floatIndex, vertexCount);
        if (!vertex.has_value()) {
            ++controlCount;
            appendRunTriangles();
            continue;
        }

        run.push_back(Entry{
            .floatIndex = *floatIndex,
            .flags = static_cast<std::int16_t>(*flags),
            .vertex = *vertex,
        });
    }
    appendRunTriangles();

    if (!mesh.indices.empty()) {
        diagnostics.push_back("GOBJ stream parser produced " + std::to_string(mesh.indices.size() / 3U) +
            " triangle(s) across " + std::to_string(runCount) +
            " run(s) at " + hexOffset(layout.attachOffset) +
            " after " + std::to_string(separatorCount) + " separator(s) and " +
            std::to_string(controlCount) + " control record(s).");
    }
    return mesh;
}

struct WalkContext {
    std::span<const std::uint8_t> bytes{};
    GobjDecodeResult* result = nullptr;
    std::unordered_map<std::uint32_t, std::size_t> nodeIndexByOffset{};
    std::unordered_set<std::uint32_t> activeStack{};
};

[[nodiscard]] std::optional<std::size_t> walkNode(
    WalkContext& context,
    const std::uint32_t nodeOffset,
    const std::optional<std::size_t> parentIndex) {
    if (static_cast<std::size_t>(nodeOffset) + kNodeSize > context.bytes.size()) {
        context.result->diagnostics.push_back("GOBJ node is out of bounds at " + hexOffset(nodeOffset) + ".");
        return std::nullopt;
    }

    if (const auto existing = context.nodeIndexByOffset.find(nodeOffset); existing != context.nodeIndexByOffset.end()) {
        return existing->second;
    }
    if (!context.activeStack.insert(nodeOffset).second) {
        context.result->diagnostics.push_back("GOBJ node cycle detected at " + hexOffset(nodeOffset) + ".");
        return std::nullopt;
    }

    GobjNode node{};
    node.sourceNodeOffset = nodeOffset;
    node.parentNodeIndex = parentIndex;
    node.transform = readNodeTransform(context.bytes, nodeOffset);

    const auto attachRel = readI32BE(context.bytes, nodeOffset);
    if (attachRel.has_value()) {
        if (const auto attachOffset = addRelativeTarget(nodeOffset, *attachRel, context.bytes.size())) {
            if (const auto layout = readGobjAttachLayout(context.bytes, *attachOffset, context.result->diagnostics)) {
                node.sourceAttachOffset = layout->attachOffset;
                node.streamMesh = readGobjTriangleStreamMesh(context.bytes, *layout, context.result->diagnostics);
            }
        }
    }

    const std::size_t nodeIndex = context.result->nodes.size();
    context.nodeIndexByOffset.emplace(nodeOffset, nodeIndex);
    context.result->nodes.push_back(std::move(node));
    if (!parentIndex.has_value()) {
        context.result->rootNodeIndices.push_back(nodeIndex);
    }

    const auto childRel = readI32BE(context.bytes, nodeOffset + 0x2CU);
    if (childRel.has_value()) {
        if (const auto childOffset = addRelativeTarget(nodeOffset + 0x2CU, *childRel, context.bytes.size())) {
            if (const auto childIndex = walkNode(context, *childOffset, nodeIndex)) {
                context.result->nodes[nodeIndex].childNodeIndices.push_back(*childIndex);
            }
        }
    }

    const auto siblingRel = readI32BE(context.bytes, nodeOffset + 0x30U);
    if (siblingRel.has_value()) {
        if (const auto siblingOffset = addRelativeTarget(nodeOffset + 0x2CU, *siblingRel, context.bytes.size())) {
            (void)walkNode(context, *siblingOffset, parentIndex);
        }
    }

    context.activeStack.erase(nodeOffset);
    return nodeIndex;
}

} // namespace

GobjDecodeResult GobjParser::decode(std::span<const std::uint8_t> blockBytes, const std::uint32_t sourceOffset) const {
    GobjDecodeResult result{};
    result.sourceOffset = sourceOffset;

    if (blockBytes.size() < kRootNodeOffset + kNodeSize) {
        result.diagnostics.push_back("GOBJ block at " + hexOffset(sourceOffset) + " is too small.");
        return result;
    }

    if (common::readU32AtBE(blockBytes, 0U).value_or(0U) != kGobjTag) {
        result.diagnostics.push_back("GOBJ block at " + hexOffset(sourceOffset) + " does not start with GOBJ magic.");
        return result;
    }

    const auto declaredSize = common::readU32AtBE(blockBytes, 4U);
    if (!declaredSize.has_value() || *declaredSize < kRootNodeOffset + kNodeSize || *declaredSize > blockBytes.size()) {
        result.diagnostics.push_back("GOBJ block at " + hexOffset(sourceOffset) + " has an invalid declared size.");
        return result;
    }

    const auto bytes = blockBytes.first(static_cast<std::size_t>(*declaredSize));
    WalkContext context{
        .bytes = bytes,
        .result = &result,
    };

    (void)walkNode(context, kRootNodeOffset, std::nullopt);
    std::size_t streamMeshCount = 0;
    for (const auto& node : result.nodes) {
        if (!node.streamMesh.indices.empty()) {
            ++streamMeshCount;
        }
    }

    result.decoded = streamMeshCount > 0;
    result.diagnostics.push_back("GOBJ decoded at " + hexOffset(sourceOffset) +
        ": nodes=" + std::to_string(result.nodes.size()) +
        ", streamMeshes=" + std::to_string(streamMeshCount) + ".");
    return result;
}

} // namespace soasim::mld::parsing
