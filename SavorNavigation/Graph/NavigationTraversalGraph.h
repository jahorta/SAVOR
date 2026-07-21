#pragma once

#include "../Coordinate/NavigationCoordinatePolicy.h"
#include "../Model/NavigationAreaModel.h"

#include <array>
#include <compare>
#include <cstddef>
#include <optional>
#include <vector>

namespace savor::navigation {

struct NavigationTriangleKey {
    std::size_t surfaceIndex = 0;
    std::size_t triangleIndex = 0;

    [[nodiscard]] auto operator<=>(const NavigationTriangleKey&) const = default;
};

struct NavigationPortalSegment {
    NavigationVec3 first{};
    NavigationVec3 second{};

    [[nodiscard]] NavigationVec3 midpoint() const noexcept;
};

enum class NavigationGraphEdgeKind {
    IntraSurface,
    GroundLink,
};

struct NavigationGraphEdge {
    std::size_t targetNodeIndex = 0;
    NavigationPortalSegment portal{};
    NavigationGraphEdgeKind kind = NavigationGraphEdgeKind::IntraSurface;
    std::optional<std::size_t> groundLinkIndex{};
};

struct NavigationGraphNode {
    NavigationTriangleKey key{};
    std::array<NavigationVec3, 3> vertices{};
    NavigationVec3 centroid{};
    NavigationVec3 normal{ 0.0F, 1.0F, 0.0F };
    std::vector<NavigationGraphEdge> edges{};
};

struct NavigationTraversalGraphStatistics {
    std::size_t sourceSurfaceCount = 0;
    std::size_t sourceTriangleCount = 0;
    std::size_t skippedInvalidTriangleCount = 0;
    std::size_t skippedDegenerateTriangleCount = 0;
    std::size_t skippedDuplicateTriangleCount = 0;
    std::size_t skippedNonManifoldTriangleCount = 0;
    std::size_t nonManifoldEdgeCount = 0;
    std::size_t intraSurfaceConnectionCount = 0;
    std::size_t groundLinkPortalCount = 0;
    std::size_t unresolvedGroundLinkCount = 0;
};

struct NavigationTraversalGraph {
    std::vector<NavigationGraphNode> nodes{};
    std::vector<NavigationDiagnostic> diagnostics{};
    NavigationTraversalGraphStatistics statistics{};
    bool hasCompleteGroundGeometry = false;
    bool hasCompleteWallGeometry = false;

    [[nodiscard]] bool isPathfindingReady() const noexcept;
    [[nodiscard]] std::optional<std::size_t> findNodeIndex(const NavigationTriangleKey& key) const noexcept;
};

struct NavigationGraphBuildOptions {
    // Vertices within this distance on the same surface are treated as one
    // topological vertex. This joins meshes that duplicate shared-edge vertices.
    float vertexWeldTolerance = 1.0e-3F;
    // Minimum magnitude of a triangle cross product (twice its area).
    float degenerateTriangleEpsilon = 1.0e-6F;
    // Maximum separation between otherwise collinear cross-surface boundaries.
    float portalSeparationTolerance = 1.0e-2F;
    // Minimum retained overlap length for a cross-surface portal.
    float minimumPortalLength = 1.0e-3F;
    // Maximum 1-|dot(directionA,directionB)| for parallel boundaries.
    float parallelDirectionTolerance = 1.0e-3F;
};

class NavigationGraphBuilder final {
public:
    [[nodiscard]] NavigationTraversalGraph build(
        const NavigationAreaModel& area,
        NavigationCoordinatePolicy coordinatePolicy = NavigationCoordinatePolicy::identity(),
        const NavigationGraphBuildOptions& options = {}) const;
};

} // namespace savor::navigation
