#pragma once

#include "../Coordinate/NavigationCoordinatePolicy.h"
#include "../Graph/NavigationTraversalGraph.h"
#include "../Model/NavigationScriptModel.h"

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace savor::navigation {

struct NavigationGraphAnchor {
    NavigationTriangleKey triangle{};
    std::size_t nodeIndex = 0;
    NavigationVec3 exactPoint{};
    NavigationVec3 snappedPoint{};
    float snapDistance = 0.0F;
};

enum class NavigationGraphAnchorStatus {
    Anchored,
    SurfaceNotFound,
    NoWalkableTriangle,
    TooFar,
};

struct NavigationGraphAnchorResult {
    NavigationGraphAnchorStatus status = NavigationGraphAnchorStatus::NoWalkableTriangle;
    std::optional<NavigationGraphAnchor> anchor{};
    std::string message{};

    [[nodiscard]] bool hasAnchor() const noexcept {
        return status == NavigationGraphAnchorStatus::Anchored && anchor.has_value();
    }
};

class NavigationGraphAnchorer final {
public:
    [[nodiscard]] NavigationGraphAnchorResult anchorToSurface(
        const NavigationTraversalGraph& graph,
        std::size_t surfaceIndex,
        const NavigationVec3& exactPoint,
        float maximumSnapDistance = std::numeric_limits<float>::infinity()) const;
};

enum class NavigationStartResolveStatus {
    Resolved,
    MissingCandidate,
    IncompletePlacement,
    MissingGround,
    IncompleteGraph,
    AmbiguousSurface,
    AnchoringFailure,
};

struct NavigationStartResolveResult {
    NavigationStartResolveStatus status = NavigationStartResolveStatus::MissingCandidate;
    std::optional<NavigationGraphAnchor> anchor{};
    std::optional<float> yawDegrees{};
    std::string message{};

    [[nodiscard]] bool hasAnchor() const noexcept {
        return status == NavigationStartResolveStatus::Resolved && anchor.has_value();
    }
};

struct NavigationStartResolveOptions {
    // Distances this close are treated as equivalent. A tie across distinct
    // source surfaces is ambiguous instead of being resolved by container order.
    float surfaceDistanceTieTolerance = 1.0e-4F;
    // Equidistant surfaces that snap to the same world-space point are treated
    // as duplicate/seam geometry and resolved in stable surface order.
    float snappedPointCoincidenceTolerance = 1.0e-4F;
};

class NavigationStartResolver final {
public:
    [[nodiscard]] NavigationStartResolveResult resolve(
        const NavigationAreaModel& area,
        const NavigationTraversalGraph& graph,
        const NavigationStartOption* candidate,
        NavigationCoordinatePolicy coordinatePolicy = NavigationCoordinatePolicy::identity(),
        const NavigationStartResolveOptions& options = {}) const;

    [[nodiscard]] NavigationStartResolveResult resolve(
        const NavigationAreaModel& area,
        const NavigationTraversalGraph& graph,
        const NavigationStartOption& candidate,
        NavigationCoordinatePolicy coordinatePolicy = NavigationCoordinatePolicy::identity(),
        const NavigationStartResolveOptions& options = {}) const {
        return resolve(area, graph, &candidate, coordinatePolicy, options);
    }
};

enum class NavigationTriggerGoalResolveStatus {
    Resolved,
    RegionNotFound,
    NotTrigger,
    MissingGeometry,
    IncompleteGraph,
    AmbiguousSurface,
    AnchoringFailure,
};

struct NavigationTriggerGoalTarget {
    std::size_t regionIndex = 0;
    std::size_t primaryMeshIndex = 0;
    NavigationBounds displayBounds{};
    NavigationGraphAnchor anchor{};
    // The primary trigger mesh is selected by this world-space AABB metric.
    // Squared diagonal is compared first so planar collision meshes retain
    // their spatial size; referenced triangle count and mesh index break ties.
    float primaryMeshAabbSquaredDiagonal = 0.0F;
    std::size_t primaryMeshReferencedTriangleCount = 0;
};

struct NavigationTriggerGoalResolveResult {
    NavigationTriggerGoalResolveStatus status = NavigationTriggerGoalResolveStatus::RegionNotFound;
    std::optional<NavigationTriggerGoalTarget> target{};
    std::string message{};

    [[nodiscard]] bool hasTarget() const noexcept {
        return status == NavigationTriggerGoalResolveStatus::Resolved && target.has_value();
    }
};

struct NavigationTriggerGoalResolveOptions {
    float surfaceDistanceTieTolerance = 1.0e-4F;
    float snappedPointCoincidenceTolerance = 1.0e-4F;
};

class NavigationTriggerGoalResolver final {
public:
    [[nodiscard]] NavigationTriggerGoalResolveResult resolve(
        const NavigationAreaModel& area,
        const NavigationTraversalGraph& graph,
        std::size_t regionIndex,
        const NavigationTriggerGoalResolveOptions& options = {}) const;
};

struct NavigationPathQuery {
    NavigationGraphAnchor start{};
    NavigationGraphAnchor goal{};
    float slopeCostMultiplier = 0.25F;
    bool requireCompleteGeometry = true;
};

enum class NavigationPathStatus {
    Success,
    IncompleteGeometry,
    InvalidStart,
    InvalidGoal,
    Unreachable,
};

struct NavigationPathResult {
    NavigationPathStatus status = NavigationPathStatus::Unreachable;
    std::vector<NavigationTriangleKey> trianglePath{};
    std::vector<NavigationVec3> polyline{};
    float routeLength = 0.0F;
    float totalCost = 0.0F;
    std::string message{};

    [[nodiscard]] bool hasPath() const noexcept {
        return status == NavigationPathStatus::Success;
    }
};

class NavigationPathfinder final {
public:
    [[nodiscard]] NavigationPathResult findPath(
        const NavigationTraversalGraph& graph,
        const NavigationPathQuery& query,
        NavigationCoordinatePolicy coordinatePolicy = NavigationCoordinatePolicy::identity()) const;
};

} // namespace savor::navigation
