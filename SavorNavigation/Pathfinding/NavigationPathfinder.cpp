#include "NavigationPathfinder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>

namespace savor::navigation {
namespace {

constexpr float kPathComparisonEpsilon = 1.0e-6F;

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

[[nodiscard]] float distance(const NavigationVec3& lhs, const NavigationVec3& rhs) noexcept {
    return std::sqrt(lengthSquared(subtract(lhs, rhs)));
}

[[nodiscard]] NavigationVec3 normalized(const NavigationVec3& value) noexcept {
    const float valueLength = std::sqrt(lengthSquared(value));
    if (valueLength <= kPathComparisonEpsilon) {
        return NavigationVec3{ 0.0F, 1.0F, 0.0F };
    }
    return multiply(value, 1.0F / valueLength);
}

// Closest-point-on-triangle regions from Real-Time Collision Detection,
// expressed in SAVOR-owned vector types.
[[nodiscard]] NavigationVec3 closestPointOnTriangle(
    const NavigationVec3& point,
    const std::array<NavigationVec3, 3>& triangle) noexcept {
    const NavigationVec3& a = triangle[0];
    const NavigationVec3& b = triangle[1];
    const NavigationVec3& c = triangle[2];
    const NavigationVec3 ab = subtract(b, a);
    const NavigationVec3 ac = subtract(c, a);
    const NavigationVec3 ap = subtract(point, a);
    const float d1 = dot(ab, ap);
    const float d2 = dot(ac, ap);
    if (d1 <= 0.0F && d2 <= 0.0F) {
        return a;
    }

    const NavigationVec3 bp = subtract(point, b);
    const float d3 = dot(ab, bp);
    const float d4 = dot(ac, bp);
    if (d3 >= 0.0F && d4 <= d3) {
        return b;
    }

    const float vc = (d1 * d4) - (d3 * d2);
    if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
        const float interpolation = d1 / (d1 - d3);
        return add(a, multiply(ab, interpolation));
    }

    const NavigationVec3 cp = subtract(point, c);
    const float d5 = dot(ab, cp);
    const float d6 = dot(ac, cp);
    if (d6 >= 0.0F && d5 <= d6) {
        return c;
    }

    const float vb = (d5 * d2) - (d1 * d6);
    if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
        const float interpolation = d2 / (d2 - d6);
        return add(a, multiply(ac, interpolation));
    }

    const float va = (d3 * d6) - (d5 * d4);
    if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
        const NavigationVec3 bc = subtract(c, b);
        const float interpolation = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return add(b, multiply(bc, interpolation));
    }

    const float inverseDenominator = 1.0F / (va + vb + vc);
    const float bWeight = vb * inverseDenominator;
    const float cWeight = vc * inverseDenominator;
    return add(a, add(multiply(ab, bWeight), multiply(ac, cWeight)));
}

[[nodiscard]] bool isFinite(const NavigationVec3& point) noexcept {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

void updateBounds(NavigationBounds& bounds, const NavigationVec3& point) noexcept {
    if (!bounds.valid) {
        bounds.minimum = point;
        bounds.maximum = point;
        bounds.valid = true;
        return;
    }
    bounds.minimum.x = std::min(bounds.minimum.x, point.x);
    bounds.minimum.y = std::min(bounds.minimum.y, point.y);
    bounds.minimum.z = std::min(bounds.minimum.z, point.z);
    bounds.maximum.x = std::max(bounds.maximum.x, point.x);
    bounds.maximum.y = std::max(bounds.maximum.y, point.y);
    bounds.maximum.z = std::max(bounds.maximum.z, point.z);
}

[[nodiscard]] float boundsSquaredDiagonal(const NavigationBounds& bounds) noexcept {
    return bounds.valid ? lengthSquared(subtract(bounds.maximum, bounds.minimum)) : 0.0F;
}

struct ClosestTrianglePair {
    NavigationVec3 triggerPoint{};
    NavigationVec3 groundPoint{};
    float distanceSquared = std::numeric_limits<float>::infinity();
};

void considerClosestPair(
    ClosestTrianglePair& best,
    const NavigationVec3& triggerPoint,
    const NavigationVec3& groundPoint) noexcept {
    const float candidateDistanceSquared = lengthSquared(subtract(triggerPoint, groundPoint));
    if (candidateDistanceSquared < best.distanceSquared - kPathComparisonEpsilon) {
        best.triggerPoint = triggerPoint;
        best.groundPoint = groundPoint;
        best.distanceSquared = candidateDistanceSquared;
    }
}

[[nodiscard]] std::pair<NavigationVec3, NavigationVec3> closestPointsOnSegments(
    const NavigationVec3& firstStart,
    const NavigationVec3& firstEnd,
    const NavigationVec3& secondStart,
    const NavigationVec3& secondEnd) noexcept {
    const NavigationVec3 firstDirection = subtract(firstEnd, firstStart);
    const NavigationVec3 secondDirection = subtract(secondEnd, secondStart);
    const NavigationVec3 startDelta = subtract(firstStart, secondStart);
    const float firstLengthSquared = lengthSquared(firstDirection);
    const float secondLengthSquared = lengthSquared(secondDirection);
    const float secondProjection = dot(secondDirection, startDelta);
    float firstParameter = 0.0F;
    float secondParameter = 0.0F;

    if (firstLengthSquared <= kPathComparisonEpsilon &&
        secondLengthSquared <= kPathComparisonEpsilon) {
        return { firstStart, secondStart };
    }
    if (firstLengthSquared <= kPathComparisonEpsilon) {
        secondParameter = std::clamp(secondProjection / secondLengthSquared, 0.0F, 1.0F);
    } else {
        const float firstProjection = dot(firstDirection, startDelta);
        if (secondLengthSquared <= kPathComparisonEpsilon) {
            firstParameter = std::clamp(-firstProjection / firstLengthSquared, 0.0F, 1.0F);
        } else {
            const float directionDot = dot(firstDirection, secondDirection);
            const float denominator = (firstLengthSquared * secondLengthSquared) -
                (directionDot * directionDot);
            if (std::abs(denominator) > kPathComparisonEpsilon) {
                firstParameter = std::clamp(
                    ((directionDot * secondProjection) - (firstProjection * secondLengthSquared)) /
                        denominator,
                    0.0F,
                    1.0F);
            }
            secondParameter = ((directionDot * firstParameter) + secondProjection) /
                secondLengthSquared;
            if (secondParameter < 0.0F) {
                secondParameter = 0.0F;
                firstParameter = std::clamp(-firstProjection / firstLengthSquared, 0.0F, 1.0F);
            } else if (secondParameter > 1.0F) {
                secondParameter = 1.0F;
                firstParameter = std::clamp(
                    (directionDot - firstProjection) / firstLengthSquared,
                    0.0F,
                    1.0F);
            }
        }
    }

    return {
        add(firstStart, multiply(firstDirection, firstParameter)),
        add(secondStart, multiply(secondDirection, secondParameter)),
    };
}

[[nodiscard]] std::optional<NavigationVec3> segmentTriangleIntersection(
    const NavigationVec3& segmentStart,
    const NavigationVec3& segmentEnd,
    const std::array<NavigationVec3, 3>& triangle) noexcept {
    const NavigationVec3 direction = subtract(segmentEnd, segmentStart);
    const NavigationVec3 firstEdge = subtract(triangle[1], triangle[0]);
    const NavigationVec3 secondEdge = subtract(triangle[2], triangle[0]);
    const NavigationVec3 determinantVector = cross(direction, secondEdge);
    const float determinant = dot(firstEdge, determinantVector);
    if (std::abs(determinant) <= kPathComparisonEpsilon) {
        return std::nullopt;
    }

    const float inverseDeterminant = 1.0F / determinant;
    const NavigationVec3 originDelta = subtract(segmentStart, triangle[0]);
    const float firstWeight = dot(originDelta, determinantVector) * inverseDeterminant;
    if (firstWeight < -kPathComparisonEpsilon || firstWeight > 1.0F + kPathComparisonEpsilon) {
        return std::nullopt;
    }

    const NavigationVec3 secondWeightVector = cross(originDelta, firstEdge);
    const float secondWeight = dot(direction, secondWeightVector) * inverseDeterminant;
    if (secondWeight < -kPathComparisonEpsilon ||
        firstWeight + secondWeight > 1.0F + kPathComparisonEpsilon) {
        return std::nullopt;
    }

    const float segmentParameter = dot(secondEdge, secondWeightVector) * inverseDeterminant;
    if (segmentParameter < -kPathComparisonEpsilon ||
        segmentParameter > 1.0F + kPathComparisonEpsilon) {
        return std::nullopt;
    }
    return add(segmentStart, multiply(direction, std::clamp(segmentParameter, 0.0F, 1.0F)));
}

[[nodiscard]] ClosestTrianglePair closestPointsBetweenTriangles(
    const std::array<NavigationVec3, 3>& triggerTriangle,
    const std::array<NavigationVec3, 3>& groundTriangle) noexcept {
    ClosestTrianglePair best{};
    for (const NavigationVec3& triggerVertex : triggerTriangle) {
        considerClosestPair(
            best,
            triggerVertex,
            closestPointOnTriangle(triggerVertex, groundTriangle));
    }
    for (const NavigationVec3& groundVertex : groundTriangle) {
        considerClosestPair(
            best,
            closestPointOnTriangle(groundVertex, triggerTriangle),
            groundVertex);
    }

    constexpr std::array<std::array<std::size_t, 2>, 3> edges{
        std::array<std::size_t, 2>{ 0U, 1U },
        std::array<std::size_t, 2>{ 1U, 2U },
        std::array<std::size_t, 2>{ 2U, 0U },
    };
    for (const auto& triggerEdge : edges) {
        if (const auto intersection = segmentTriangleIntersection(
                triggerTriangle[triggerEdge[0]],
                triggerTriangle[triggerEdge[1]],
                groundTriangle)) {
            considerClosestPair(best, *intersection, *intersection);
        }
    }
    for (const auto& groundEdge : edges) {
        if (const auto intersection = segmentTriangleIntersection(
                groundTriangle[groundEdge[0]],
                groundTriangle[groundEdge[1]],
                triggerTriangle)) {
            considerClosestPair(best, *intersection, *intersection);
        }
    }
    for (const auto& triggerEdge : edges) {
        for (const auto& groundEdge : edges) {
            const auto [triggerPoint, groundPoint] = closestPointsOnSegments(
                triggerTriangle[triggerEdge[0]],
                triggerTriangle[triggerEdge[1]],
                groundTriangle[groundEdge[0]],
                groundTriangle[groundEdge[1]]);
            considerClosestPair(best, triggerPoint, groundPoint);
        }
    }
    return best;
}

struct TriggerMeshGeometry {
    std::size_t meshIndex = 0;
    NavigationBounds bounds{};
    std::vector<std::array<NavigationVec3, 3>> triangles{};
    float aabbSquaredDiagonal = 0.0F;
};

[[nodiscard]] std::optional<TriggerMeshGeometry> collectUsableTriggerMesh(
    const NavigationRegionMesh& source,
    const std::size_t meshIndex) {
    TriggerMeshGeometry geometry{ .meshIndex = meshIndex };
    const NavigationMesh& mesh = source.mesh;
    for (std::size_t index = 0; index + 2U < mesh.indices.size(); index += 3U) {
        const std::uint32_t firstIndex = mesh.indices[index + 0U];
        const std::uint32_t secondIndex = mesh.indices[index + 1U];
        const std::uint32_t thirdIndex = mesh.indices[index + 2U];
        if (firstIndex >= mesh.vertices.size() ||
            secondIndex >= mesh.vertices.size() ||
            thirdIndex >= mesh.vertices.size()) {
            continue;
        }
        const std::array<NavigationVec3, 3> triangle{
            mesh.vertices[firstIndex].position,
            mesh.vertices[secondIndex].position,
            mesh.vertices[thirdIndex].position,
        };
        if (!isFinite(triangle[0]) || !isFinite(triangle[1]) || !isFinite(triangle[2])) {
            continue;
        }
        const NavigationVec3 triangleCross = cross(
            subtract(triangle[1], triangle[0]),
            subtract(triangle[2], triangle[0]));
        if (lengthSquared(triangleCross) <= kPathComparisonEpsilon * kPathComparisonEpsilon) {
            continue;
        }
        geometry.triangles.push_back(triangle);
        updateBounds(geometry.bounds, triangle[0]);
        updateBounds(geometry.bounds, triangle[1]);
        updateBounds(geometry.bounds, triangle[2]);
    }
    if (geometry.triangles.empty() || !geometry.bounds.valid) {
        return std::nullopt;
    }
    geometry.aabbSquaredDiagonal = boundsSquaredDiagonal(geometry.bounds);
    return geometry;
}

[[nodiscard]] bool isLargerTriggerMesh(
    const TriggerMeshGeometry& candidate,
    const TriggerMeshGeometry& current) noexcept {
    if (std::abs(candidate.aabbSquaredDiagonal - current.aabbSquaredDiagonal) >
        kPathComparisonEpsilon) {
        return candidate.aabbSquaredDiagonal > current.aabbSquaredDiagonal;
    }
    if (candidate.triangles.size() != current.triangles.size()) {
        return candidate.triangles.size() > current.triangles.size();
    }
    return candidate.meshIndex < current.meshIndex;
}

[[nodiscard]] bool isValidAnchor(
    const NavigationTraversalGraph& graph,
    const NavigationGraphAnchor& anchor) noexcept {
    return anchor.nodeIndex < graph.nodes.size() && graph.nodes[anchor.nodeIndex].key == anchor.triangle;
}

[[nodiscard]] float segmentCost(
    const NavigationVec3& first,
    const NavigationVec3& second,
    const NavigationVec3& normalizedUp,
    const float slopeCostMultiplier) noexcept {
    const NavigationVec3 delta = subtract(second, first);
    return std::sqrt(lengthSquared(delta)) +
        (slopeCostMultiplier * std::abs(dot(delta, normalizedUp)));
}

[[nodiscard]] float edgeCost(
    const NavigationTraversalGraph& graph,
    const std::size_t sourceNodeIndex,
    const NavigationGraphEdge& edge,
    const NavigationVec3& up,
    const float slopeCostMultiplier) noexcept {
    const NavigationVec3 portalMidpoint = edge.portal.midpoint();
    return segmentCost(graph.nodes[sourceNodeIndex].centroid, portalMidpoint, up, slopeCostMultiplier) +
        segmentCost(portalMidpoint, graph.nodes[edge.targetNodeIndex].centroid, up, slopeCostMultiplier);
}

void appendPolylinePoint(std::vector<NavigationVec3>& points, const NavigationVec3& point) {
    if (points.empty() || distance(points.back(), point) > kPathComparisonEpsilon) {
        points.push_back(point);
    }
}

struct OpenEntry {
    float estimatedTotalCost = 0.0F;
    float costFromStart = 0.0F;
    std::size_t nodeIndex = 0;
};

struct OpenEntryGreater {
    [[nodiscard]] bool operator()(const OpenEntry& lhs, const OpenEntry& rhs) const noexcept {
        if (lhs.estimatedTotalCost != rhs.estimatedTotalCost) {
            return lhs.estimatedTotalCost > rhs.estimatedTotalCost;
        }
        if (lhs.nodeIndex != rhs.nodeIndex) {
            return lhs.nodeIndex > rhs.nodeIndex;
        }
        return lhs.costFromStart > rhs.costFromStart;
    }
};

} // namespace

NavigationGraphAnchorResult NavigationGraphAnchorer::anchorToSurface(
    const NavigationTraversalGraph& graph,
    const std::size_t surfaceIndex,
    const NavigationVec3& exactPoint,
    const float maximumSnapDistance) const {
    if (surfaceIndex >= graph.statistics.sourceSurfaceCount) {
        return NavigationGraphAnchorResult{
            .status = NavigationGraphAnchorStatus::SurfaceNotFound,
            .message = "The selected surface is not part of the traversal graph source model.",
        };
    }

    std::optional<std::size_t> bestNodeIndex{};
    NavigationVec3 bestPoint{};
    float bestDistanceSquared = std::numeric_limits<float>::infinity();
    for (std::size_t nodeIndex = 0; nodeIndex < graph.nodes.size(); ++nodeIndex) {
        const NavigationGraphNode& node = graph.nodes[nodeIndex];
        if (node.key.surfaceIndex != surfaceIndex) {
            continue;
        }
        const NavigationVec3 candidate = closestPointOnTriangle(exactPoint, node.vertices);
        const float candidateDistanceSquared = lengthSquared(subtract(candidate, exactPoint));
        if (candidateDistanceSquared < bestDistanceSquared - kPathComparisonEpsilon ||
            (std::abs(candidateDistanceSquared - bestDistanceSquared) <= kPathComparisonEpsilon &&
                (!bestNodeIndex.has_value() || nodeIndex < *bestNodeIndex))) {
            bestNodeIndex = nodeIndex;
            bestPoint = candidate;
            bestDistanceSquared = candidateDistanceSquared;
        }
    }

    if (!bestNodeIndex.has_value()) {
        return NavigationGraphAnchorResult{
            .status = NavigationGraphAnchorStatus::NoWalkableTriangle,
            .message = "The selected surface contains no usable walkable triangle.",
        };
    }

    const float snapDistance = std::sqrt(bestDistanceSquared);
    if (snapDistance > std::max(maximumSnapDistance, 0.0F)) {
        return NavigationGraphAnchorResult{
            .status = NavigationGraphAnchorStatus::TooFar,
            .message = "The selected point is farther from walkable geometry than the snap limit.",
        };
    }

    const NavigationGraphNode& node = graph.nodes[*bestNodeIndex];
    return NavigationGraphAnchorResult{
        .status = NavigationGraphAnchorStatus::Anchored,
        .anchor = NavigationGraphAnchor{
            .triangle = node.key,
            .nodeIndex = *bestNodeIndex,
            .exactPoint = exactPoint,
            .snappedPoint = bestPoint,
            .snapDistance = snapDistance,
        },
        .message = "Point anchored to walkable geometry.",
    };
}

NavigationStartResolveResult NavigationStartResolver::resolve(
    const NavigationAreaModel& area,
    const NavigationTraversalGraph& graph,
    const NavigationStartOption* const candidate,
    const NavigationCoordinatePolicy coordinatePolicy,
    const NavigationStartResolveOptions& options) const {
    if (candidate == nullptr) {
        return NavigationStartResolveResult{
            .status = NavigationStartResolveStatus::MissingCandidate,
            .message = "No navigation start candidate was selected.",
        };
    }

    if (candidate->availability != NavigationStartAvailability::Resolvable ||
        !candidate->groundTblId.has_value() ||
        !candidate->position.has_value()) {
        std::string message = "The selected navigation start does not contain a complete ground and position.";
        if (!candidate->unavailableReason.empty()) {
            message += " " + candidate->unavailableReason;
        }
        return NavigationStartResolveResult{
            .status = NavigationStartResolveStatus::IncompletePlacement,
            .message = std::move(message),
        };
    }

    const NavigationVec3 exactPoint = coordinatePolicy.convertPosition(*candidate->position);
    if (!std::isfinite(exactPoint.x) || !std::isfinite(exactPoint.y) || !std::isfinite(exactPoint.z)) {
        return NavigationStartResolveResult{
            .status = NavigationStartResolveStatus::IncompletePlacement,
            .message = "The selected navigation start position is not finite.",
        };
    }

    if (!graph.isPathfindingReady() || graph.statistics.sourceSurfaceCount != area.surfaces.size()) {
        return NavigationStartResolveResult{
            .status = NavigationStartResolveStatus::IncompleteGraph,
            .message = "The traversal graph is incomplete or does not match the loaded area.",
        };
    }

    std::vector<std::size_t> matchingSurfaceIndices{};
    for (std::size_t surfaceIndex = 0; surfaceIndex < area.surfaces.size(); ++surfaceIndex) {
        if (area.surfaces[surfaceIndex].tblId == *candidate->groundTblId) {
            matchingSurfaceIndices.push_back(surfaceIndex);
        }
    }
    if (matchingSurfaceIndices.empty()) {
        return NavigationStartResolveResult{
            .status = NavigationStartResolveStatus::MissingGround,
            .message = "No navigation surface matches ground tblId " +
                std::to_string(*candidate->groundTblId) + ".",
        };
    }

    struct SurfaceCandidate {
        std::size_t surfaceIndex = 0;
        NavigationGraphAnchor anchor{};
    };
    std::vector<SurfaceCandidate> anchors{};
    anchors.reserve(matchingSurfaceIndices.size());
    const NavigationGraphAnchorer anchorer{};
    for (const std::size_t surfaceIndex : matchingSurfaceIndices) {
        const NavigationGraphAnchorResult anchored = anchorer.anchorToSurface(
            graph,
            surfaceIndex,
            exactPoint,
            std::numeric_limits<float>::infinity());
        if (anchored.hasAnchor()) {
            anchors.push_back(SurfaceCandidate{
                .surfaceIndex = surfaceIndex,
                .anchor = *anchored.anchor,
            });
        }
    }
    if (anchors.empty()) {
        return NavigationStartResolveResult{
            .status = NavigationStartResolveStatus::AnchoringFailure,
            .message = "Ground tblId " + std::to_string(*candidate->groundTblId) +
                " has no usable traversal-graph triangle.",
        };
    }

    std::sort(anchors.begin(), anchors.end(), [](const SurfaceCandidate& lhs, const SurfaceCandidate& rhs) {
        if (lhs.anchor.snapDistance != rhs.anchor.snapDistance) {
            return lhs.anchor.snapDistance < rhs.anchor.snapDistance;
        }
        if (lhs.surfaceIndex != rhs.surfaceIndex) {
            return lhs.surfaceIndex < rhs.surfaceIndex;
        }
        return lhs.anchor.nodeIndex < rhs.anchor.nodeIndex;
    });

    const float tieTolerance = std::max(options.surfaceDistanceTieTolerance, 0.0F);
    const float pointTolerance = std::max(options.snappedPointCoincidenceTolerance, 0.0F);
    for (std::size_t index = 1; index < anchors.size(); ++index) {
        if (std::abs(anchors[index].anchor.snapDistance - anchors[0].anchor.snapDistance) > tieTolerance) {
            break;
        }
        if (distance(anchors[index].anchor.snappedPoint, anchors[0].anchor.snappedPoint) > pointTolerance) {
            return NavigationStartResolveResult{
                .status = NavigationStartResolveStatus::AmbiguousSurface,
                .message = "Ground tblId " + std::to_string(*candidate->groundTblId) +
                    " has multiple effectively equidistant navigation surfaces at different world-space points.",
            };
        }
    }

    std::optional<float> convertedYaw{};
    if (candidate->yawDegrees.has_value() && std::isfinite(*candidate->yawDegrees)) {
        const float value = coordinatePolicy.convertYaw(*candidate->yawDegrees);
        if (std::isfinite(value)) {
            convertedYaw = value;
        }
    }

    return NavigationStartResolveResult{
        .status = NavigationStartResolveStatus::Resolved,
        .anchor = anchors.front().anchor,
        .yawDegrees = convertedYaw,
        .message = "Navigation start anchored to ground tblId " +
            std::to_string(*candidate->groundTblId) + ".",
    };
}

NavigationTriggerGoalResolveResult NavigationTriggerGoalResolver::resolve(
    const NavigationAreaModel& area,
    const NavigationTraversalGraph& graph,
    const std::size_t regionIndex,
    const NavigationTriggerGoalResolveOptions& options) const {
    if (regionIndex >= area.regions.size()) {
        return NavigationTriggerGoalResolveResult{
            .status = NavigationTriggerGoalResolveStatus::RegionNotFound,
            .message = "The selected navigation region does not exist.",
        };
    }

    const NavigationRegion& region = area.regions[regionIndex];
    if (region.kind != NavigationRegionKind::Trigger) {
        return NavigationTriggerGoalResolveResult{
            .status = NavigationTriggerGoalResolveStatus::NotTrigger,
            .message = "The selected navigation region is not a trigger.",
        };
    }

    std::optional<TriggerMeshGeometry> primaryMesh{};
    for (std::size_t meshIndex = 0; meshIndex < region.meshes.size(); ++meshIndex) {
        auto candidate = collectUsableTriggerMesh(region.meshes[meshIndex], meshIndex);
        if (candidate.has_value() &&
            (!primaryMesh.has_value() || isLargerTriggerMesh(*candidate, *primaryMesh))) {
            primaryMesh = std::move(candidate);
        }
    }
    if (!primaryMesh.has_value()) {
        return NavigationTriggerGoalResolveResult{
            .status = NavigationTriggerGoalResolveStatus::MissingGeometry,
            .message = "The selected trigger has no usable projected triangle mesh.",
        };
    }

    if (!graph.isPathfindingReady() || graph.statistics.sourceSurfaceCount != area.surfaces.size()) {
        return NavigationTriggerGoalResolveResult{
            .status = NavigationTriggerGoalResolveStatus::IncompleteGraph,
            .message = "The traversal graph is incomplete or does not match the loaded area.",
        };
    }

    struct GraphCandidate {
        std::size_t nodeIndex = 0;
        NavigationVec3 triggerPoint{};
        NavigationVec3 groundPoint{};
        float distance = std::numeric_limits<float>::infinity();
    };
    std::vector<GraphCandidate> candidates{};
    candidates.reserve(graph.nodes.size());
    for (std::size_t nodeIndex = 0; nodeIndex < graph.nodes.size(); ++nodeIndex) {
        const NavigationGraphNode& node = graph.nodes[nodeIndex];
        ClosestTrianglePair best{};
        for (const auto& triggerTriangle : primaryMesh->triangles) {
            const ClosestTrianglePair pair = closestPointsBetweenTriangles(
                triggerTriangle,
                node.vertices);
            if (pair.distanceSquared < best.distanceSquared - kPathComparisonEpsilon) {
                best = pair;
            }
        }
        if (!std::isfinite(best.distanceSquared)) {
            continue;
        }
        candidates.push_back(GraphCandidate{
            .nodeIndex = nodeIndex,
            .triggerPoint = best.triggerPoint,
            .groundPoint = best.groundPoint,
            .distance = std::sqrt(std::max(best.distanceSquared, 0.0F)),
        });
    }
    if (candidates.empty()) {
        return NavigationTriggerGoalResolveResult{
            .status = NavigationTriggerGoalResolveStatus::AnchoringFailure,
            .message = "No walkable traversal-graph triangle can be anchored to the selected trigger.",
        };
    }

    std::sort(candidates.begin(), candidates.end(), [](const GraphCandidate& lhs, const GraphCandidate& rhs) {
        if (lhs.distance != rhs.distance) {
            return lhs.distance < rhs.distance;
        }
        return lhs.nodeIndex < rhs.nodeIndex;
    });

    const float tieTolerance = std::max(options.surfaceDistanceTieTolerance, 0.0F);
    const float pointTolerance = std::max(options.snappedPointCoincidenceTolerance, 0.0F);
    const NavigationGraphNode& bestNode = graph.nodes[candidates.front().nodeIndex];
    for (std::size_t index = 1; index < candidates.size(); ++index) {
        if (std::abs(candidates[index].distance - candidates.front().distance) > tieTolerance) {
            break;
        }
        const NavigationGraphNode& tiedNode = graph.nodes[candidates[index].nodeIndex];
        if (tiedNode.key.surfaceIndex != bestNode.key.surfaceIndex &&
            distance(candidates[index].groundPoint, candidates.front().groundPoint) > pointTolerance) {
            return NavigationTriggerGoalResolveResult{
                .status = NavigationTriggerGoalResolveStatus::AmbiguousSurface,
                .message = "The selected trigger is effectively equidistant from multiple navigation surfaces at different world-space points.",
            };
        }
    }

    const GraphCandidate& best = candidates.front();
    return NavigationTriggerGoalResolveResult{
        .status = NavigationTriggerGoalResolveStatus::Resolved,
        .target = NavigationTriggerGoalTarget{
            .regionIndex = regionIndex,
            .primaryMeshIndex = primaryMesh->meshIndex,
            .displayBounds = primaryMesh->bounds,
            .anchor = NavigationGraphAnchor{
                .triangle = bestNode.key,
                .nodeIndex = best.nodeIndex,
                .exactPoint = best.triggerPoint,
                .snappedPoint = best.groundPoint,
                .snapDistance = best.distance,
            },
            .primaryMeshAabbSquaredDiagonal = primaryMesh->aabbSquaredDiagonal,
            .primaryMeshReferencedTriangleCount = primaryMesh->triangles.size(),
        },
        .message = "Trigger goal anchored to the nearest walkable navigation surface.",
    };
}

NavigationPathResult NavigationPathfinder::findPath(
    const NavigationTraversalGraph& graph,
    const NavigationPathQuery& query,
    const NavigationCoordinatePolicy coordinatePolicy) const {
    if (query.requireCompleteGeometry && !graph.isPathfindingReady()) {
        return NavigationPathResult{
            .status = NavigationPathStatus::IncompleteGeometry,
            .message = "Pathfinding requires complete ground and wall geometry.",
        };
    }
    if (!isValidAnchor(graph, query.start)) {
        return NavigationPathResult{
            .status = NavigationPathStatus::InvalidStart,
            .message = "The start anchor does not identify a traversal-graph triangle.",
        };
    }
    if (!isValidAnchor(graph, query.goal)) {
        return NavigationPathResult{
            .status = NavigationPathStatus::InvalidGoal,
            .message = "The goal anchor does not identify a traversal-graph triangle.",
        };
    }

    const std::size_t startNodeIndex = query.start.nodeIndex;
    const std::size_t goalNodeIndex = query.goal.nodeIndex;
    if (startNodeIndex == goalNodeIndex) {
        NavigationPathResult result{
            .status = NavigationPathStatus::Success,
            .trianglePath = { query.start.triangle },
            .message = "Start and goal share one walkable triangle.",
        };
        appendPolylinePoint(result.polyline, query.start.snappedPoint);
        appendPolylinePoint(result.polyline, query.goal.snappedPoint);
        result.routeLength = distance(query.start.snappedPoint, query.goal.snappedPoint);
        const NavigationVec3 up = normalized(coordinatePolicy.upAxis());
        result.totalCost = segmentCost(
            query.start.snappedPoint,
            query.goal.snappedPoint,
            up,
            std::max(query.slopeCostMultiplier, 0.0F));
        return result;
    }

    const float slopeCostMultiplier = std::max(query.slopeCostMultiplier, 0.0F);
    const NavigationVec3 up = normalized(coordinatePolicy.upAxis());
    const std::size_t nodeCount = graph.nodes.size();
    const float infinity = std::numeric_limits<float>::infinity();
    std::vector<float> costs(nodeCount, infinity);
    std::vector<std::optional<std::size_t>> predecessors(nodeCount);
    std::vector<std::optional<std::size_t>> predecessorEdges(nodeCount);
    std::vector<bool> closed(nodeCount, false);
    std::priority_queue<OpenEntry, std::vector<OpenEntry>, OpenEntryGreater> open{};

    costs[startNodeIndex] = 0.0F;
    open.push(OpenEntry{
        .estimatedTotalCost = distance(
            graph.nodes[startNodeIndex].centroid,
            graph.nodes[goalNodeIndex].centroid),
        .costFromStart = 0.0F,
        .nodeIndex = startNodeIndex,
    });

    while (!open.empty()) {
        const OpenEntry current = open.top();
        open.pop();
        if (current.costFromStart > costs[current.nodeIndex] + kPathComparisonEpsilon || closed[current.nodeIndex]) {
            continue;
        }
        if (current.nodeIndex == goalNodeIndex) {
            break;
        }
        closed[current.nodeIndex] = true;

        const NavigationGraphNode& node = graph.nodes[current.nodeIndex];
        for (std::size_t edgeIndex = 0; edgeIndex < node.edges.size(); ++edgeIndex) {
            const NavigationGraphEdge& edge = node.edges[edgeIndex];
            if (edge.targetNodeIndex >= nodeCount || closed[edge.targetNodeIndex]) {
                continue;
            }
            const float candidateCost = costs[current.nodeIndex] + edgeCost(
                graph,
                current.nodeIndex,
                edge,
                up,
                slopeCostMultiplier);
            const bool strictlyBetter = candidateCost < costs[edge.targetNodeIndex] - kPathComparisonEpsilon;
            const bool equalWithStablePredecessor =
                std::abs(candidateCost - costs[edge.targetNodeIndex]) <= kPathComparisonEpsilon &&
                (!predecessors[edge.targetNodeIndex].has_value() ||
                    current.nodeIndex < *predecessors[edge.targetNodeIndex]);
            if (!strictlyBetter && !equalWithStablePredecessor) {
                continue;
            }

            costs[edge.targetNodeIndex] = candidateCost;
            predecessors[edge.targetNodeIndex] = current.nodeIndex;
            predecessorEdges[edge.targetNodeIndex] = edgeIndex;
            open.push(OpenEntry{
                .estimatedTotalCost = candidateCost + distance(
                    graph.nodes[edge.targetNodeIndex].centroid,
                    graph.nodes[goalNodeIndex].centroid),
                .costFromStart = candidateCost,
                .nodeIndex = edge.targetNodeIndex,
            });
        }
    }

    if (!predecessors[goalNodeIndex].has_value()) {
        return NavigationPathResult{
            .status = NavigationPathStatus::Unreachable,
            .message = "No directed route connects the selected start and goal.",
        };
    }

    std::vector<std::size_t> reverseNodes{ goalNodeIndex };
    std::vector<std::pair<std::size_t, std::size_t>> reverseEdges{};
    std::size_t currentNodeIndex = goalNodeIndex;
    while (currentNodeIndex != startNodeIndex) {
        if (!predecessors[currentNodeIndex].has_value() || !predecessorEdges[currentNodeIndex].has_value()) {
            return NavigationPathResult{
                .status = NavigationPathStatus::Unreachable,
                .message = "The route predecessor chain is incomplete.",
            };
        }
        const std::size_t predecessor = *predecessors[currentNodeIndex];
        reverseEdges.emplace_back(predecessor, *predecessorEdges[currentNodeIndex]);
        currentNodeIndex = predecessor;
        reverseNodes.push_back(currentNodeIndex);
    }
    std::reverse(reverseNodes.begin(), reverseNodes.end());
    std::reverse(reverseEdges.begin(), reverseEdges.end());

    NavigationPathResult result{
        .status = NavigationPathStatus::Success,
        .message = "Path found.",
    };
    result.trianglePath.reserve(reverseNodes.size());
    for (const std::size_t nodeIndex : reverseNodes) {
        result.trianglePath.push_back(graph.nodes[nodeIndex].key);
    }

    appendPolylinePoint(result.polyline, query.start.snappedPoint);
    for (const auto& [predecessor, edgeIndex] : reverseEdges) {
        appendPolylinePoint(result.polyline, graph.nodes[predecessor].edges[edgeIndex].portal.midpoint());
    }
    appendPolylinePoint(result.polyline, query.goal.snappedPoint);

    for (std::size_t index = 1; index < result.polyline.size(); ++index) {
        result.routeLength += distance(result.polyline[index - 1U], result.polyline[index]);
        result.totalCost += segmentCost(
            result.polyline[index - 1U],
            result.polyline[index],
            up,
            slopeCostMultiplier);
    }
    return result;
}

} // namespace savor::navigation
