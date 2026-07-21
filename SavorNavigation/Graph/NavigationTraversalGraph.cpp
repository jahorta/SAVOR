#include "NavigationTraversalGraph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace savor::navigation {
namespace {

constexpr float kComparisonEpsilon = 1.0e-6F;

[[nodiscard]] NavigationVec3 add(const NavigationVec3& lhs, const NavigationVec3& rhs) noexcept {
    return NavigationVec3{ lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z };
}

[[nodiscard]] NavigationVec3 subtract(const NavigationVec3& lhs, const NavigationVec3& rhs) noexcept {
    return NavigationVec3{ lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z };
}

[[nodiscard]] NavigationVec3 multiply(const NavigationVec3& value, const float scalar) noexcept {
    return NavigationVec3{ value.x * scalar, value.y * scalar, value.z * scalar };
}

[[nodiscard]] float dot(const NavigationVec3& lhs, const NavigationVec3& rhs) noexcept {
    return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z);
}

[[nodiscard]] NavigationVec3 cross(const NavigationVec3& lhs, const NavigationVec3& rhs) noexcept {
    return NavigationVec3{
        (lhs.y * rhs.z) - (lhs.z * rhs.y),
        (lhs.z * rhs.x) - (lhs.x * rhs.z),
        (lhs.x * rhs.y) - (lhs.y * rhs.x),
    };
}

[[nodiscard]] float lengthSquared(const NavigationVec3& value) noexcept {
    return dot(value, value);
}

[[nodiscard]] float length(const NavigationVec3& value) noexcept {
    return std::sqrt(lengthSquared(value));
}

[[nodiscard]] NavigationVec3 normalized(const NavigationVec3& value) noexcept {
    const float valueLength = length(value);
    if (valueLength <= kComparisonEpsilon) {
        return NavigationVec3{};
    }
    return multiply(value, 1.0F / valueLength);
}

[[nodiscard]] bool isFinite(const NavigationVec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

void appendDiagnostic(
    NavigationTraversalGraph& graph,
    const NavigationDiagnosticSeverity severity,
    std::string message) {
    graph.diagnostics.push_back(NavigationDiagnostic{
        .severity = severity,
        .message = std::move(message),
    });
}

struct QuantizedCell {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;

    [[nodiscard]] bool operator==(const QuantizedCell&) const = default;
};

struct QuantizedCellHash {
    [[nodiscard]] std::size_t operator()(const QuantizedCell& value) const noexcept {
        std::size_t seed = std::hash<std::int64_t>{}(value.x);
        seed ^= std::hash<std::int64_t>{}(value.y) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        seed ^= std::hash<std::int64_t>{}(value.z) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

class VertexWelder final {
public:
    explicit VertexWelder(const float tolerance)
        : tolerance_(std::max(tolerance, kComparisonEpsilon)),
          toleranceSquared_(tolerance_ * tolerance_) {
    }

    [[nodiscard]] std::size_t canonicalIndex(const NavigationVec3& value) {
        const QuantizedCell home = cellFor(value);
        std::optional<std::size_t> best{};
        for (std::int64_t deltaX = -1; deltaX <= 1; ++deltaX) {
            for (std::int64_t deltaY = -1; deltaY <= 1; ++deltaY) {
                for (std::int64_t deltaZ = -1; deltaZ <= 1; ++deltaZ) {
                    const QuantizedCell neighbor{
                        home.x + deltaX,
                        home.y + deltaY,
                        home.z + deltaZ,
                    };
                    const auto found = members_.find(neighbor);
                    if (found == members_.end()) {
                        continue;
                    }
                    for (const std::size_t candidate : found->second) {
                        if (lengthSquared(subtract(canonicalPositions_[candidate], value)) <= toleranceSquared_ &&
                            (!best.has_value() || candidate < *best)) {
                            best = candidate;
                        }
                    }
                }
            }
        }
        if (best.has_value()) {
            return *best;
        }

        const std::size_t result = canonicalPositions_.size();
        canonicalPositions_.push_back(value);
        members_[home].push_back(result);
        return result;
    }

private:
    [[nodiscard]] QuantizedCell cellFor(const NavigationVec3& value) const noexcept {
        return QuantizedCell{
            static_cast<std::int64_t>(std::floor(value.x / tolerance_)),
            static_cast<std::int64_t>(std::floor(value.y / tolerance_)),
            static_cast<std::int64_t>(std::floor(value.z / tolerance_)),
        };
    }

    float tolerance_ = 0.0F;
    float toleranceSquared_ = 0.0F;
    std::vector<NavigationVec3> canonicalPositions_{};
    std::unordered_map<QuantizedCell, std::vector<std::size_t>, QuantizedCellHash> members_{};
};

struct CanonicalEdge {
    std::size_t first = 0;
    std::size_t second = 0;

    [[nodiscard]] bool operator==(const CanonicalEdge&) const = default;
};

struct CanonicalEdgeHash {
    [[nodiscard]] std::size_t operator()(const CanonicalEdge& value) const noexcept {
        std::size_t seed = std::hash<std::size_t>{}(value.first);
        seed ^= std::hash<std::size_t>{}(value.second) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

struct CanonicalTriangle {
    std::array<std::size_t, 3> vertices{};

    [[nodiscard]] bool operator==(const CanonicalTriangle&) const = default;
};

struct CanonicalTriangleHash {
    [[nodiscard]] std::size_t operator()(const CanonicalTriangle& value) const noexcept {
        std::size_t seed = 0;
        for (const std::size_t vertex : value.vertices) {
            seed ^= std::hash<std::size_t>{}(vertex) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        }
        return seed;
    }
};

struct EdgeOccurrence {
    std::size_t nodeIndex = 0;
    NavigationVec3 first{};
    NavigationVec3 second{};
};

struct BoundarySegment {
    std::size_t nodeIndex = 0;
    NavigationVec3 first{};
    NavigationVec3 second{};
};

[[nodiscard]] bool nearlyEqual(const NavigationVec3& lhs, const NavigationVec3& rhs) noexcept {
    return lengthSquared(subtract(lhs, rhs)) <= (kComparisonEpsilon * kComparisonEpsilon);
}

[[nodiscard]] bool addEdgeIfUnique(NavigationGraphNode& node, NavigationGraphEdge edge) {
    const auto duplicate = std::find_if(node.edges.begin(), node.edges.end(), [&](const NavigationGraphEdge& existing) {
        if (existing.targetNodeIndex != edge.targetNodeIndex || existing.kind != edge.kind ||
            existing.groundLinkIndex != edge.groundLinkIndex) {
            return false;
        }
        const bool sameOrder = nearlyEqual(existing.portal.first, edge.portal.first) &&
            nearlyEqual(existing.portal.second, edge.portal.second);
        const bool reverseOrder = nearlyEqual(existing.portal.first, edge.portal.second) &&
            nearlyEqual(existing.portal.second, edge.portal.first);
        return sameOrder || reverseOrder;
    });
    if (duplicate != node.edges.end()) {
        return false;
    }
    node.edges.push_back(std::move(edge));
    return true;
}

[[nodiscard]] std::optional<NavigationPortalSegment> overlappingPortal(
    const BoundarySegment& source,
    const BoundarySegment& target,
    const NavigationGraphBuildOptions& options) noexcept {
    const NavigationVec3 sourceVector = subtract(source.second, source.first);
    const NavigationVec3 targetVector = subtract(target.second, target.first);
    const float sourceLength = length(sourceVector);
    const float targetLength = length(targetVector);
    if (sourceLength <= options.minimumPortalLength || targetLength <= options.minimumPortalLength) {
        return std::nullopt;
    }

    const NavigationVec3 sourceDirection = multiply(sourceVector, 1.0F / sourceLength);
    const NavigationVec3 targetDirection = multiply(targetVector, 1.0F / targetLength);
    const float alignment = std::abs(dot(sourceDirection, targetDirection));
    if ((1.0F - alignment) > options.parallelDirectionTolerance) {
        return std::nullopt;
    }

    const float targetFirstSeparation = length(cross(subtract(target.first, source.first), sourceDirection));
    const float targetSecondSeparation = length(cross(subtract(target.second, source.first), sourceDirection));
    if (targetFirstSeparation > options.portalSeparationTolerance ||
        targetSecondSeparation > options.portalSeparationTolerance) {
        return std::nullopt;
    }

    const float targetFirstOnSource = dot(subtract(target.first, source.first), sourceDirection) / sourceLength;
    const float targetSecondOnSource = dot(subtract(target.second, source.first), sourceDirection) / sourceLength;
    const float overlapFirst = std::max(0.0F, std::min(targetFirstOnSource, targetSecondOnSource));
    const float overlapSecond = std::min(1.0F, std::max(targetFirstOnSource, targetSecondOnSource));
    if (((overlapSecond - overlapFirst) * sourceLength) < options.minimumPortalLength) {
        return std::nullopt;
    }

    const auto averageWithTarget = [&](const NavigationVec3& sourcePoint) {
        const float targetParameter = std::clamp(
            dot(subtract(sourcePoint, target.first), targetVector) / lengthSquared(targetVector),
            0.0F,
            1.0F);
        const NavigationVec3 targetPoint = add(target.first, multiply(targetVector, targetParameter));
        return multiply(add(sourcePoint, targetPoint), 0.5F);
    };

    const NavigationVec3 sourceFirst = add(source.first, multiply(sourceVector, overlapFirst));
    const NavigationVec3 sourceSecond = add(source.first, multiply(sourceVector, overlapSecond));
    return NavigationPortalSegment{
        .first = averageWithTarget(sourceFirst),
        .second = averageWithTarget(sourceSecond),
    };
}

[[nodiscard]] std::vector<std::size_t> findSurfaceIndices(
    const NavigationAreaModel& area,
    const NavigationSurfaceSourceKey& key) {
    std::vector<std::size_t> result{};
    for (std::size_t surfaceIndex = 0; surfaceIndex < area.surfaces.size(); ++surfaceIndex) {
        if (area.surfaces[surfaceIndex].sourceKey == key) {
            result.push_back(surfaceIndex);
        }
    }
    return result;
}

[[nodiscard]] auto edgeSortKey(const NavigationGraphEdge& edge) noexcept {
    return std::tuple{
        edge.targetNodeIndex,
        static_cast<int>(edge.kind),
        edge.groundLinkIndex.value_or(std::numeric_limits<std::size_t>::max()),
        edge.portal.first.x,
        edge.portal.first.y,
        edge.portal.first.z,
        edge.portal.second.x,
        edge.portal.second.y,
        edge.portal.second.z,
    };
}

} // namespace

NavigationVec3 NavigationPortalSegment::midpoint() const noexcept {
    return multiply(add(first, second), 0.5F);
}

bool NavigationTraversalGraph::isPathfindingReady() const noexcept {
    return hasCompleteGroundGeometry && hasCompleteWallGeometry && !nodes.empty();
}

std::optional<std::size_t> NavigationTraversalGraph::findNodeIndex(
    const NavigationTriangleKey& key) const noexcept {
    const auto found = std::lower_bound(nodes.begin(), nodes.end(), key, [](const NavigationGraphNode& node,
        const NavigationTriangleKey& candidate) {
        return node.key < candidate;
    });
    if (found == nodes.end() || found->key != key) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(nodes.begin(), found));
}

NavigationTraversalGraph NavigationGraphBuilder::build(
    const NavigationAreaModel& area,
    const NavigationCoordinatePolicy coordinatePolicy,
    const NavigationGraphBuildOptions& options) const {
    NavigationTraversalGraph graph{};
    graph.statistics.sourceSurfaceCount = area.surfaces.size();
    graph.hasCompleteGroundGeometry = area.hasCompleteGroundGeometry;
    graph.hasCompleteWallGeometry = area.hasCompleteWallGeometry;

    NavigationGraphBuildOptions effectiveOptions = options;
    if (effectiveOptions.vertexWeldTolerance <= 0.0F) {
        effectiveOptions.vertexWeldTolerance = 1.0e-3F;
        appendDiagnostic(graph, NavigationDiagnosticSeverity::Warning,
            "Graph vertex weld tolerance was non-positive; 0.001 was used.");
    }
    effectiveOptions.degenerateTriangleEpsilon =
        std::max(effectiveOptions.degenerateTriangleEpsilon, kComparisonEpsilon);
    effectiveOptions.portalSeparationTolerance =
        std::max(effectiveOptions.portalSeparationTolerance, 0.0F);
    effectiveOptions.minimumPortalLength =
        std::max(effectiveOptions.minimumPortalLength, kComparisonEpsilon);
    effectiveOptions.parallelDirectionTolerance =
        std::clamp(effectiveOptions.parallelDirectionTolerance, 0.0F, 1.0F);

    std::vector<std::vector<BoundarySegment>> boundariesBySurface(area.surfaces.size());
    for (std::size_t surfaceIndex = 0; surfaceIndex < area.surfaces.size(); ++surfaceIndex) {
        const NavigationSurface& surface = area.surfaces[surfaceIndex];
        const NavigationMesh& mesh = surface.mesh;
        const std::size_t triangleCount = mesh.indices.size() / 3U;
        graph.statistics.sourceTriangleCount += triangleCount;
        if ((mesh.indices.size() % 3U) != 0U) {
            appendDiagnostic(graph, NavigationDiagnosticSeverity::Warning,
                "Surface " + std::to_string(surfaceIndex) +
                " has an index count that is not divisible by three; trailing indices were ignored.");
        }

        std::vector<NavigationVec3> positions{};
        positions.reserve(mesh.vertices.size());
        VertexWelder welder(effectiveOptions.vertexWeldTolerance);
        std::vector<std::size_t> canonicalVertices{};
        canonicalVertices.reserve(mesh.vertices.size());
        for (const NavigationMeshVertex& vertex : mesh.vertices) {
            const NavigationVec3 position = coordinatePolicy.convertPosition(vertex.position);
            positions.push_back(position);
            canonicalVertices.push_back(isFinite(position)
                ? welder.canonicalIndex(position)
                : std::numeric_limits<std::size_t>::max());
        }

        std::unordered_map<CanonicalEdge, std::vector<EdgeOccurrence>, CanonicalEdgeHash> edgeOccurrences{};
        std::unordered_set<CanonicalTriangle, CanonicalTriangleHash> canonicalTriangles{};
        const std::size_t surfaceNodeStart = graph.nodes.size();
        for (std::size_t triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex) {
            const std::array<std::uint32_t, 3> indices{
                mesh.indices[(triangleIndex * 3U) + 0U],
                mesh.indices[(triangleIndex * 3U) + 1U],
                mesh.indices[(triangleIndex * 3U) + 2U],
            };
            if (indices[0] >= positions.size() || indices[1] >= positions.size() || indices[2] >= positions.size()) {
                ++graph.statistics.skippedInvalidTriangleCount;
                appendDiagnostic(graph, NavigationDiagnosticSeverity::Warning,
                    "Surface " + std::to_string(surfaceIndex) + " triangle " +
                    std::to_string(triangleIndex) + " has an invalid vertex index and was omitted.");
                continue;
            }

            const std::array<NavigationVec3, 3> triangle{
                positions[indices[0]],
                positions[indices[1]],
                positions[indices[2]],
            };
            if (!isFinite(triangle[0]) || !isFinite(triangle[1]) || !isFinite(triangle[2])) {
                ++graph.statistics.skippedInvalidTriangleCount;
                appendDiagnostic(graph, NavigationDiagnosticSeverity::Warning,
                    "Surface " + std::to_string(surfaceIndex) + " triangle " +
                    std::to_string(triangleIndex) + " contains a non-finite position and was omitted.");
                continue;
            }

            const NavigationVec3 triangleCross = cross(
                subtract(triangle[1], triangle[0]),
                subtract(triangle[2], triangle[0]));
            if (length(triangleCross) <= effectiveOptions.degenerateTriangleEpsilon) {
                ++graph.statistics.skippedDegenerateTriangleCount;
                appendDiagnostic(graph, NavigationDiagnosticSeverity::Warning,
                    "Surface " + std::to_string(surfaceIndex) + " triangle " +
                    std::to_string(triangleIndex) + " is degenerate and was omitted.");
                continue;
            }

            CanonicalTriangle canonicalTriangle{
                .vertices = {
                    canonicalVertices[indices[0]],
                    canonicalVertices[indices[1]],
                    canonicalVertices[indices[2]],
                },
            };
            std::sort(canonicalTriangle.vertices.begin(), canonicalTriangle.vertices.end());
            if (!canonicalTriangles.emplace(canonicalTriangle).second) {
                ++graph.statistics.skippedDuplicateTriangleCount;
                appendDiagnostic(graph, NavigationDiagnosticSeverity::Warning,
                    "Surface " + std::to_string(surfaceIndex) + " triangle " +
                    std::to_string(triangleIndex) + " duplicates another triangle and was omitted.");
                continue;
            }

            const std::size_t nodeIndex = graph.nodes.size();
            graph.nodes.push_back(NavigationGraphNode{
                .key = NavigationTriangleKey{ surfaceIndex, triangleIndex },
                .vertices = triangle,
                .centroid = multiply(add(add(triangle[0], triangle[1]), triangle[2]), 1.0F / 3.0F),
                .normal = normalized(coordinatePolicy.convertDirection(triangleCross)),
            });

            for (std::size_t edgeIndex = 0; edgeIndex < 3U; ++edgeIndex) {
                const std::size_t nextEdgeIndex = (edgeIndex + 1U) % 3U;
                const std::size_t firstCanonical = canonicalVertices[indices[edgeIndex]];
                const std::size_t secondCanonical = canonicalVertices[indices[nextEdgeIndex]];
                const bool canonicalOrder = firstCanonical < secondCanonical;
                const CanonicalEdge edge{
                    .first = std::min(firstCanonical, secondCanonical),
                    .second = std::max(firstCanonical, secondCanonical),
                };
                edgeOccurrences[edge].push_back(EdgeOccurrence{
                    .nodeIndex = nodeIndex,
                    .first = canonicalOrder ? triangle[edgeIndex] : triangle[nextEdgeIndex],
                    .second = canonicalOrder ? triangle[nextEdgeIndex] : triangle[edgeIndex],
                });
            }
        }

        std::unordered_set<std::size_t> nonManifoldNodes{};
        for (const auto& [edge, occurrences] : edgeOccurrences) {
            static_cast<void>(edge);
            if (occurrences.size() <= 2U) {
                continue;
            }
            ++graph.statistics.nonManifoldEdgeCount;
            for (const EdgeOccurrence& occurrence : occurrences) {
                nonManifoldNodes.emplace(occurrence.nodeIndex);
            }
            appendDiagnostic(graph, NavigationDiagnosticSeverity::Warning,
                "Surface " + std::to_string(surfaceIndex) +
                " contains a non-manifold edge shared by " + std::to_string(occurrences.size()) +
                " triangles; the incident triangles were omitted.");
        }
        if (!nonManifoldNodes.empty()) {
            const std::size_t originalSurfaceNodeCount = graph.nodes.size() - surfaceNodeStart;
            std::vector<std::size_t> remappedNodeIndices(
                originalSurfaceNodeCount,
                std::numeric_limits<std::size_t>::max());
            std::vector<NavigationGraphNode> retainedNodes{};
            retainedNodes.reserve(originalSurfaceNodeCount - nonManifoldNodes.size());
            for (std::size_t oldNodeIndex = surfaceNodeStart; oldNodeIndex < graph.nodes.size(); ++oldNodeIndex) {
                if (nonManifoldNodes.contains(oldNodeIndex)) {
                    continue;
                }
                const std::size_t newNodeIndex = surfaceNodeStart + retainedNodes.size();
                remappedNodeIndices[oldNodeIndex - surfaceNodeStart] = newNodeIndex;
                retainedNodes.push_back(std::move(graph.nodes[oldNodeIndex]));
            }
            graph.nodes.resize(surfaceNodeStart);
            for (NavigationGraphNode& node : retainedNodes) {
                graph.nodes.push_back(std::move(node));
            }
            graph.statistics.skippedNonManifoldTriangleCount += nonManifoldNodes.size();

            for (auto& [edge, occurrences] : edgeOccurrences) {
                static_cast<void>(edge);
                std::erase_if(occurrences, [&](EdgeOccurrence& occurrence) {
                    if (nonManifoldNodes.contains(occurrence.nodeIndex)) {
                        return true;
                    }
                    occurrence.nodeIndex = remappedNodeIndices[occurrence.nodeIndex - surfaceNodeStart];
                    return false;
                });
            }
        }

        for (const auto& [edge, occurrences] : edgeOccurrences) {
            static_cast<void>(edge);
            if (occurrences.empty()) {
                continue;
            }
            if (occurrences.size() == 1U) {
                boundariesBySurface[surfaceIndex].push_back(BoundarySegment{
                    .nodeIndex = occurrences[0].nodeIndex,
                    .first = occurrences[0].first,
                    .second = occurrences[0].second,
                });
                continue;
            }
            if (occurrences.size() > 2U) {
                // All incident triangles are removed above. Retain this guard so
                // malformed future topology adapters cannot create adjacency.
                continue;
            }

            const NavigationPortalSegment portal{
                .first = multiply(add(occurrences[0].first, occurrences[1].first), 0.5F),
                .second = multiply(add(occurrences[0].second, occurrences[1].second), 0.5F),
            };
            const bool forwardAdded = addEdgeIfUnique(graph.nodes[occurrences[0].nodeIndex], NavigationGraphEdge{
                .targetNodeIndex = occurrences[1].nodeIndex,
                .portal = portal,
                .kind = NavigationGraphEdgeKind::IntraSurface,
            });
            const bool reverseAdded = addEdgeIfUnique(graph.nodes[occurrences[1].nodeIndex], NavigationGraphEdge{
                .targetNodeIndex = occurrences[0].nodeIndex,
                .portal = portal,
                .kind = NavigationGraphEdgeKind::IntraSurface,
            });
            if (forwardAdded || reverseAdded) {
                ++graph.statistics.intraSurfaceConnectionCount;
            }
        }
    }

    for (std::size_t linkIndex = 0; linkIndex < area.groundLinks.size(); ++linkIndex) {
        const NavigationGroundLink& link = area.groundLinks[linkIndex];
        std::set<std::pair<std::size_t, std::size_t>> candidateSurfacePairs{};
        for (const NavigationSurfaceSourceKey& sourceKey : link.sourceSurfaces) {
            const auto sourceIndices = findSurfaceIndices(area, sourceKey);
            for (const NavigationSurfaceSourceKey& targetKey : link.targetSurfaces) {
                const auto targetIndices = findSurfaceIndices(area, targetKey);
                for (const std::size_t sourceIndex : sourceIndices) {
                    for (const std::size_t targetIndex : targetIndices) {
                        if (sourceIndex != targetIndex) {
                            candidateSurfacePairs.emplace(sourceIndex, targetIndex);
                        }
                    }
                }
            }
        }

        std::set<std::pair<std::size_t, std::size_t>> matchedSurfacePairs{};
        std::size_t addedPortalCount = 0;
        for (const auto& [sourceSurfaceIndex, targetSurfaceIndex] : candidateSurfacePairs) {
            for (const BoundarySegment& sourceBoundary : boundariesBySurface[sourceSurfaceIndex]) {
                for (const BoundarySegment& targetBoundary : boundariesBySurface[targetSurfaceIndex]) {
                    const auto portal = overlappingPortal(sourceBoundary, targetBoundary, effectiveOptions);
                    if (!portal.has_value()) {
                        continue;
                    }
                    if (addEdgeIfUnique(graph.nodes[sourceBoundary.nodeIndex], NavigationGraphEdge{
                        .targetNodeIndex = targetBoundary.nodeIndex,
                        .portal = *portal,
                        .kind = NavigationGraphEdgeKind::GroundLink,
                        .groundLinkIndex = linkIndex,
                    })) {
                        ++graph.statistics.groundLinkPortalCount;
                        ++addedPortalCount;
                        matchedSurfacePairs.emplace(sourceSurfaceIndex, targetSurfaceIndex);
                    }
                }
            }
        }

        if (addedPortalCount == 0U) {
            ++graph.statistics.unresolvedGroundLinkCount;
            appendDiagnostic(graph, NavigationDiagnosticSeverity::Warning,
                "Ground link entry=" + std::to_string(link.sourceEntryId) + " -> " +
                std::to_string(link.targetEntryId) +
                " has no geometrically overlapping boundary and was not connected.");
        } else if (link.resolution == NavigationGroundLinkResolution::Ambiguous &&
            matchedSurfacePairs.size() > 1U) {
            appendDiagnostic(graph, NavigationDiagnosticSeverity::Warning,
                "Ambiguous ground link entry=" + std::to_string(link.sourceEntryId) + " -> " +
                std::to_string(link.targetEntryId) + " matched " +
                std::to_string(matchedSurfacePairs.size()) +
                " surface pairs; all geometrically validated portals were retained.");
        }
    }

    for (NavigationGraphNode& node : graph.nodes) {
        std::sort(node.edges.begin(), node.edges.end(), [](const NavigationGraphEdge& lhs,
            const NavigationGraphEdge& rhs) {
            return edgeSortKey(lhs) < edgeSortKey(rhs);
        });
    }
    return graph;
}

} // namespace savor::navigation
