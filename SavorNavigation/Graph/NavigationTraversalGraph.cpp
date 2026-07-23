#include "NavigationTraversalGraph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

struct PlanarPoint {
    float x = 0.0F;
    float y = 0.0F;
};

[[nodiscard]] PlanarPoint add(const PlanarPoint lhs, const PlanarPoint rhs) noexcept {
    return PlanarPoint{ lhs.x + rhs.x, lhs.y + rhs.y };
}

[[nodiscard]] PlanarPoint subtract(const PlanarPoint lhs, const PlanarPoint rhs) noexcept {
    return PlanarPoint{ lhs.x - rhs.x, lhs.y - rhs.y };
}

[[nodiscard]] PlanarPoint multiply(const PlanarPoint value, const float scalar) noexcept {
    return PlanarPoint{ value.x * scalar, value.y * scalar };
}

[[nodiscard]] float cross2(const PlanarPoint lhs, const PlanarPoint rhs) noexcept {
    return (lhs.x * rhs.y) - (lhs.y * rhs.x);
}

[[nodiscard]] float length(const PlanarPoint value) noexcept {
    return std::sqrt((value.x * value.x) + (value.y * value.y));
}

struct PlanarBasis {
    NavigationVec3 horizontal{};
    NavigationVec3 vertical{};
    NavigationVec3 up{};

    [[nodiscard]] PlanarPoint project(const NavigationVec3& value) const noexcept {
        return PlanarPoint{ dot(value, horizontal), dot(value, vertical) };
    }

    [[nodiscard]] NavigationVec3 lift(const PlanarPoint value, const float height) const noexcept {
        return add(
            add(multiply(horizontal, value.x), multiply(vertical, value.y)),
            multiply(up, height));
    }
};

[[nodiscard]] PlanarBasis makePlanarBasis(const NavigationVec3& requestedUp) noexcept {
    NavigationVec3 up = normalized(requestedUp);
    if (lengthSquared(up) <= kComparisonEpsilon) {
        up = NavigationVec3{ 0.0F, 1.0F, 0.0F };
    }
    const NavigationVec3 reference = std::abs(up.y) < 0.9F
        ? NavigationVec3{ 0.0F, 1.0F, 0.0F }
        : NavigationVec3{ 1.0F, 0.0F, 0.0F };
    const NavigationVec3 horizontal = normalized(cross(reference, up));
    return PlanarBasis{
        .horizontal = horizontal,
        .vertical = normalized(cross(up, horizontal)),
        .up = up,
    };
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

struct TriangleRecord {
    NavigationTriangleKey key{};
    std::array<NavigationVec3, 3> vertices{};
    NavigationVec3 planeNormal{};
    NavigationVec3 unitNormal{};
    std::optional<std::size_t> nodeIndex{};
    bool retained = true;
};

struct EdgeOccurrence {
    std::size_t triangleRecordIndex = 0;
    NavigationVec3 first{};
    NavigationVec3 second{};
    NavigationVec3 opposite{};
};

struct BoundarySegment {
    std::size_t triangleRecordIndex = 0;
    NavigationVec3 first{};
    NavigationVec3 second{};
    NavigationVec3 opposite{};
};

[[nodiscard]] bool nearlyEqual(
    const NavigationVec3& lhs,
    const NavigationVec3& rhs,
    const float tolerance = kComparisonEpsilon) noexcept {
    return lengthSquared(subtract(lhs, rhs)) <= (tolerance * tolerance);
}

[[nodiscard]] bool addEdgeIfUnique(NavigationGraphNode& node, NavigationGraphEdge edge) {
    const auto duplicate = std::find_if(node.edges.begin(), node.edges.end(), [&](const NavigationGraphEdge& existing) {
        if (existing.targetNodeIndex != edge.targetNodeIndex || existing.kind != edge.kind ||
            existing.collisionHandoffIndex != edge.collisionHandoffIndex) {
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

[[nodiscard]] bool pointInTriangle(
    const PlanarPoint point,
    const std::array<PlanarPoint, 3>& triangle,
    const float tolerance) noexcept {
    const float orientation = cross2(
        subtract(triangle[1], triangle[0]),
        subtract(triangle[2], triangle[0]));
    if (std::abs(orientation) <= kComparisonEpsilon) {
        return false;
    }
    const float orientationSign = orientation > 0.0F ? 1.0F : -1.0F;
    for (std::size_t edge = 0; edge < 3U; ++edge) {
        const PlanarPoint first = triangle[edge];
        const PlanarPoint second = triangle[(edge + 1U) % 3U];
        const PlanarPoint edgeVector = subtract(second, first);
        const float edgeLength = length(edgeVector);
        if ((orientationSign * cross2(edgeVector, subtract(point, first))) < -(tolerance * edgeLength)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<float> triangleHeightAt(
    const TriangleRecord& triangle,
    const PlanarPoint point,
    const PlanarBasis& basis,
    const NavigationGraphBuildOptions& options,
    const bool requireContainment = true) noexcept {
    const std::array<PlanarPoint, 3> projected{
        basis.project(triangle.vertices[0]),
        basis.project(triangle.vertices[1]),
        basis.project(triangle.vertices[2]),
    };
    if (requireContainment && !pointInTriangle(point, projected, options.planarContainmentTolerance)) {
        return std::nullopt;
    }

    if (std::abs(dot(triangle.unitNormal, basis.up)) < options.minimumUpNormalComponent) {
        return std::nullopt;
    }
    const float upComponent = dot(triangle.planeNormal, basis.up);
    const NavigationVec3 planarPoint = basis.lift(point, 0.0F);
    const float height = dot(triangle.planeNormal, subtract(triangle.vertices[0], planarPoint)) / upComponent;
    if (!std::isfinite(height)) {
        return std::nullopt;
    }
    return height;
}

enum class CollisionQueryStatus {
    NoHit,
    Hit,
    Ambiguous,
};

struct CollisionQueryResult {
    CollisionQueryStatus status = CollisionQueryStatus::NoHit;
    std::optional<std::size_t> triangleRecordIndex{};
    float height = 0.0F;
};

[[nodiscard]] CollisionQueryResult queryTriangles(
    const std::vector<std::size_t>& candidates,
    const std::vector<TriangleRecord>& triangles,
    const PlanarPoint point,
    const float expectedHeight,
    const PlanarBasis& basis,
    const NavigationGraphBuildOptions& options,
    const std::optional<std::size_t> excludedTriangleRecord = std::nullopt) noexcept {
    CollisionQueryResult result{};
    float bestDistance = std::numeric_limits<float>::infinity();
    for (const std::size_t candidateIndex : candidates) {
        const TriangleRecord& candidate = triangles[candidateIndex];
        if (!candidate.retained ||
            (excludedTriangleRecord.has_value() && candidateIndex == *excludedTriangleRecord)) {
            continue;
        }
        const auto height = triangleHeightAt(candidate, point, basis, options);
        if (!height.has_value()) {
            continue;
        }

        const float distance = std::abs(*height - expectedHeight);
        if (distance < (bestDistance - options.distinctHeightTieTolerance)) {
            result.status = CollisionQueryStatus::Hit;
            result.triangleRecordIndex = candidateIndex;
            result.height = *height;
            bestDistance = distance;
            continue;
        }
        if (std::abs(distance - bestDistance) > options.distinctHeightTieTolerance) {
            continue;
        }

        if (std::abs(*height - result.height) > options.distinctHeightTieTolerance) {
            result.status = CollisionQueryStatus::Ambiguous;
            continue;
        }
        if (result.triangleRecordIndex.has_value() &&
            candidate.key < triangles[*result.triangleRecordIndex].key) {
            result.triangleRecordIndex = candidateIndex;
            result.height = *height;
        }
    }
    return result;
}

void addSegmentIntersectionParameters(
    std::vector<float>& parameters,
    const PlanarPoint sourceFirst,
    const PlanarPoint sourceSecond,
    const PlanarPoint targetFirst,
    const PlanarPoint targetSecond) {
    const PlanarPoint sourceVector = subtract(sourceSecond, sourceFirst);
    const PlanarPoint targetVector = subtract(targetSecond, targetFirst);
    const float denominator = cross2(sourceVector, targetVector);
    const PlanarPoint displacement = subtract(targetFirst, sourceFirst);
    if (std::abs(denominator) > kComparisonEpsilon) {
        const float sourceParameter = cross2(displacement, targetVector) / denominator;
        const float targetParameter = cross2(displacement, sourceVector) / denominator;
        if (sourceParameter >= -kComparisonEpsilon && sourceParameter <= (1.0F + kComparisonEpsilon) &&
            targetParameter >= -kComparisonEpsilon && targetParameter <= (1.0F + kComparisonEpsilon)) {
            parameters.push_back(std::clamp(sourceParameter, 0.0F, 1.0F));
        }
        return;
    }

    if (std::abs(cross2(displacement, sourceVector)) > kComparisonEpsilon) {
        return;
    }
    const float sourceLengthSquared =
        (sourceVector.x * sourceVector.x) + (sourceVector.y * sourceVector.y);
    if (sourceLengthSquared <= kComparisonEpsilon) {
        return;
    }
    const auto parameterFor = [&](const PlanarPoint point) {
        const PlanarPoint relative = subtract(point, sourceFirst);
        return ((relative.x * sourceVector.x) + (relative.y * sourceVector.y)) / sourceLengthSquared;
    };
    const float firstParameter = parameterFor(targetFirst);
    const float secondParameter = parameterFor(targetSecond);
    if (firstParameter >= -kComparisonEpsilon && firstParameter <= (1.0F + kComparisonEpsilon)) {
        parameters.push_back(std::clamp(firstParameter, 0.0F, 1.0F));
    }
    if (secondParameter >= -kComparisonEpsilon && secondParameter <= (1.0F + kComparisonEpsilon)) {
        parameters.push_back(std::clamp(secondParameter, 0.0F, 1.0F));
    }
}

void appendTrianglePartitionParameters(
    std::vector<float>& parameters,
    const PlanarPoint segmentFirst,
    const PlanarPoint segmentSecond,
    const TriangleRecord& triangle,
    const PlanarBasis& basis) {
    const std::array<PlanarPoint, 3> projected{
        basis.project(triangle.vertices[0]),
        basis.project(triangle.vertices[1]),
        basis.project(triangle.vertices[2]),
    };
    for (std::size_t edge = 0; edge < 3U; ++edge) {
        addSegmentIntersectionParameters(
            parameters,
            segmentFirst,
            segmentSecond,
            projected[edge],
            projected[(edge + 1U) % 3U]);
    }
}

[[nodiscard]] std::vector<float> partitionBoundary(
    const BoundarySegment& boundary,
    const std::vector<std::size_t>& candidates,
    const std::vector<TriangleRecord>& triangles,
    const PlanarBasis& basis,
    const PlanarPoint outward,
    const NavigationGraphBuildOptions& options) {
    const PlanarPoint first = basis.project(boundary.first);
    const PlanarPoint second = basis.project(boundary.second);
    const PlanarPoint fullyProbedFirst = add(first, multiply(outward, options.outwardProbeDistance));
    const PlanarPoint fullyProbedSecond = add(second, multiply(outward, options.outwardProbeDistance));
    std::vector<float> parameters{ 0.0F, 1.0F };
    for (const std::size_t candidate : candidates) {
        if (!triangles[candidate].retained) {
            continue;
        }
        appendTrianglePartitionParameters(parameters, first, second, triangles[candidate], basis);
        appendTrianglePartitionParameters(
            parameters,
            fullyProbedFirst,
            fullyProbedSecond,
            triangles[candidate],
            basis);
    }
    std::sort(parameters.begin(), parameters.end());
    std::erase_if(parameters, [](const float value) {
        return !std::isfinite(value);
    });
    const auto newEnd = std::unique(parameters.begin(), parameters.end(), [](const float lhs, const float rhs) {
        return std::abs(lhs - rhs) <= kComparisonEpsilon;
    });
    parameters.erase(newEnd, parameters.end());
    return parameters;
}

[[nodiscard]] std::vector<float> partitionBoundaryIntervalAtProbeDistance(
    const BoundarySegment& boundary,
    const float firstParameter,
    const float secondParameter,
    const std::vector<std::size_t>& candidates,
    const std::vector<TriangleRecord>& triangles,
    const PlanarBasis& basis,
    const PlanarPoint outward,
    const float probeDistance) {
    const PlanarPoint boundaryFirst = basis.project(boundary.first);
    const PlanarPoint boundarySecond = basis.project(boundary.second);
    const PlanarPoint probedFirst = add(boundaryFirst, multiply(outward, probeDistance));
    const PlanarPoint probedSecond = add(boundarySecond, multiply(outward, probeDistance));

    std::vector<float> parameters{ firstParameter, secondParameter };
    std::vector<float> probeIntersections{};
    for (const std::size_t candidate : candidates) {
        if (!triangles[candidate].retained) {
            continue;
        }
        appendTrianglePartitionParameters(
            probeIntersections,
            probedFirst,
            probedSecond,
            triangles[candidate],
            basis);
    }
    for (const float parameter : probeIntersections) {
        if (std::isfinite(parameter) &&
            parameter > (firstParameter + kComparisonEpsilon) &&
            parameter < (secondParameter - kComparisonEpsilon)) {
            parameters.push_back(parameter);
        }
    }
    std::sort(parameters.begin(), parameters.end());
    const auto newEnd = std::unique(parameters.begin(), parameters.end(), [](const float lhs, const float rhs) {
        return std::abs(lhs - rhs) <= kComparisonEpsilon;
    });
    parameters.erase(newEnd, parameters.end());
    return parameters;
}

struct BoundaryProbeInterval {
    float firstParameter = 0.0F;
    float secondParameter = 0.0F;
    float probeDistance = 0.0F;
};

constexpr std::size_t kMaximumProbeRefinementDepth = 64U;

void appendRefinedBoundaryProbeIntervals(
    std::vector<BoundaryProbeInterval>& out,
    std::size_t& refinementExhaustionCount,
    const BoundarySegment& boundary,
    const float firstParameter,
    const float secondParameter,
    const float planarLength,
    const std::vector<std::size_t>& candidates,
    const std::vector<TriangleRecord>& triangles,
    const PlanarBasis& basis,
    const PlanarPoint outward,
    const NavigationGraphBuildOptions& options,
    const std::size_t depth = 0U) {
    const float intervalLength = (secondParameter - firstParameter) * planarLength;
    if (intervalLength < options.minimumPortalLength) {
        return;
    }
    const float probeDistance = std::min(
        options.outwardProbeDistance,
        intervalLength * 0.25F);
    const std::vector<float> partitions = partitionBoundaryIntervalAtProbeDistance(
        boundary,
        firstParameter,
        secondParameter,
        candidates,
        triangles,
        basis,
        outward,
        probeDistance);
    if (partitions.size() <= 2U) {
        out.push_back(BoundaryProbeInterval{
            .firstParameter = firstParameter,
            .secondParameter = secondParameter,
            .probeDistance = probeDistance,
        });
        return;
    }
    if (depth >= kMaximumProbeRefinementDepth) {
        ++refinementExhaustionCount;
        return;
    }
    for (std::size_t partition = 1U; partition < partitions.size(); ++partition) {
        appendRefinedBoundaryProbeIntervals(
            out,
            refinementExhaustionCount,
            boundary,
            partitions[partition - 1U],
            partitions[partition],
            planarLength,
            candidates,
            triangles,
            basis,
            outward,
            options,
            depth + 1U);
    }
}

[[nodiscard]] bool handoffIsContinuous(
    const BoundarySegment& boundary,
    const TriangleRecord& target,
    const PlanarPoint outward,
    const float firstParameter,
    const float secondParameter,
    const float probeDistance,
    const PlanarBasis& basis,
    const NavigationGraphBuildOptions& options) noexcept {
    const NavigationVec3 sourceVector = subtract(boundary.second, boundary.first);
    const PlanarPoint planarFirst = basis.project(boundary.first);
    const PlanarPoint planarVector = subtract(basis.project(boundary.second), planarFirst);
    for (const float parameter : { firstParameter, secondParameter }) {
        const NavigationVec3 sourcePoint = add(boundary.first, multiply(sourceVector, parameter));
        const float sourceHeight = dot(sourcePoint, basis.up);
        const PlanarPoint probePoint = add(
            add(planarFirst, multiply(planarVector, parameter)),
            multiply(outward, probeDistance));
        const auto targetHeight = triangleHeightAt(target, probePoint, basis, options);
        if (!targetHeight.has_value() ||
            std::abs(*targetHeight - sourceHeight) > options.heightContinuityTolerance) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::vector<std::size_t> surfaceIndicesForKeys(
    const NavigationAreaModel& area,
    const std::vector<NavigationSurfaceSourceKey>& keys) {
    std::vector<std::size_t> result{};
    for (std::size_t surfaceIndex = 0; surfaceIndex < area.surfaces.size(); ++surfaceIndex) {
        const NavigationSurfaceSourceKey& surfaceKey = area.surfaces[surfaceIndex].sourceKey;
        if (std::find(keys.begin(), keys.end(), surfaceKey) != keys.end()) {
            result.push_back(surfaceIndex);
        }
    }
    return result;
}

[[nodiscard]] std::vector<std::size_t> triangleIndicesForSurfaces(
    const std::vector<std::size_t>& surfaces,
    const std::vector<std::vector<std::size_t>>& trianglesBySurface) {
    std::vector<std::size_t> result{};
    for (const std::size_t surface : surfaces) {
        if (surface >= trianglesBySurface.size()) {
            continue;
        }
        result.insert(result.end(), trianglesBySurface[surface].begin(), trianglesBySurface[surface].end());
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

[[nodiscard]] bool targetRequiresRuntimeState(
    const NavigationAreaModel& area,
    const TriangleRecord& source,
    const TriangleRecord& target) noexcept {
    return area.surfaces[source.key.surfaceIndex].traversalAvailability ==
            NavigationSurfaceTraversalAvailability::RequiresRuntimeState ||
        area.surfaces[target.key.surfaceIndex].traversalAvailability ==
            NavigationSurfaceTraversalAvailability::RequiresRuntimeState;
}

[[nodiscard]] bool anySurfaceRequiresRuntimeState(
    const NavigationAreaModel& area,
    const std::vector<std::size_t>& surfaceIndices,
    const std::optional<std::size_t> excludedSurface = std::nullopt) noexcept {
    return std::any_of(surfaceIndices.begin(), surfaceIndices.end(), [&](const std::size_t surfaceIndex) {
        return (!excludedSurface.has_value() || surfaceIndex != *excludedSurface) &&
            area.surfaces[surfaceIndex].traversalAvailability ==
                NavigationSurfaceTraversalAvailability::RequiresRuntimeState;
    });
}

[[nodiscard]] bool samePortal(
    const NavigationPortalSegment& lhs,
    const NavigationPortalSegment& rhs,
    const float tolerance) noexcept {
    return (nearlyEqual(lhs.first, rhs.first, tolerance) && nearlyEqual(lhs.second, rhs.second, tolerance)) ||
        (nearlyEqual(lhs.first, rhs.second, tolerance) && nearlyEqual(lhs.second, rhs.first, tolerance));
}

[[nodiscard]] std::size_t appendHandoffIfUnique(
    NavigationTraversalGraph& graph,
    NavigationCollisionHandoff handoff,
    const float tolerance) {
    if (handoff.kind == NavigationCollisionHandoffKind::SameEntryBundle &&
        handoff.targetTriangle < handoff.sourceTriangle) {
        std::swap(handoff.sourceTriangle, handoff.targetTriangle);
        std::swap(handoff.sourceEntryId, handoff.targetEntryId);
    }

    const auto found = std::find_if(
        graph.collisionHandoffs.begin(),
        graph.collisionHandoffs.end(),
        [&](const NavigationCollisionHandoff& existing) {
            return existing.sourceTriangle == handoff.sourceTriangle &&
                existing.targetTriangle == handoff.targetTriangle &&
                existing.kind == handoff.kind &&
                existing.availability == handoff.availability &&
                existing.authoredFallbackChainIndex == handoff.authoredFallbackChainIndex &&
                existing.authoredFallbackTargetIndex == handoff.authoredFallbackTargetIndex &&
                samePortal(existing.portal, handoff.portal, tolerance);
        });
    if (found != graph.collisionHandoffs.end()) {
        return static_cast<std::size_t>(std::distance(graph.collisionHandoffs.begin(), found));
    }

    const std::size_t result = graph.collisionHandoffs.size();
    if (handoff.kind == NavigationCollisionHandoffKind::SameEntryBundle) {
        ++graph.statistics.sameEntryHandoffCount;
    } else {
        ++graph.statistics.authoredFallbackHandoffCount;
    }
    if (handoff.availability == NavigationCollisionHandoffAvailability::RequiresRuntimeState) {
        ++graph.statistics.conditionalHandoffCount;
    }
    graph.collisionHandoffs.push_back(std::move(handoff));
    return result;
}

struct PendingHandoff {
    std::size_t sourceTriangleRecordIndex = 0;
    std::size_t targetTriangleRecordIndex = 0;
    float firstParameter = 0.0F;
    float secondParameter = 0.0F;
    NavigationCollisionHandoffKind kind = NavigationCollisionHandoffKind::SameEntryBundle;
    NavigationCollisionHandoffAvailability availability =
        NavigationCollisionHandoffAvailability::ActiveStatic;
    std::uint32_t sourceEntryId = 0;
    std::uint32_t targetEntryId = 0;
    std::optional<std::size_t> chainIndex{};
    std::optional<std::size_t> targetIndex{};
    std::optional<std::size_t> authoredOrdinal{};
};

[[nodiscard]] bool canMerge(const PendingHandoff& lhs, const PendingHandoff& rhs) noexcept {
    return lhs.sourceTriangleRecordIndex == rhs.sourceTriangleRecordIndex &&
        lhs.targetTriangleRecordIndex == rhs.targetTriangleRecordIndex &&
        lhs.kind == rhs.kind &&
        lhs.availability == rhs.availability &&
        lhs.chainIndex == rhs.chainIndex &&
        lhs.targetIndex == rhs.targetIndex &&
        std::abs(lhs.secondParameter - rhs.firstParameter) <= kComparisonEpsilon;
}

void commitPendingHandoffs(
    NavigationTraversalGraph& graph,
    const BoundarySegment& boundary,
    std::vector<PendingHandoff>& pending,
    const std::vector<TriangleRecord>& triangles,
    const NavigationGraphBuildOptions& options) {
    if (pending.empty()) {
        return;
    }
    std::vector<PendingHandoff> merged{};
    merged.reserve(pending.size());
    for (const PendingHandoff& candidate : pending) {
        if (!merged.empty() && canMerge(merged.back(), candidate)) {
            merged.back().secondParameter = candidate.secondParameter;
        } else {
            merged.push_back(candidate);
        }
    }

    const NavigationVec3 boundaryVector = subtract(boundary.second, boundary.first);
    for (const PendingHandoff& candidate : merged) {
        const NavigationPortalSegment portal{
            .first = add(boundary.first, multiply(boundaryVector, candidate.firstParameter)),
            .second = add(boundary.first, multiply(boundaryVector, candidate.secondParameter)),
        };
        if (length(subtract(portal.second, portal.first)) < options.minimumPortalLength) {
            continue;
        }

        NavigationCollisionHandoff handoff{
            .sourceTriangle = triangles[candidate.sourceTriangleRecordIndex].key,
            .targetTriangle = triangles[candidate.targetTriangleRecordIndex].key,
            .portal = portal,
            .kind = candidate.kind,
            .availability = candidate.availability,
            .sourceEntryId = candidate.sourceEntryId,
            .targetEntryId = candidate.targetEntryId,
            .authoredFallbackChainIndex = candidate.chainIndex,
            .authoredFallbackTargetIndex = candidate.targetIndex,
            .authoredOrdinal = candidate.authoredOrdinal,
        };
        const std::size_t handoffIndex = appendHandoffIfUnique(
            graph,
            std::move(handoff),
            options.planarContainmentTolerance);
        const NavigationCollisionHandoff& stored = graph.collisionHandoffs[handoffIndex];
        if (stored.availability != NavigationCollisionHandoffAvailability::ActiveStatic) {
            continue;
        }
        const auto sourceNode = graph.findNodeIndex(stored.sourceTriangle);
        const auto targetNode = graph.findNodeIndex(stored.targetTriangle);
        if (!sourceNode.has_value() || !targetNode.has_value()) {
            continue;
        }
        const auto addDirectedEdge = [&](const std::size_t source, const std::size_t target) {
            static_cast<void>(addEdgeIfUnique(graph.nodes[source], NavigationGraphEdge{
                .targetNodeIndex = target,
                .portal = stored.portal,
                .kind = NavigationGraphEdgeKind::CollisionHandoff,
                .collisionHandoffIndex = handoffIndex,
            }));
        };
        addDirectedEdge(*sourceNode, *targetNode);
        if (stored.kind == NavigationCollisionHandoffKind::SameEntryBundle) {
            addDirectedEdge(*targetNode, *sourceNode);
        }
    }
}

[[nodiscard]] auto edgeSortKey(const NavigationGraphEdge& edge) noexcept {
    return std::tuple{
        edge.targetNodeIndex,
        static_cast<int>(edge.kind),
        edge.collisionHandoffIndex.value_or(std::numeric_limits<std::size_t>::max()),
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
    graph.statistics.authoredFallbackChainCount = area.authoredGroundFallbackChains.size();
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
    effectiveOptions.planarContainmentTolerance =
        std::max(effectiveOptions.planarContainmentTolerance, 0.0F);
    effectiveOptions.outwardProbeDistance =
        std::max(effectiveOptions.outwardProbeDistance, 0.0F);
    effectiveOptions.heightContinuityTolerance =
        std::max(effectiveOptions.heightContinuityTolerance, 0.0F);
    effectiveOptions.distinctHeightTieTolerance =
        std::max(effectiveOptions.distinctHeightTieTolerance, kComparisonEpsilon);
    effectiveOptions.minimumPortalLength =
        std::max(effectiveOptions.minimumPortalLength, kComparisonEpsilon);
    effectiveOptions.minimumUpNormalComponent =
        std::max(effectiveOptions.minimumUpNormalComponent, kComparisonEpsilon);

    const PlanarBasis basis = makePlanarBasis(coordinatePolicy.upAxis());
    std::vector<TriangleRecord> triangleRecords{};
    std::vector<std::vector<std::size_t>> trianglesBySurface(area.surfaces.size());
    std::vector<std::vector<BoundarySegment>> boundariesBySurface(area.surfaces.size());

    for (std::size_t surfaceIndex = 0; surfaceIndex < area.surfaces.size(); ++surfaceIndex) {
        const NavigationSurface& surface = area.surfaces[surfaceIndex];
        if (surface.traversalAvailability == NavigationSurfaceTraversalAvailability::RequiresRuntimeState) {
            ++graph.statistics.runtimeDependentSurfaceCount;
        }
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

            const std::size_t recordIndex = triangleRecords.size();
            triangleRecords.push_back(TriangleRecord{
                .key = NavigationTriangleKey{ surfaceIndex, triangleIndex },
                .vertices = triangle,
                .planeNormal = triangleCross,
                .unitNormal = normalized(triangleCross),
            });
            trianglesBySurface[surfaceIndex].push_back(recordIndex);

            for (std::size_t edgeIndex = 0; edgeIndex < 3U; ++edgeIndex) {
                const std::size_t nextEdgeIndex = (edgeIndex + 1U) % 3U;
                const std::size_t oppositeIndex = (edgeIndex + 2U) % 3U;
                const std::size_t firstCanonical = canonicalVertices[indices[edgeIndex]];
                const std::size_t secondCanonical = canonicalVertices[indices[nextEdgeIndex]];
                const bool canonicalOrder = firstCanonical < secondCanonical;
                const CanonicalEdge edge{
                    .first = std::min(firstCanonical, secondCanonical),
                    .second = std::max(firstCanonical, secondCanonical),
                };
                edgeOccurrences[edge].push_back(EdgeOccurrence{
                    .triangleRecordIndex = recordIndex,
                    .first = canonicalOrder ? triangle[edgeIndex] : triangle[nextEdgeIndex],
                    .second = canonicalOrder ? triangle[nextEdgeIndex] : triangle[edgeIndex],
                    .opposite = triangle[oppositeIndex],
                });
            }
        }

        std::unordered_set<std::size_t> nonManifoldTriangles{};
        for (const auto& [edge, occurrences] : edgeOccurrences) {
            static_cast<void>(edge);
            if (occurrences.size() <= 2U) {
                continue;
            }
            ++graph.statistics.nonManifoldEdgeCount;
            for (const EdgeOccurrence& occurrence : occurrences) {
                nonManifoldTriangles.emplace(occurrence.triangleRecordIndex);
            }
            appendDiagnostic(graph, NavigationDiagnosticSeverity::Warning,
                "Surface " + std::to_string(surfaceIndex) +
                " contains a non-manifold edge shared by " + std::to_string(occurrences.size()) +
                " triangles; the incident triangles were omitted.");
        }
        for (const std::size_t recordIndex : nonManifoldTriangles) {
            triangleRecords[recordIndex].retained = false;
        }
        graph.statistics.skippedNonManifoldTriangleCount += nonManifoldTriangles.size();
        for (auto& [edge, occurrences] : edgeOccurrences) {
            static_cast<void>(edge);
            std::erase_if(occurrences, [&](const EdgeOccurrence& occurrence) {
                return !triangleRecords[occurrence.triangleRecordIndex].retained;
            });
        }

        const bool activeSurface =
            surface.traversalAvailability == NavigationSurfaceTraversalAvailability::Static;
        if (activeSurface) {
            for (const std::size_t recordIndex : trianglesBySurface[surfaceIndex]) {
                TriangleRecord& record = triangleRecords[recordIndex];
                if (!record.retained) {
                    continue;
                }
                record.nodeIndex = graph.nodes.size();
                graph.nodes.push_back(NavigationGraphNode{
                    .key = record.key,
                    .vertices = record.vertices,
                    .centroid = multiply(add(add(record.vertices[0], record.vertices[1]), record.vertices[2]),
                        1.0F / 3.0F),
                    .normal = normalized(coordinatePolicy.convertDirection(record.planeNormal)),
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
                    .triangleRecordIndex = occurrences[0].triangleRecordIndex,
                    .first = occurrences[0].first,
                    .second = occurrences[0].second,
                    .opposite = occurrences[0].opposite,
                });
                continue;
            }
            if (occurrences.size() > 2U || !activeSurface) {
                continue;
            }

            const TriangleRecord& firstTriangle = triangleRecords[occurrences[0].triangleRecordIndex];
            const TriangleRecord& secondTriangle = triangleRecords[occurrences[1].triangleRecordIndex];
            if (!firstTriangle.nodeIndex.has_value() || !secondTriangle.nodeIndex.has_value()) {
                continue;
            }
            const NavigationPortalSegment portal{
                .first = multiply(add(occurrences[0].first, occurrences[1].first), 0.5F),
                .second = multiply(add(occurrences[0].second, occurrences[1].second), 0.5F),
            };
            const bool forwardAdded = addEdgeIfUnique(graph.nodes[*firstTriangle.nodeIndex], NavigationGraphEdge{
                .targetNodeIndex = *secondTriangle.nodeIndex,
                .portal = portal,
                .kind = NavigationGraphEdgeKind::IntraSurface,
            });
            const bool reverseAdded = addEdgeIfUnique(graph.nodes[*secondTriangle.nodeIndex], NavigationGraphEdge{
                .targetNodeIndex = *firstTriangle.nodeIndex,
                .portal = portal,
                .kind = NavigationGraphEdgeKind::IntraSurface,
            });
            if (forwardAdded || reverseAdded) {
                ++graph.statistics.intraSurfaceConnectionCount;
            }
        }
    }

    std::unordered_map<std::size_t, std::vector<std::size_t>> surfacesByTableIndex{};
    for (std::size_t surfaceIndex = 0; surfaceIndex < area.surfaces.size(); ++surfaceIndex) {
        surfacesByTableIndex[area.surfaces[surfaceIndex].sourceTableIndex].push_back(surfaceIndex);
    }
    std::unordered_map<std::size_t, std::size_t> chainBySourceTableIndex{};
    for (std::size_t chainIndex = 0; chainIndex < area.authoredGroundFallbackChains.size(); ++chainIndex) {
        const NavigationAuthoredGroundFallbackChain& chain = area.authoredGroundFallbackChains[chainIndex];
        chainBySourceTableIndex.emplace(chain.sourceTableIndex, chainIndex);
        graph.statistics.authoredFallbackTargetCount += chain.targets.size();
    }

    std::size_t probeRefinementExhaustionCount = 0U;
    for (std::size_t sourceSurfaceIndex = 0; sourceSurfaceIndex < area.surfaces.size(); ++sourceSurfaceIndex) {
        const NavigationSurface& sourceSurface = area.surfaces[sourceSurfaceIndex];
        const auto bundleFound = surfacesByTableIndex.find(sourceSurface.sourceTableIndex);
        if (bundleFound == surfacesByTableIndex.end()) {
            continue;
        }
        const std::vector<std::size_t> currentBundleTriangles =
            triangleIndicesForSurfaces(bundleFound->second, trianglesBySurface);
        const bool currentBundleHasRuntimeUncertainty = anySurfaceRequiresRuntimeState(
            area,
            bundleFound->second,
            sourceSurfaceIndex);

        const NavigationAuthoredGroundFallbackChain* chain = nullptr;
        std::optional<std::size_t> chainIndex{};
        const auto chainFound = chainBySourceTableIndex.find(sourceSurface.sourceTableIndex);
        if (chainFound != chainBySourceTableIndex.end()) {
            chainIndex = chainFound->second;
            chain = &area.authoredGroundFallbackChains[*chainIndex];
        }

        std::vector<std::vector<std::size_t>> targetTriangles{};
        std::vector<bool> targetHasRuntimeUncertainty{};
        if (chain != nullptr) {
            targetTriangles.reserve(chain->targets.size());
            targetHasRuntimeUncertainty.reserve(chain->targets.size());
            for (const NavigationAuthoredGroundFallbackTarget& target : chain->targets) {
                std::vector<std::size_t> surfaces = surfaceIndicesForKeys(area, target.targetSurfaces);
                targetTriangles.push_back(triangleIndicesForSurfaces(surfaces, trianglesBySurface));
                targetHasRuntimeUncertainty.push_back(anySurfaceRequiresRuntimeState(area, surfaces));
            }
        }

        for (const BoundarySegment& boundary : boundariesBySurface[sourceSurfaceIndex]) {
            const TriangleRecord& sourceTriangle = triangleRecords[boundary.triangleRecordIndex];
            const PlanarPoint planarFirst = basis.project(boundary.first);
            const PlanarPoint planarSecond = basis.project(boundary.second);
            const PlanarPoint planarVector = subtract(planarSecond, planarFirst);
            const float planarLength = length(planarVector);
            if (planarLength < effectiveOptions.minimumPortalLength) {
                continue;
            }
            const PlanarPoint opposite = basis.project(boundary.opposite);
            const float interiorSide = cross2(planarVector, subtract(opposite, planarFirst));
            if (std::abs(interiorSide) <= kComparisonEpsilon) {
                continue;
            }
            const PlanarPoint rightNormal{ planarVector.y / planarLength, -planarVector.x / planarLength };
            const PlanarPoint outward = interiorSide > 0.0F
                ? rightNormal
                : multiply(rightNormal, -1.0F);

            std::vector<std::size_t> partitionCandidates = currentBundleTriangles;
            for (const auto& targetSet : targetTriangles) {
                partitionCandidates.insert(partitionCandidates.end(), targetSet.begin(), targetSet.end());
            }
            std::sort(partitionCandidates.begin(), partitionCandidates.end());
            partitionCandidates.erase(
                std::unique(partitionCandidates.begin(), partitionCandidates.end()),
                partitionCandidates.end());
            const std::vector<float> baseParameters = partitionBoundary(
                boundary,
                partitionCandidates,
                triangleRecords,
                basis,
                outward,
                effectiveOptions);

            std::vector<PendingHandoff> pending{};
            for (std::size_t baseInterval = 1; baseInterval < baseParameters.size(); ++baseInterval) {
                const float baseFirstParameter = baseParameters[baseInterval - 1U];
                const float baseSecondParameter = baseParameters[baseInterval];
                std::vector<BoundaryProbeInterval> probeIntervals{};
                appendRefinedBoundaryProbeIntervals(
                    probeIntervals,
                    probeRefinementExhaustionCount,
                    boundary,
                    baseFirstParameter,
                    baseSecondParameter,
                    planarLength,
                    partitionCandidates,
                    triangleRecords,
                    basis,
                    outward,
                    effectiveOptions);

                for (const BoundaryProbeInterval& probeInterval : probeIntervals) {
                    const float firstParameter = probeInterval.firstParameter;
                    const float secondParameter = probeInterval.secondParameter;
                    const float intervalLength = (secondParameter - firstParameter) * planarLength;
                    if (intervalLength < effectiveOptions.minimumPortalLength) {
                        continue;
                    }
                    const float probeDistance = probeInterval.probeDistance;
                    const float midpointParameter = (firstParameter + secondParameter) * 0.5F;
                    const NavigationVec3 sourcePoint = add(
                        boundary.first,
                        multiply(subtract(boundary.second, boundary.first), midpointParameter));
                    const float sourceHeight = dot(sourcePoint, basis.up);
                    const PlanarPoint probePoint = add(
                        add(planarFirst, multiply(planarVector, midpointParameter)),
                        multiply(outward, probeDistance));

                    const CollisionQueryResult currentHit = queryTriangles(
                        currentBundleTriangles,
                        triangleRecords,
                        probePoint,
                        sourceHeight,
                        basis,
                        effectiveOptions,
                        boundary.triangleRecordIndex);
                    if (currentHit.status != CollisionQueryStatus::NoHit) {
                        if (currentHit.status == CollisionQueryStatus::Ambiguous ||
                            !currentHit.triangleRecordIndex.has_value() ||
                            !handoffIsContinuous(
                                boundary,
                                triangleRecords[*currentHit.triangleRecordIndex],
                                outward,
                                firstParameter,
                                secondParameter,
                                probeDistance,
                                basis,
                                effectiveOptions)) {
                            ++graph.statistics.unresolvedHandoffCount;
                            continue;
                        }
                        const TriangleRecord& targetTriangle = triangleRecords[*currentHit.triangleRecordIndex];
                        pending.push_back(PendingHandoff{
                            .sourceTriangleRecordIndex = boundary.triangleRecordIndex,
                            .targetTriangleRecordIndex = *currentHit.triangleRecordIndex,
                            .firstParameter = firstParameter,
                            .secondParameter = secondParameter,
                            .kind = NavigationCollisionHandoffKind::SameEntryBundle,
                            .availability = (currentBundleHasRuntimeUncertainty ||
                                targetRequiresRuntimeState(area, sourceTriangle, targetTriangle))
                                ? NavigationCollisionHandoffAvailability::RequiresRuntimeState
                                : NavigationCollisionHandoffAvailability::ActiveStatic,
                            .sourceEntryId = sourceSurface.sourceKey.sourceEntryId,
                            .targetEntryId = area.surfaces[targetTriangle.key.surfaceIndex].sourceKey.sourceEntryId,
                        });
                        continue;
                    }

                    if (chain == nullptr) {
                        continue;
                    }
                    if (currentBundleHasRuntimeUncertainty) {
                        ++graph.statistics.runtimeStateBlockedIntervalCount;
                        continue;
                    }
                    for (std::size_t targetIndex = 0; targetIndex < chain->targets.size(); ++targetIndex) {
                        const NavigationAuthoredGroundFallbackTarget& target = chain->targets[targetIndex];
                        if (target.status == NavigationAuthoredGroundFallbackTargetStatus::MissingEntry ||
                            target.status ==
                                NavigationAuthoredGroundFallbackTargetStatus::SuppressedAfterMissingEntry) {
                            break;
                        }
                        if (target.status == NavigationAuthoredGroundFallbackTargetStatus::MissingGeometry) {
                            continue;
                        }
                        const CollisionQueryResult targetHit = queryTriangles(
                            targetTriangles[targetIndex],
                            triangleRecords,
                            probePoint,
                            sourceHeight,
                            basis,
                            effectiveOptions);
                        if (targetHit.status == CollisionQueryStatus::NoHit) {
                            if (targetHasRuntimeUncertainty[targetIndex]) {
                                ++graph.statistics.runtimeStateBlockedIntervalCount;
                                break;
                            }
                            continue;
                        }

                        for (std::size_t laterTarget = targetIndex + 1U;
                             laterTarget < chain->targets.size();
                             ++laterTarget) {
                            if (chain->targets[laterTarget].status !=
                                NavigationAuthoredGroundFallbackTargetStatus::Resolved) {
                                continue;
                            }
                            if (queryTriangles(
                                    targetTriangles[laterTarget],
                                    triangleRecords,
                                    probePoint,
                                    sourceHeight,
                                    basis,
                                    effectiveOptions).status != CollisionQueryStatus::NoHit) {
                                ++graph.statistics.priorityShadowedCandidateCount;
                            }
                        }

                        if (targetHit.status == CollisionQueryStatus::Ambiguous ||
                            !targetHit.triangleRecordIndex.has_value() ||
                            !handoffIsContinuous(
                                boundary,
                                triangleRecords[*targetHit.triangleRecordIndex],
                                outward,
                                firstParameter,
                                secondParameter,
                                probeDistance,
                                basis,
                                effectiveOptions)) {
                            ++graph.statistics.unresolvedHandoffCount;
                            break;
                        }
                        const TriangleRecord& targetTriangle = triangleRecords[*targetHit.triangleRecordIndex];
                        pending.push_back(PendingHandoff{
                            .sourceTriangleRecordIndex = boundary.triangleRecordIndex,
                            .targetTriangleRecordIndex = *targetHit.triangleRecordIndex,
                            .firstParameter = firstParameter,
                            .secondParameter = secondParameter,
                            .kind = NavigationCollisionHandoffKind::AuthoredFallback,
                            .availability = (targetHasRuntimeUncertainty[targetIndex] ||
                                targetRequiresRuntimeState(area, sourceTriangle, targetTriangle))
                                ? NavigationCollisionHandoffAvailability::RequiresRuntimeState
                                : NavigationCollisionHandoffAvailability::ActiveStatic,
                            .sourceEntryId = chain->sourceEntryId,
                            .targetEntryId = target.targetEntryId,
                            .chainIndex = chainIndex,
                            .targetIndex = targetIndex,
                            .authoredOrdinal = target.authoredOrdinal,
                        });
                        break;
                    }
                }
            }
            commitPendingHandoffs(
                graph,
                boundary,
                pending,
                triangleRecords,
                effectiveOptions);
        }
    }

    if (graph.statistics.runtimeDependentSurfaceCount > 0U) {
        appendDiagnostic(graph, NavigationDiagnosticSeverity::Info,
            std::to_string(graph.statistics.runtimeDependentSurfaceCount) +
            " runtime-dependent ground surface(s) were retained for conditional collision evidence "
            "but excluded from pathfinding nodes.");
    }
    if (graph.statistics.unresolvedHandoffCount > 0U) {
        appendDiagnostic(graph, NavigationDiagnosticSeverity::Warning,
            std::to_string(graph.statistics.unresolvedHandoffCount) +
            " collision handoff interval(s) were left unresolved because the first collision "
            "candidate was ambiguous or height-discontinuous.");
    }
    if (graph.statistics.runtimeStateBlockedIntervalCount > 0U) {
        appendDiagnostic(graph, NavigationDiagnosticSeverity::Info,
            std::to_string(graph.statistics.runtimeStateBlockedIntervalCount) +
            " collision handoff interval(s) could not be evaluated past an earlier "
            "runtime-dependent collision provider; no later static fallback was activated.");
    }
    if (probeRefinementExhaustionCount > 0U) {
        appendDiagnostic(graph, NavigationDiagnosticSeverity::Warning,
            std::to_string(probeRefinementExhaustionCount) +
            " collision handoff interval(s) exceeded the adaptive probe-refinement limit and "
            "were omitted rather than accepting an uncertain portal.");
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
