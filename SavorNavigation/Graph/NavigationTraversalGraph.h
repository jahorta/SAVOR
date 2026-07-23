#pragma once

#include "../Coordinate/NavigationCoordinatePolicy.h"
#include "../Model/NavigationAreaModel.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
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
    CollisionHandoff,
};

enum class NavigationCollisionHandoffKind {
    SameEntryBundle,
    AuthoredFallback,
};

enum class NavigationCollisionHandoffAvailability {
    ActiveStatic,
    RequiresRuntimeState,
};

struct NavigationCollisionHandoff {
    NavigationTriangleKey sourceTriangle{};
    NavigationTriangleKey targetTriangle{};
    NavigationPortalSegment portal{};
    NavigationCollisionHandoffKind kind = NavigationCollisionHandoffKind::SameEntryBundle;
    NavigationCollisionHandoffAvailability availability =
        NavigationCollisionHandoffAvailability::ActiveStatic;
    std::uint32_t sourceEntryId = 0;
    std::uint32_t targetEntryId = 0;
    std::optional<std::size_t> authoredFallbackChainIndex{};
    std::optional<std::size_t> authoredFallbackTargetIndex{};
    std::optional<std::size_t> authoredOrdinal{};
};

struct NavigationGraphEdge {
    std::size_t targetNodeIndex = 0;
    NavigationPortalSegment portal{};
    NavigationGraphEdgeKind kind = NavigationGraphEdgeKind::IntraSurface;
    std::optional<std::size_t> collisionHandoffIndex{};
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
    std::size_t authoredFallbackChainCount = 0;
    std::size_t authoredFallbackTargetCount = 0;
    std::size_t sameEntryHandoffCount = 0;
    std::size_t authoredFallbackHandoffCount = 0;
    std::size_t conditionalHandoffCount = 0;
    std::size_t unresolvedHandoffCount = 0;
    std::size_t runtimeDependentSurfaceCount = 0;
    std::size_t runtimeStateBlockedIntervalCount = 0;
    std::size_t priorityShadowedCandidateCount = 0;
};

struct NavigationTraversalGraph {
    std::vector<NavigationGraphNode> nodes{};
    std::vector<NavigationCollisionHandoff> collisionHandoffs{};
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
    // Planar point-in-triangle tolerance used by collision coverage queries.
    float planarContainmentTolerance = 1.0e-3F;
    // Distance to probe beyond a source triangle boundary, capped to one
    // quarter of the candidate interval length.
    float outwardProbeDistance = 1.0e-2F;
    // Maximum source/target height difference at both ends of a handoff.
    float heightContinuityTolerance = 1.0e-2F;
    // Height separation that distinguishes tied stacked collision hits.
    float distinctHeightTieTolerance = 1.0e-3F;
    // Minimum retained interval length for a collision handoff.
    float minimumPortalLength = 1.0e-3F;
    // Minimum absolute triangle-plane/up-axis dot product for height solving.
    float minimumUpNormalComponent = 1.0e-4F;
};

class NavigationGraphBuilder final {
public:
    [[nodiscard]] NavigationTraversalGraph build(
        const NavigationAreaModel& area,
        NavigationCoordinatePolicy coordinatePolicy = NavigationCoordinatePolicy::identity(),
        const NavigationGraphBuildOptions& options = {}) const;
};

} // namespace savor::navigation
