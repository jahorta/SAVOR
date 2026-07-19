#include "RegionMeshProjector.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace savor::navigation {
namespace {

using BlenderIrMesh = spice::mld::model::BlenderIrMesh;
using BlenderIrObjectTree = spice::mld::model::BlenderIrObjectTree;
using SpiceTransform = spice::mld::model::Transform;

constexpr float kNormalEpsilon = 1.0e-6F;

struct Matrix4 {
    float values[4][4]{};
};

[[nodiscard]] Matrix4 identityMatrix() {
    Matrix4 out{};
    for (std::size_t i = 0; i < 4; ++i) {
        out.values[i][i] = 1.0F;
    }
    return out;
}

[[nodiscard]] Matrix4 multiply(const Matrix4& lhs, const Matrix4& rhs) {
    Matrix4 out{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            for (std::size_t inner = 0; inner < 4; ++inner) {
                out.values[row][column] += lhs.values[row][inner] * rhs.values[inner][column];
            }
        }
    }
    return out;
}

[[nodiscard]] Matrix4 transformMatrix(const SpiceTransform& transform) {
    const float x = transform.rotation.x;
    const float y = transform.rotation.y;
    const float z = transform.rotation.z;
    const float w = transform.rotation.w;

    Matrix4 rotation = identityMatrix();
    rotation.values[0][0] = 1.0F - (2.0F * ((y * y) + (z * z)));
    rotation.values[0][1] = 2.0F * ((x * y) - (z * w));
    rotation.values[0][2] = 2.0F * ((x * z) + (y * w));
    rotation.values[1][0] = 2.0F * ((x * y) + (z * w));
    rotation.values[1][1] = 1.0F - (2.0F * ((x * x) + (z * z)));
    rotation.values[1][2] = 2.0F * ((y * z) - (x * w));
    rotation.values[2][0] = 2.0F * ((x * z) - (y * w));
    rotation.values[2][1] = 2.0F * ((y * z) + (x * w));
    rotation.values[2][2] = 1.0F - (2.0F * ((x * x) + (y * y)));

    Matrix4 scale = identityMatrix();
    scale.values[0][0] = transform.scale.x;
    scale.values[1][1] = transform.scale.y;
    scale.values[2][2] = transform.scale.z;

    Matrix4 translation = identityMatrix();
    translation.values[0][3] = transform.position.x;
    translation.values[1][3] = transform.position.y;
    translation.values[2][3] = transform.position.z;
    return multiply(translation, multiply(rotation, scale));
}

[[nodiscard]] NavigationVec3 transformPoint(const Matrix4& matrix, const NavigationVec3& source) {
    return NavigationVec3{
        (matrix.values[0][0] * source.x) + (matrix.values[0][1] * source.y) +
            (matrix.values[0][2] * source.z) + matrix.values[0][3],
        (matrix.values[1][0] * source.x) + (matrix.values[1][1] * source.y) +
            (matrix.values[1][2] * source.z) + matrix.values[1][3],
        (matrix.values[2][0] * source.x) + (matrix.values[2][1] * source.y) +
            (matrix.values[2][2] * source.z) + matrix.values[2][3],
    };
}

[[nodiscard]] NavigationVec3 add(const NavigationVec3& lhs, const NavigationVec3& rhs) {
    return NavigationVec3{ lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z };
}

[[nodiscard]] NavigationVec3 subtract(const NavigationVec3& lhs, const NavigationVec3& rhs) {
    return NavigationVec3{ lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z };
}

[[nodiscard]] NavigationVec3 cross(const NavigationVec3& lhs, const NavigationVec3& rhs) {
    return NavigationVec3{
        (lhs.y * rhs.z) - (lhs.z * rhs.y),
        (lhs.z * rhs.x) - (lhs.x * rhs.z),
        (lhs.x * rhs.y) - (lhs.y * rhs.x),
    };
}

[[nodiscard]] NavigationVec3 normalize(const NavigationVec3& value) {
    const float lengthSquared = (value.x * value.x) + (value.y * value.y) + (value.z * value.z);
    if (lengthSquared <= kNormalEpsilon) {
        return NavigationVec3{ 0.0F, 1.0F, 0.0F };
    }
    const float inverseLength = 1.0F / std::sqrt(lengthSquared);
    return NavigationVec3{ value.x * inverseLength, value.y * inverseLength, value.z * inverseLength };
}

[[nodiscard]] NavigationVec3 transformNormal(const Matrix4& matrix, const NavigationVec3& source) {
    const float a00 = matrix.values[0][0];
    const float a01 = matrix.values[0][1];
    const float a02 = matrix.values[0][2];
    const float a10 = matrix.values[1][0];
    const float a11 = matrix.values[1][1];
    const float a12 = matrix.values[1][2];
    const float a20 = matrix.values[2][0];
    const float a21 = matrix.values[2][1];
    const float a22 = matrix.values[2][2];

    const float cofactor00 = (a11 * a22) - (a12 * a21);
    const float cofactor01 = (a12 * a20) - (a10 * a22);
    const float cofactor02 = (a10 * a21) - (a11 * a20);
    const float determinant = (a00 * cofactor00) + (a01 * cofactor01) + (a02 * cofactor02);
    if (std::abs(determinant) <= kNormalEpsilon) {
        return normalize(source);
    }

    const float inverseDeterminant = 1.0F / determinant;
    const float inverse00 = cofactor00 * inverseDeterminant;
    const float inverse01 = ((a02 * a21) - (a01 * a22)) * inverseDeterminant;
    const float inverse02 = ((a01 * a12) - (a02 * a11)) * inverseDeterminant;
    const float inverse10 = cofactor01 * inverseDeterminant;
    const float inverse11 = ((a00 * a22) - (a02 * a20)) * inverseDeterminant;
    const float inverse12 = ((a02 * a10) - (a00 * a12)) * inverseDeterminant;
    const float inverse20 = cofactor02 * inverseDeterminant;
    const float inverse21 = ((a01 * a20) - (a00 * a21)) * inverseDeterminant;
    const float inverse22 = ((a00 * a11) - (a01 * a10)) * inverseDeterminant;

    return normalize(NavigationVec3{
        (inverse00 * source.x) + (inverse10 * source.y) + (inverse20 * source.z),
        (inverse01 * source.x) + (inverse11 * source.y) + (inverse21 * source.z),
        (inverse02 * source.x) + (inverse12 * source.y) + (inverse22 * source.z),
    });
}

void appendWarning(std::vector<NavigationDiagnostic>& diagnostics, std::string message) {
    diagnostics.push_back(NavigationDiagnostic{
        .severity = NavigationDiagnosticSeverity::Warning,
        .message = std::move(message),
    });
}

[[nodiscard]] std::string regionLabel(const NavigationRegionKind kind) {
    switch (kind) {
    case NavigationRegionKind::Collision:
        return "Wall collision";
    case NavigationRegionKind::Trigger:
        return "Trigger";
    case NavigationRegionKind::MovingObject:
        return "Moving object";
    case NavigationRegionKind::Unknown:
    default:
        return "Region";
    }
}

[[nodiscard]] std::vector<Matrix4> buildNodeWorldMatrices(
    const BlenderIrObjectTree& tree,
    const NavigationRegionKind kind,
    const std::uint32_t entryId,
    std::vector<NavigationDiagnostic>& diagnostics) {
    std::vector<std::optional<Matrix4>> memo(tree.nodes.size());
    std::vector<bool> active(tree.nodes.size(), false);

    std::function<Matrix4(std::size_t)> resolve = [&](const std::size_t nodeIndex) -> Matrix4 {
        if (memo[nodeIndex].has_value()) {
            return *memo[nodeIndex];
        }
        if (active[nodeIndex]) {
            appendWarning(diagnostics, regionLabel(kind) + " entry=" + std::to_string(entryId) +
                " contains an object-tree transform cycle; identity ancestry was used.");
            return identityMatrix();
        }
        active[nodeIndex] = true;
        Matrix4 parent = identityMatrix();
        if (const auto parentIndex = tree.nodes[nodeIndex].parentNodeIndex; parentIndex.has_value()) {
            if (*parentIndex < tree.nodes.size()) {
                parent = resolve(*parentIndex);
            } else {
                appendWarning(diagnostics, regionLabel(kind) + " entry=" + std::to_string(entryId) +
                    " contains an invalid parent-node index.");
            }
        }
        active[nodeIndex] = false;
        memo[nodeIndex] = multiply(parent, transformMatrix(tree.nodes[nodeIndex].localTransform));
        return *memo[nodeIndex];
    };

    std::vector<Matrix4> matrices(tree.nodes.size());
    for (std::size_t nodeIndex = 0; nodeIndex < tree.nodes.size(); ++nodeIndex) {
        matrices[nodeIndex] = resolve(nodeIndex);
    }
    return matrices;
}

[[nodiscard]] NavigationMesh convertMesh(
    const BlenderIrMesh& source,
    const Matrix4& world,
    const NavigationRegionKind kind,
    const std::uint32_t entryId,
    std::vector<NavigationDiagnostic>& diagnostics) {
    NavigationMesh out{};
    out.vertices.reserve(source.vertices.size());
    std::vector<bool> validPositions{};
    validPositions.reserve(source.vertices.size());
    for (const auto& sourceVertex : source.vertices) {
        NavigationMeshVertex vertex{};
        vertex.position = transformPoint(world, NavigationVec3{
            sourceVertex.position.x,
            sourceVertex.position.y,
            sourceVertex.position.z,
        });
        vertex.hasSourceNormal = sourceVertex.hasNormal;
        if (sourceVertex.hasNormal) {
            vertex.normal = transformNormal(world, NavigationVec3{
                sourceVertex.normal.x,
                sourceVertex.normal.y,
                sourceVertex.normal.z,
            });
        }
        vertex.rawUserAttributesU32 = sourceVertex.rawUserAttributesU32;
        out.vertices.push_back(std::move(vertex));
        validPositions.push_back(sourceVertex.hasPosition);
    }

    std::size_t rejectedTriangles = 0;
    std::size_t trailingCorners = 0;
    for (const auto& triangleSet : source.triangleSets) {
        const std::size_t triangleCount = triangleSet.corners.size() / 3U;
        trailingCorners += triangleSet.corners.size() % 3U;
        for (std::size_t triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex) {
            const std::size_t base = triangleIndex * 3U;
            const auto a = triangleSet.corners[base + 0U].vertexIndex;
            const auto b = triangleSet.corners[base + 1U].vertexIndex;
            const auto c = triangleSet.corners[base + 2U].vertexIndex;
            if (a >= out.vertices.size() || b >= out.vertices.size() || c >= out.vertices.size() ||
                !validPositions[a] || !validPositions[b] || !validPositions[c]) {
                ++rejectedTriangles;
                continue;
            }
            out.indices.push_back(a);
            out.indices.push_back(b);
            out.indices.push_back(c);

            NavigationTriangleMetadata metadata{};
            if (triangleIndex < triangleSet.triangleMetadata.size()) {
                metadata.rawU16 = triangleSet.triangleMetadata[triangleIndex].rawU16;
                metadata.present = true;
            }
            out.triangleMetadata.push_back(metadata);
        }
    }

    if (trailingCorners > 0U) {
        appendWarning(diagnostics, regionLabel(kind) + " entry=" + std::to_string(entryId) + " mesh=" + source.label +
            " has trailing non-triangle corners; they were ignored.");
    }
    if (rejectedTriangles > 0U) {
        appendWarning(diagnostics, regionLabel(kind) + " entry=" + std::to_string(entryId) + " mesh=" + source.label +
            " rejected " + std::to_string(rejectedTriangles) + " triangle(s) with invalid vertices.");
    }

    std::vector<NavigationVec3> generatedNormals(out.vertices.size());
    for (std::size_t index = 0; index + 2U < out.indices.size(); index += 3U) {
        const auto a = out.indices[index + 0U];
        const auto b = out.indices[index + 1U];
        const auto c = out.indices[index + 2U];
        const auto faceNormal = cross(
            subtract(out.vertices[b].position, out.vertices[a].position),
            subtract(out.vertices[c].position, out.vertices[a].position));
        if (!out.vertices[a].hasSourceNormal) {
            generatedNormals[a] = add(generatedNormals[a], faceNormal);
        }
        if (!out.vertices[b].hasSourceNormal) {
            generatedNormals[b] = add(generatedNormals[b], faceNormal);
        }
        if (!out.vertices[c].hasSourceNormal) {
            generatedNormals[c] = add(generatedNormals[c], faceNormal);
        }
    }
    for (std::size_t vertexIndex = 0; vertexIndex < out.vertices.size(); ++vertexIndex) {
        if (!out.vertices[vertexIndex].hasSourceNormal) {
            out.vertices[vertexIndex].normal = normalize(generatedNormals[vertexIndex]);
        }
    }
    return out;
}

} // namespace

RegionMeshProjectionResult RegionMeshProjector::project(
    const spice::mld::model::BlenderIrScene& scene,
    const std::span<const RegionMeshProjectionTarget> targets) const {
    RegionMeshProjectionResult out{};
    std::vector<bool> claimedInstances(scene.indexEntries.size(), false);
    for (const auto& target : targets) {
        ProjectedNavigationRegion region{};
        region.kind = target.kind;
        region.sourceEntryId = target.sourceEntryId;
        region.tblId = target.tblId;

        const spice::mld::model::BlenderIrInstance* instance = nullptr;
        for (std::size_t index = 0; index < scene.indexEntries.size(); ++index) {
            const auto& candidate = scene.indexEntries[index];
            if (!claimedInstances[index] && candidate.sourceEntryId == target.sourceEntryId &&
                candidate.tblId == target.tblId) {
                claimedInstances[index] = true;
                instance = &candidate;
                break;
            }
        }
        if (instance == nullptr) {
            appendWarning(out.diagnostics, regionLabel(target.kind) + " entry=" +
                std::to_string(target.sourceEntryId) + " tbl=" + std::to_string(target.tblId) +
                " has no matching Blender IR instance.");
            out.regions.push_back(std::move(region));
            continue;
        }

        region.sourceEntryId = instance->sourceEntryId;
        region.sourceTableIndex = instance->tableIndex;
        region.tblId = instance->tblId;
        const Matrix4 entryWorld = transformMatrix(instance->transform);

        for (const auto treeIndex : instance->objectTreeIndices) {
            if (treeIndex >= scene.objectTrees.size()) {
                appendWarning(out.diagnostics, regionLabel(target.kind) + " entry=" +
                    std::to_string(instance->sourceEntryId) +
                    " references missing object tree " + std::to_string(treeIndex) + '.');
                continue;
            }
            const auto& tree = scene.objectTrees[treeIndex];
            const auto nodeWorld = buildNodeWorldMatrices(
                tree, target.kind, instance->sourceEntryId, out.diagnostics);
            for (std::size_t nodeIndex = 0; nodeIndex < tree.nodes.size(); ++nodeIndex) {
                const auto& node = tree.nodes[nodeIndex];
                if (!node.meshIndex.has_value()) {
                    continue;
                }
                if (*node.meshIndex >= scene.meshes.size()) {
                    appendWarning(out.diagnostics, regionLabel(target.kind) + " entry=" +
                        std::to_string(instance->sourceEntryId) +
                        " references missing mesh " + std::to_string(*node.meshIndex) + '.');
                    continue;
                }

                const auto& sourceMesh = scene.meshes[*node.meshIndex];
                std::size_t transformNodeIndex = nodeIndex;
                if (sourceMesh.weightedBinding.has_value()) {
                    transformNodeIndex = sourceMesh.weightedBinding->rootNodeIndex;
                    if (transformNodeIndex >= nodeWorld.size()) {
                        appendWarning(out.diagnostics, regionLabel(target.kind) + " entry=" +
                            std::to_string(instance->sourceEntryId) +
                            " mesh=" + sourceMesh.label + " has an invalid weighted root node.");
                        continue;
                    }
                }

                auto mesh = convertMesh(sourceMesh,
                    multiply(entryWorld, nodeWorld[transformNodeIndex]),
                    target.kind,
                    instance->sourceEntryId,
                    out.diagnostics);
                if (mesh.vertices.empty() || mesh.indices.empty()) {
                    continue;
                }
                region.meshes.push_back(NavigationRegionMesh{
                    .sourceObjectAddress = sourceMesh.sourceObjectAddress != 0U
                        ? sourceMesh.sourceObjectAddress
                        : tree.sourceObjectAddress,
                    .sourceChunkOffset = sourceMesh.sourceChunkOffset,
                    .sourceNodeOffset = node.sourceNodeOffset,
                    .sourceAttachOffset = sourceMesh.sourceAttachOffset != 0U
                        ? sourceMesh.sourceAttachOffset
                        : node.sourceAttachOffset,
                    .mesh = std::move(mesh),
                });
            }
        }

        region.complete = !region.meshes.empty();
        if (!region.complete) {
            appendWarning(out.diagnostics, regionLabel(target.kind) + " entry=" +
                std::to_string(instance->sourceEntryId) + " tbl=" + std::to_string(instance->tblId) +
                " produced no usable projected mesh.");
        }
        out.regions.push_back(std::move(region));
    }
    return out;
}

} // namespace savor::navigation
