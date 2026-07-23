#include "Graph/NavigationTraversalGraph.h"
#include "Loading/NavigationAreaLoader.h"
#include "Pathfinding/NavigationPathfinder.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace savor::navigation {
namespace {

[[nodiscard]] NavigationMesh makeMesh(
    const std::vector<NavigationVec3>& positions,
    std::vector<std::uint32_t> indices) {
    NavigationMesh mesh{};
    mesh.indices = std::move(indices);
    mesh.vertices.reserve(positions.size());
    for (const NavigationVec3& position : positions) {
        mesh.vertices.push_back(NavigationMeshVertex{ .position = position });
    }
    return mesh;
}

[[nodiscard]] NavigationSurface makeSurface(
    const std::uint32_t entryId,
    const std::uint32_t blockOffset,
    NavigationMesh mesh,
    const std::int32_t tblId = 0,
    const NavigationSurfaceSourceKind sourceKind = NavigationSurfaceSourceKind::Grnd,
    const NavigationSurfaceTraversalAvailability traversalAvailability =
        NavigationSurfaceTraversalAvailability::Static,
    const std::optional<std::size_t> sourceTableIndex = std::nullopt) {
    return NavigationSurface{
        .sourceKey = NavigationSurfaceSourceKey{
            .sourceEntryId = entryId,
            .sourceBlockOffset = blockOffset,
            .sourceNodeOffset = 0,
        },
        .sourceKind = sourceKind,
        .traversalAvailability = traversalAvailability,
        .sourceTableIndex = sourceTableIndex.value_or(static_cast<std::size_t>(entryId)),
        .tblId = tblId,
        .mesh = std::move(mesh),
    };
}

void addFallback(
    NavigationAreaModel& area,
    const std::size_t sourceSurfaceIndex,
    const std::vector<std::vector<std::size_t>>& orderedTargetSurfaceGroups) {
    const NavigationSurface& source = area.surfaces[sourceSurfaceIndex];
    NavigationAuthoredGroundFallbackChain chain{
        .sourceTableIndex = source.sourceTableIndex,
        .sourceEntryId = source.sourceKey.sourceEntryId,
        .sourceSurfaces = { source.sourceKey },
    };
    for (std::size_t ordinal = 0; ordinal < orderedTargetSurfaceGroups.size(); ++ordinal) {
        const auto& targetIndices = orderedTargetSurfaceGroups[ordinal];
        ASSERT_FALSE(targetIndices.empty());
        const NavigationSurface& firstTarget = area.surfaces[targetIndices.front()];
        NavigationAuthoredGroundFallbackTarget target{
            .targetEntryId = firstTarget.sourceKey.sourceEntryId,
            .authoredOrdinal = ordinal,
            .targetTableIndex = firstTarget.sourceTableIndex,
            .status = NavigationAuthoredGroundFallbackTargetStatus::Resolved,
        };
        for (const std::size_t targetIndex : targetIndices) {
            target.targetSurfaces.push_back(area.surfaces[targetIndex].sourceKey);
        }
        chain.targets.push_back(std::move(target));
    }
    area.authoredGroundFallbackChains.push_back(std::move(chain));
}

[[nodiscard]] NavigationAuthoredGroundFallbackTarget makeFallbackTarget(
    const NavigationAreaModel& area,
    const std::uint32_t targetEntryId,
    const std::size_t authoredOrdinal,
    const NavigationAuthoredGroundFallbackTargetStatus status,
    const std::vector<std::size_t>& targetSurfaceIndices = {},
    const std::optional<std::size_t> targetTableIndex = std::nullopt) {
    NavigationAuthoredGroundFallbackTarget target{
        .targetEntryId = targetEntryId,
        .authoredOrdinal = authoredOrdinal,
        .targetTableIndex = targetTableIndex,
        .status = status,
    };
    for (const std::size_t surfaceIndex : targetSurfaceIndices) {
        target.targetSurfaces.push_back(area.surfaces[surfaceIndex].sourceKey);
        if (!target.targetTableIndex.has_value()) {
            target.targetTableIndex = area.surfaces[surfaceIndex].sourceTableIndex;
        }
    }
    return target;
}

[[nodiscard]] NavigationStartOption makeStartOption(
    const std::int32_t groundTblId,
    const NavigationVec3 position,
    const std::optional<float> yawDegrees = std::nullopt) {
    NavigationStartOption option{};
    option.id = "test-start";
    option.label = "Test start";
    option.availability = NavigationStartAvailability::Resolvable;
    option.groundTblId = groundTblId;
    option.position = position;
    option.yawDegrees = yawDegrees;
    return option;
}

[[nodiscard]] NavigationAreaModel makeCompleteArea(std::vector<NavigationSurface> surfaces) {
    NavigationAreaModel area{};
    area.surfaces = std::move(surfaces);
    area.hasCompleteGroundGeometry = true;
    area.hasCompleteWallGeometry = true;
    return area;
}

[[nodiscard]] NavigationRegionMesh makeRegionMesh(NavigationMesh mesh) {
    return NavigationRegionMesh{ .mesh = std::move(mesh) };
}

[[nodiscard]] NavigationRegion makeRegion(
    const NavigationRegionKind kind,
    std::vector<NavigationRegionMesh> meshes = {}) {
    NavigationRegion region{};
    region.kind = kind;
    region.meshes = std::move(meshes);
    return region;
}

[[nodiscard]] bool hasDiagnosticContaining(
    const NavigationTraversalGraph& graph,
    const std::string_view needle) {
    for (const NavigationDiagnostic& diagnostic : graph.diagnostics) {
        if (diagnostic.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] NavigationGraphAnchor anchorForNode(
    const NavigationTraversalGraph& graph,
    const std::size_t nodeIndex,
    const NavigationVec3& point) {
    return NavigationGraphAnchor{
        .triangle = graph.nodes[nodeIndex].key,
        .nodeIndex = nodeIndex,
        .exactPoint = point,
        .snappedPoint = point,
    };
}

TEST(NavigationGraphBuilder, JoinsIndexedAndWeldedTriangleBoundaries) {
    auto area = makeCompleteArea({ makeSurface(1, 100, makeMesh(
        {
            { 0.0F, 0.0F, 0.0F },
            { 1.0F, 0.0F, 0.0F },
            { 0.0F, 0.0F, 1.0F },
            { 1.0005F, 0.0F, 0.0F },
            { 1.0F, 0.0F, 1.0F },
            { 0.0005F, 0.0F, 1.0F },
        },
        { 0, 1, 2, 3, 4, 5 })) });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    ASSERT_EQ(graph.nodes.size(), 2U);
    EXPECT_EQ(graph.statistics.intraSurfaceConnectionCount, 1U);
    ASSERT_EQ(graph.nodes[0].edges.size(), 1U);
    ASSERT_EQ(graph.nodes[1].edges.size(), 1U);
    EXPECT_EQ(graph.nodes[0].edges[0].targetNodeIndex, 1U);
    EXPECT_EQ(graph.nodes[1].edges[0].targetNodeIndex, 0U);
    EXPECT_EQ(graph.nodes[0].edges[0].kind, NavigationGraphEdgeKind::IntraSurface);
    EXPECT_EQ(graph.findNodeIndex(NavigationTriangleKey{ 0, 1 }), 1U);
}

TEST(NavigationGraphBuilder, LeavesStackedSurfacesDisconnectedWithoutAuthoredFallback) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
        makeSurface(2, 200, makeMesh(
            { { 0.0F, 1.0F, 0.0F }, { 1.0F, 1.0F, 0.0F }, { 0.0F, 1.0F, 1.0F } },
            { 0, 1, 2 })),
    });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    ASSERT_EQ(graph.nodes.size(), 2U);
    EXPECT_TRUE(graph.nodes[0].edges.empty());
    EXPECT_TRUE(graph.nodes[1].edges.empty());
    EXPECT_TRUE(graph.collisionHandoffs.empty());
}

TEST(NavigationGraphBuilder, CreatesDirectedFallbackFromCollisionCoverageBeyondSourceBoundary) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            {
                { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F },
                { 1.0F, 0.0F, 1.0F }, { 0.0F, 0.0F, 1.0F },
            },
            { 0, 1, 2, 0, 2, 3 })),
        makeSurface(2, 200, makeMesh(
            {
                { 0.0F, 0.0F, 0.8F }, { 1.0F, 0.0F, 0.8F },
                { 1.0F, 0.0F, 2.0F }, { 0.0F, 0.0F, 2.0F },
            },
            { 0, 1, 2, 0, 2, 3 }),
            0,
            NavigationSurfaceSourceKind::Gobj),
    });
    addFallback(area, 0, { { 1 } });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    ASSERT_EQ(graph.nodes.size(), 4U);
    ASSERT_FALSE(graph.collisionHandoffs.empty());
    EXPECT_EQ(graph.statistics.authoredFallbackHandoffCount, graph.collisionHandoffs.size());
    EXPECT_EQ(graph.collisionHandoffs.front().kind, NavigationCollisionHandoffKind::AuthoredFallback);
    EXPECT_EQ(graph.collisionHandoffs.front().sourceEntryId, 1U);
    EXPECT_EQ(graph.collisionHandoffs.front().targetEntryId, 2U);
    bool hasForwardHandoff = false;
    bool hasReverseHandoff = false;
    for (std::size_t sourceNode = 0; sourceNode < graph.nodes.size(); ++sourceNode) {
        for (const NavigationGraphEdge& edge : graph.nodes[sourceNode].edges) {
            if (edge.kind != NavigationGraphEdgeKind::CollisionHandoff) {
                continue;
            }
            hasForwardHandoff |= sourceNode < 2U && edge.targetNodeIndex >= 2U;
            hasReverseHandoff |= sourceNode >= 2U && edge.targetNodeIndex < 2U;
        }
    }
    EXPECT_TRUE(hasForwardHandoff);
    EXPECT_FALSE(hasReverseHandoff);
}

TEST(NavigationGraphBuilder, RefinesShortSlopedCoverageAtTheCappedProbeDistance) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            {
                { 0.0F, 0.0F, 0.0F }, { 0.03F, 0.0F, 0.0F },
                { 0.0F, 0.0F, -0.03F },
            },
            { 0, 1, 2 })),
        makeSurface(2, 200, makeMesh(
            {
                { 0.0F, 0.0F, 0.0F }, { 0.03F, 0.0F, 0.01F },
                { 0.03F, 0.0F, 0.0F },
            },
            { 0, 1, 2 })),
    });
    addFallback(area, 0, { { 1 } });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    const auto handoff = std::find_if(
        graph.collisionHandoffs.begin(),
        graph.collisionHandoffs.end(),
        [](const NavigationCollisionHandoff& candidate) {
            return candidate.kind == NavigationCollisionHandoffKind::AuthoredFallback &&
                candidate.sourceTriangle == NavigationTriangleKey{ 0, 0 } &&
                candidate.targetTriangle == NavigationTriangleKey{ 1, 0 } &&
                std::abs(candidate.portal.first.z) < 1.0e-6F &&
                std::abs(candidate.portal.second.z) < 1.0e-6F;
        });
    ASSERT_NE(handoff, graph.collisionHandoffs.end());
    EXPECT_GE(std::abs(handoff->portal.second.x - handoff->portal.first.x), 1.0e-3F);
    EXPECT_NEAR(std::max(handoff->portal.first.x, handoff->portal.second.x), 0.03F, 1.0e-5F);
}

TEST(NavigationGraphBuilder, DoesNotRejectTheCoveredPartOfAShortSlantedWedge) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            {
                { 0.0F, 0.0F, 0.0F }, { 0.03F, 0.0F, 0.0F },
                { 0.0F, 0.0F, -0.03F },
            },
            { 0, 1, 2 })),
        makeSurface(2, 200, makeMesh(
            {
                { 0.0F, 0.0F, 0.0F }, { 0.03F, 0.0F, 0.0F },
                { 0.03F, 0.0F, 0.03F },
            },
            { 0, 1, 2 })),
    });
    addFallback(area, 0, { { 1 } });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    const bool hasSourceEdgeHandoff = std::any_of(
        graph.collisionHandoffs.begin(),
        graph.collisionHandoffs.end(),
        [](const NavigationCollisionHandoff& candidate) {
            return candidate.kind == NavigationCollisionHandoffKind::AuthoredFallback &&
                candidate.sourceTriangle == NavigationTriangleKey{ 0, 0 } &&
                candidate.targetTriangle == NavigationTriangleKey{ 1, 0 } &&
                std::abs(candidate.portal.first.z) < 1.0e-6F &&
                std::abs(candidate.portal.second.z) < 1.0e-6F;
        });
    EXPECT_TRUE(hasSourceEdgeHandoff);
    EXPECT_EQ(graph.statistics.unresolvedHandoffCount, 0U);
}

TEST(NavigationGraphBuilder, LeavesTiedStackedFallbackCoverageUnresolved) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            {
                { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F },
                { 1.0F, 0.0F, 1.0F }, { 0.0F, 0.0F, 1.0F },
            },
            { 0, 1, 2, 0, 2, 3 })),
        makeSurface(2, 200, makeMesh(
            {
                { 0.0F, 0.002F, 0.8F }, { 1.0F, 0.002F, 0.8F },
                { 1.0F, 0.002F, 2.0F }, { 0.0F, 0.002F, 2.0F },
            },
            { 0, 1, 2, 0, 2, 3 }), 0, NavigationSurfaceSourceKind::Grnd,
            NavigationSurfaceTraversalAvailability::Static, 2),
        makeSurface(2, 300, makeMesh(
            {
                { 0.0F, -0.002F, 0.8F }, { 1.0F, -0.002F, 0.8F },
                { 1.0F, -0.002F, 2.0F }, { 0.0F, -0.002F, 2.0F },
            },
            { 0, 1, 2, 0, 2, 3 }), 0, NavigationSurfaceSourceKind::Grnd,
            NavigationSurfaceTraversalAvailability::Static, 2),
    });
    addFallback(area, 0, { { 1, 2 } });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    EXPECT_TRUE(graph.collisionHandoffs.empty());
    EXPECT_GT(graph.statistics.unresolvedHandoffCount, 0U);
}

TEST(NavigationGraphBuilder, RejectsVerticalTrianglesAsFallbackCoverage) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            {
                { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F },
                { 1.0F, 0.0F, 1.0F }, { 0.0F, 0.0F, 1.0F },
            },
            { 0, 1, 2, 0, 2, 3 })),
        makeSurface(2, 200, makeMesh(
            {
                { 0.0F, 0.0F, 1.005F }, { 1.0F, 0.0F, 1.005F },
                { 1.0F, 1.0F, 1.005F },
            },
            { 0, 1, 2 })),
    });
    addFallback(area, 0, { { 1 } });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    ASSERT_EQ(graph.nodes.size(), 3U);
    EXPECT_TRUE(graph.collisionHandoffs.empty());
}

TEST(NavigationGraphBuilder, CoincidentFootprintsWithoutOutwardCoverageDoNotContinue) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
        makeSurface(2, 200, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
    });
    addFallback(area, 0, { { 1 } });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    EXPECT_TRUE(graph.collisionHandoffs.empty());
    EXPECT_EQ(graph.statistics.authoredFallbackHandoffCount, 0U);
}

TEST(NavigationGraphBuilder, MergesDeterministicAdjacentIntervalsWithMatchingProvenance) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
        makeSurface(2, 200, makeMesh(
            { { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
        makeSurface(3, 300, makeMesh(
            {
                { 0.45F, 0.0F, 0.55F }, { 0.55F, 0.0F, 0.45F },
                { 0.55F, 0.0F, 0.55F },
            },
            { 0, 1, 2 })),
    });
    addFallback(area, 0, { { 1 }, { 2 } });

    NavigationGraphBuildOptions options{};
    options.outwardProbeDistance = 0.0F;
    const NavigationTraversalGraph first = NavigationGraphBuilder{}.build(
        area, NavigationCoordinatePolicy::identity(), options);
    const NavigationTraversalGraph second = NavigationGraphBuilder{}.build(
        area, NavigationCoordinatePolicy::identity(), options);

    const auto matchingHandoffs = [](const NavigationTraversalGraph& graph) {
        std::vector<const NavigationCollisionHandoff*> matches{};
        for (const NavigationCollisionHandoff& handoff : graph.collisionHandoffs) {
            if (handoff.kind == NavigationCollisionHandoffKind::AuthoredFallback &&
                handoff.sourceTriangle == NavigationTriangleKey{ 0, 0 } &&
                handoff.targetTriangle == NavigationTriangleKey{ 1, 0 } &&
                std::abs((handoff.portal.first.x + handoff.portal.first.z) - 1.0F) < 1.0e-5F &&
                std::abs((handoff.portal.second.x + handoff.portal.second.z) - 1.0F) < 1.0e-5F) {
                matches.push_back(&handoff);
            }
        }
        return matches;
    };
    const auto firstMatches = matchingHandoffs(first);
    const auto secondMatches = matchingHandoffs(second);
    ASSERT_EQ(firstMatches.size(), 1U);
    ASSERT_EQ(secondMatches.size(), 1U);
    EXPECT_EQ(firstMatches[0]->authoredFallbackTargetIndex, 0U);
    EXPECT_EQ(firstMatches[0]->authoredOrdinal, 0U);
    EXPECT_NEAR(firstMatches[0]->portal.first.x, secondMatches[0]->portal.first.x, 1.0e-6F);
    EXPECT_NEAR(firstMatches[0]->portal.first.z, secondMatches[0]->portal.first.z, 1.0e-6F);
    EXPECT_NEAR(firstMatches[0]->portal.second.x, secondMatches[0]->portal.second.x, 1.0e-6F);
    EXPECT_NEAR(firstMatches[0]->portal.second.z, secondMatches[0]->portal.second.z, 1.0e-6F);
    EXPECT_NEAR(
        std::abs(firstMatches[0]->portal.second.x - firstMatches[0]->portal.first.x),
        1.0F,
        1.0e-5F);
}

TEST(NavigationGraphBuilder, ConnectsDifferentGroundResourcesInTheSameEntryBidirectionally) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            {
                { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F },
                { 1.0F, 0.0F, 1.0F }, { 0.0F, 0.0F, 1.0F },
            },
            { 0, 1, 2, 0, 2, 3 }), 0, NavigationSurfaceSourceKind::Grnd,
            NavigationSurfaceTraversalAvailability::Static, 7),
        makeSurface(1, 200, makeMesh(
            {
                { 0.0F, 0.0F, 0.8F }, { 1.0F, 0.0F, 0.8F },
                { 1.0F, 0.0F, 2.0F }, { 0.0F, 0.0F, 2.0F },
            },
            { 0, 1, 2, 0, 2, 3 }), 0, NavigationSurfaceSourceKind::Gobj,
            NavigationSurfaceTraversalAvailability::Static, 7),
    });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    ASSERT_EQ(graph.nodes.size(), 4U);
    ASSERT_FALSE(graph.collisionHandoffs.empty());
    EXPECT_GT(graph.statistics.sameEntryHandoffCount, 0U);
    EXPECT_EQ(graph.statistics.authoredFallbackHandoffCount, 0U);
    EXPECT_EQ(graph.collisionHandoffs.front().kind, NavigationCollisionHandoffKind::SameEntryBundle);

    bool forward = false;
    bool reverse = false;
    for (std::size_t sourceNode = 0; sourceNode < graph.nodes.size(); ++sourceNode) {
        for (const NavigationGraphEdge& edge : graph.nodes[sourceNode].edges) {
            if (edge.kind != NavigationGraphEdgeKind::CollisionHandoff) {
                continue;
            }
            forward |= sourceNode < 2U && edge.targetNodeIndex >= 2U;
            reverse |= sourceNode >= 2U && edge.targetNodeIndex < 2U;
        }
    }
    EXPECT_TRUE(forward);
    EXPECT_TRUE(reverse);
}

TEST(NavigationGraphBuilder, CurrentEntryCoverageSuppressesAuthoredFallbacks) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 }), 0, NavigationSurfaceSourceKind::Grnd,
            NavigationSurfaceTraversalAvailability::Static, 1),
        makeSurface(1, 150, makeMesh(
            { { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 1.0F } },
            { 0, 1, 2 }), 0, NavigationSurfaceSourceKind::Gobj,
            NavigationSurfaceTraversalAvailability::Static, 1),
        makeSurface(2, 200, makeMesh(
            { { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
    });
    addFallback(area, 0, { { 2 } });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    EXPECT_GT(graph.statistics.sameEntryHandoffCount, 0U);
    EXPECT_EQ(graph.statistics.authoredFallbackHandoffCount, 0U);
    for (const NavigationCollisionHandoff& handoff : graph.collisionHandoffs) {
        EXPECT_EQ(handoff.kind, NavigationCollisionHandoffKind::SameEntryBundle);
    }
}

TEST(NavigationGraphBuilder, DisconnectedTriangleInSameSurfaceSuppressesFallback) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            {
                { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F },
                { 1.002F, 0.0F, 0.002F }, { 0.002F, 0.0F, 1.002F }, { 1.0F, 0.0F, 1.0F },
            },
            { 0, 1, 2, 3, 4, 5 })),
        makeSurface(2, 200, makeMesh(
            { { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
    });
    addFallback(area, 0, { { 1 } });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    EXPECT_GT(graph.statistics.sameEntryHandoffCount, 0U);
    bool sameSurfaceContinuation = false;
    for (const NavigationCollisionHandoff& handoff : graph.collisionHandoffs) {
        sameSurfaceContinuation |= handoff.kind == NavigationCollisionHandoffKind::SameEntryBundle &&
            handoff.sourceTriangle.surfaceIndex == 0U &&
            handoff.targetTriangle.surfaceIndex == 0U &&
            handoff.sourceTriangle.triangleIndex != handoff.targetTriangle.triangleIndex;
    }
    EXPECT_TRUE(sameSurfaceContinuation);
}

TEST(NavigationGraphBuilder, EarlierFallbackShadowsLaterContinuousCandidateEvenWhenDiscontinuous) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
        makeSurface(2, 200, makeMesh(
            { { 1.0F, 1.0F, 0.0F }, { 0.0F, 1.0F, 1.0F }, { 1.0F, 1.0F, 1.0F } },
            { 0, 1, 2 })),
        makeSurface(3, 300, makeMesh(
            { { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
    });
    addFallback(area, 0, { { 1 }, { 2 } });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    EXPECT_TRUE(graph.collisionHandoffs.empty());
    EXPECT_GT(graph.statistics.unresolvedHandoffCount, 0U);
    EXPECT_GT(graph.statistics.priorityShadowedCandidateCount, 0U);
    EXPECT_TRUE(hasDiagnosticContaining(graph, "height-discontinuous"));
}

TEST(NavigationGraphBuilder, PreservesDuplicateTargetsAndUsesTheFirstAuthoredOrdinal) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
        makeSurface(2, 200, makeMesh(
            { { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
    });
    NavigationAuthoredGroundFallbackChain chain{
        .sourceTableIndex = area.surfaces[0].sourceTableIndex,
        .sourceEntryId = area.surfaces[0].sourceKey.sourceEntryId,
        .sourceSurfaces = { area.surfaces[0].sourceKey },
    };
    chain.targets.push_back(makeFallbackTarget(
        area, 2U, 0U, NavigationAuthoredGroundFallbackTargetStatus::Resolved, { 1U }));
    chain.targets.push_back(makeFallbackTarget(
        area, 2U, 1U, NavigationAuthoredGroundFallbackTargetStatus::Resolved, { 1U }));
    area.authoredGroundFallbackChains.push_back(std::move(chain));

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    EXPECT_EQ(graph.statistics.authoredFallbackTargetCount, 2U);
    EXPECT_GT(graph.statistics.priorityShadowedCandidateCount, 0U);
    ASSERT_FALSE(graph.collisionHandoffs.empty());
    for (const NavigationCollisionHandoff& handoff : graph.collisionHandoffs) {
        ASSERT_TRUE(handoff.authoredFallbackTargetIndex.has_value());
        ASSERT_TRUE(handoff.authoredOrdinal.has_value());
        EXPECT_EQ(*handoff.authoredFallbackTargetIndex, 0U);
        EXPECT_EQ(*handoff.authoredOrdinal, 0U);
    }
}

TEST(NavigationGraphBuilder, MissingEntryTruncatesTheEffectiveFallbackChain) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
        makeSurface(3, 300, makeMesh(
            { { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
    });
    NavigationAuthoredGroundFallbackChain chain{
        .sourceTableIndex = area.surfaces[0].sourceTableIndex,
        .sourceEntryId = area.surfaces[0].sourceKey.sourceEntryId,
        .sourceSurfaces = { area.surfaces[0].sourceKey },
    };
    chain.targets.push_back(makeFallbackTarget(
        area, 99U, 0U, NavigationAuthoredGroundFallbackTargetStatus::MissingEntry));
    // A resolved value after a missing entry is deliberately malformed input. The
    // graph still enforces the runtime truncation rule defensively.
    chain.targets.push_back(makeFallbackTarget(
        area, 3U, 1U, NavigationAuthoredGroundFallbackTargetStatus::Resolved, { 1U }));
    area.authoredGroundFallbackChains.push_back(std::move(chain));

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    EXPECT_EQ(graph.statistics.authoredFallbackTargetCount, 2U);
    EXPECT_TRUE(graph.collisionHandoffs.empty());
}

TEST(NavigationGraphBuilder, MissingGeometryDoesNotSuppressALaterResolvedTarget) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
        makeSurface(3, 300, makeMesh(
            { { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
    });
    NavigationAuthoredGroundFallbackChain chain{
        .sourceTableIndex = area.surfaces[0].sourceTableIndex,
        .sourceEntryId = area.surfaces[0].sourceKey.sourceEntryId,
        .sourceSurfaces = { area.surfaces[0].sourceKey },
    };
    chain.targets.push_back(makeFallbackTarget(
        area,
        2U,
        0U,
        NavigationAuthoredGroundFallbackTargetStatus::MissingGeometry,
        {},
        2U));
    chain.targets.push_back(makeFallbackTarget(
        area, 3U, 1U, NavigationAuthoredGroundFallbackTargetStatus::Resolved, { 1U }));
    area.authoredGroundFallbackChains.push_back(std::move(chain));

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    ASSERT_FALSE(graph.collisionHandoffs.empty());
    for (const NavigationCollisionHandoff& handoff : graph.collisionHandoffs) {
        EXPECT_EQ(handoff.targetEntryId, 3U);
        EXPECT_EQ(handoff.authoredFallbackTargetIndex, 1U);
        EXPECT_EQ(handoff.authoredOrdinal, 1U);
    }
}

TEST(NavigationGraphBuilder, KeepsRuntimeDependentHandoffAsConditionalEvidenceOnly) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
        makeSurface(2, 200, makeMesh(
            { { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 1.0F } },
            { 0, 1, 2 }), 0, NavigationSurfaceSourceKind::Gobj,
            NavigationSurfaceTraversalAvailability::RequiresRuntimeState),
    });
    addFallback(area, 0, { { 1 } });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    ASSERT_EQ(graph.nodes.size(), 1U);
    ASSERT_EQ(graph.collisionHandoffs.size(), 1U);
    EXPECT_EQ(graph.collisionHandoffs[0].availability,
        NavigationCollisionHandoffAvailability::RequiresRuntimeState);
    EXPECT_EQ(graph.statistics.conditionalHandoffCount, 1U);
    EXPECT_EQ(graph.statistics.runtimeDependentSurfaceCount, 1U);
    EXPECT_TRUE(graph.nodes[0].edges.empty());
    EXPECT_TRUE(hasDiagnosticContaining(graph, "excluded from pathfinding nodes"));
}

TEST(NavigationGraphBuilder, RuntimeDependentEarlierTargetBlocksLaterStaticFallback) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
        makeSurface(2, 200, makeMesh(
            { { 10.0F, 0.0F, 0.0F }, { 11.0F, 0.0F, 0.0F }, { 10.0F, 0.0F, 1.0F } },
            { 0, 1, 2 }), 0, NavigationSurfaceSourceKind::Gobj,
            NavigationSurfaceTraversalAvailability::RequiresRuntimeState),
        makeSurface(3, 300, makeMesh(
            { { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
    });
    addFallback(area, 0, { { 1 }, { 2 } });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    EXPECT_TRUE(graph.collisionHandoffs.empty());
    EXPECT_GT(graph.statistics.runtimeStateBlockedIntervalCount, 0U);
    EXPECT_TRUE(hasDiagnosticContaining(graph, "runtime-dependent collision provider"));
}

TEST(NavigationGraphBuilder, OmitsInvalidDegenerateAndNonManifoldTopology) {
    auto area = makeCompleteArea({ makeSurface(1, 100, makeMesh(
        {
            { 0.0F, 0.0F, 0.0F },
            { 1.0F, 0.0F, 0.0F },
            { 0.0F, 0.0F, 1.0F },
            { 0.0F, 0.0F, -1.0F },
            { 0.0F, 1.0F, 0.0F },
        },
        {
            0, 1, 2,
            1, 0, 3,
            0, 1, 4,
            0, 1, 99,
            0, 0, 1,
            2,
        })) });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    EXPECT_TRUE(graph.nodes.empty());
    EXPECT_EQ(graph.statistics.skippedInvalidTriangleCount, 1U);
    EXPECT_EQ(graph.statistics.skippedDegenerateTriangleCount, 1U);
    EXPECT_EQ(graph.statistics.skippedNonManifoldTriangleCount, 3U);
    EXPECT_EQ(graph.statistics.nonManifoldEdgeCount, 1U);
    EXPECT_TRUE(hasDiagnosticContaining(graph, "not divisible by three"));
    EXPECT_TRUE(hasDiagnosticContaining(graph, "non-manifold edge"));
}

TEST(NavigationGraphBuilder, A101bConnectsLowerFloorThroughGobjStaircaseToUpperFloor) {
    const char* fixturePath = std::getenv("SAVOR_NAV_A101B_MLD");
    if (fixturePath == nullptr || *fixturePath == '\0' || !std::filesystem::is_regular_file(fixturePath)) {
        GTEST_SKIP() << "Set SAVOR_NAV_A101B_MLD to the extracted a101b.mld fixture.";
    }

    const NavigationAreaLoadResult loaded = NavigationAreaLoader{}.loadFile(fixturePath);
    ASSERT_TRUE(loaded.hasModel());
    const NavigationAreaModel& area = *loaded.model;
    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    std::vector<std::size_t> lowerNodes{};
    std::vector<std::size_t> staircaseNodes{};
    std::vector<std::size_t> upperNodes{};
    for (std::size_t nodeIndex = 0; nodeIndex < graph.nodes.size(); ++nodeIndex) {
        const NavigationSurface& surface = area.surfaces[graph.nodes[nodeIndex].key.surfaceIndex];
        switch (surface.sourceKey.sourceEntryId) {
        case 33U:
            lowerNodes.push_back(nodeIndex);
            break;
        case 34U:
            staircaseNodes.push_back(nodeIndex);
            break;
        case 36U:
            upperNodes.push_back(nodeIndex);
            break;
        default:
            break;
        }
    }
    ASSERT_FALSE(lowerNodes.empty());
    ASSERT_FALSE(staircaseNodes.empty());
    ASSERT_FALSE(upperNodes.empty());

    const auto findSuccessfulRoute = [&](const std::vector<std::size_t>& starts,
                                         const std::vector<std::size_t>& goals)
        -> std::optional<NavigationPathResult> {
        for (const std::size_t startNode : starts) {
            for (const std::size_t goalNode : goals) {
                const NavigationPathResult result = NavigationPathfinder{}.findPath(
                    graph,
                    NavigationPathQuery{
                        .start = anchorForNode(graph, startNode, graph.nodes[startNode].centroid),
                        .goal = anchorForNode(graph, goalNode, graph.nodes[goalNode].centroid),
                    });
                if (result.hasPath()) {
                    return result;
                }
            }
        }
        return std::nullopt;
    };
    const auto routeUsesFallback = [&](const NavigationPathResult& route,
                                       const std::uint32_t sourceEntryId,
                                       const std::uint32_t targetEntryId,
                                       const std::size_t authoredOrdinal) {
        if (route.trianglePath.empty() || route.edgePath.size() + 1U != route.trianglePath.size()) {
            return false;
        }
        for (std::size_t edgePathIndex = 0U; edgePathIndex < route.edgePath.size(); ++edgePathIndex) {
            const auto sourceNode = graph.findNodeIndex(route.trianglePath[edgePathIndex]);
            const auto targetNode = graph.findNodeIndex(route.trianglePath[edgePathIndex + 1U]);
            if (!sourceNode.has_value() || !targetNode.has_value() ||
                route.edgePath[edgePathIndex] >= graph.nodes[*sourceNode].edges.size()) {
                continue;
            }
            const NavigationGraphEdge& edge =
                graph.nodes[*sourceNode].edges[route.edgePath[edgePathIndex]];
            if (edge.targetNodeIndex != *targetNode || !edge.collisionHandoffIndex.has_value() ||
                *edge.collisionHandoffIndex >= graph.collisionHandoffs.size()) {
                continue;
            }
            const NavigationCollisionHandoff& handoff =
                graph.collisionHandoffs[*edge.collisionHandoffIndex];
            if (handoff.kind == NavigationCollisionHandoffKind::AuthoredFallback &&
                handoff.sourceEntryId == sourceEntryId &&
                handoff.targetEntryId == targetEntryId &&
                handoff.authoredOrdinal == authoredOrdinal) {
                return true;
            }
        }
        return false;
    };

    const auto lowerToStaircase = findSuccessfulRoute(lowerNodes, staircaseNodes);
    const auto staircaseToUpper = findSuccessfulRoute(staircaseNodes, upperNodes);
    const auto lowerToUpper = findSuccessfulRoute(lowerNodes, upperNodes);
    const auto upperToLower = findSuccessfulRoute(upperNodes, lowerNodes);
    ASSERT_TRUE(lowerToStaircase.has_value());
    ASSERT_TRUE(staircaseToUpper.has_value());
    ASSERT_TRUE(lowerToUpper.has_value());
    ASSERT_TRUE(upperToLower.has_value());
    EXPECT_TRUE(routeUsesFallback(*lowerToStaircase, 33U, 34U, 1U));
    EXPECT_TRUE(routeUsesFallback(*staircaseToUpper, 34U, 36U, 1U));
    EXPECT_TRUE(routeUsesFallback(*lowerToUpper, 33U, 34U, 1U));
    EXPECT_TRUE(routeUsesFallback(*lowerToUpper, 34U, 36U, 1U));
    EXPECT_TRUE(routeUsesFallback(*upperToLower, 36U, 34U, 0U));
    EXPECT_TRUE(routeUsesFallback(*upperToLower, 34U, 33U, 0U));
}

TEST(NavigationGraphAnchorer, SnapsToThePickedSurfaceAndHonorsDistanceLimit) {
    auto area = makeCompleteArea({ makeSurface(1, 100, makeMesh(
        { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
        { 0, 1, 2 })) });
    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    const auto anchored = NavigationGraphAnchorer{}.anchorToSurface(
        graph,
        0,
        NavigationVec3{ 0.25F, 0.2F, 0.25F },
        0.5F);
    ASSERT_TRUE(anchored.hasAnchor());
    EXPECT_NEAR(anchored.anchor->snappedPoint.x, 0.25F, 1.0e-5F);
    EXPECT_NEAR(anchored.anchor->snappedPoint.y, 0.0F, 1.0e-5F);
    EXPECT_NEAR(anchored.anchor->snappedPoint.z, 0.25F, 1.0e-5F);
    EXPECT_NEAR(anchored.anchor->snapDistance, 0.2F, 1.0e-5F);

    const auto tooFar = NavigationGraphAnchorer{}.anchorToSurface(
        graph,
        0,
        NavigationVec3{ 0.25F, 2.0F, 0.25F },
        0.5F);
    EXPECT_EQ(tooFar.status, NavigationGraphAnchorStatus::TooFar);
    EXPECT_FALSE(tooFar.hasAnchor());

    const auto missingSurface = NavigationGraphAnchorer{}.anchorToSurface(graph, 1, NavigationVec3{});
    EXPECT_EQ(missingSurface.status, NavigationGraphAnchorStatus::SurfaceNotFound);
}

TEST(NavigationStartResolver, SelectsTheUniqueNearestMatchingTblIdSurfaceWithoutADistanceCeiling) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 }), 5),
        makeSurface(2, 200, makeMesh(
            { { 0.0F, 10.0F, 0.0F }, { 1.0F, 10.0F, 0.0F }, { 0.0F, 10.0F, 1.0F } },
            { 0, 1, 2 }), 5),
    });
    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);
    const NavigationStartOption option = makeStartOption(5, { 0.25F, 100.0F, 0.25F }, -70.0F);

    const NavigationStartResolveResult result = NavigationStartResolver{}.resolve(area, graph, option);

    ASSERT_TRUE(result.hasAnchor());
    EXPECT_EQ(result.status, NavigationStartResolveStatus::Resolved);
    EXPECT_EQ(result.anchor->triangle.surfaceIndex, 1U);
    EXPECT_NEAR(result.anchor->snapDistance, 90.0F, 1.0e-5F);
    ASSERT_TRUE(result.yawDegrees.has_value());
    EXPECT_FLOAT_EQ(*result.yawDegrees, -70.0F);
    EXPECT_FLOAT_EQ(NavigationCoordinatePolicy::identity().convertYaw(135.0F), 135.0F);
}

TEST(NavigationCoordinatePolicy, ConvertsSceneYawToFacingDirectionWithoutASecondYawConversion) {
    const NavigationCoordinatePolicy policy = NavigationCoordinatePolicy::identity();

    const NavigationVec3 zero = policy.sceneFacingDirectionFromConvertedYaw(0.0F);
    EXPECT_NEAR(zero.x, 0.0F, 1.0e-6F);
    EXPECT_NEAR(zero.y, 0.0F, 1.0e-6F);
    EXPECT_NEAR(zero.z, 1.0F, 1.0e-6F);

    const NavigationVec3 ninety = policy.sceneFacingDirectionFromConvertedYaw(90.0F);
    EXPECT_NEAR(ninety.x, 1.0F, 1.0e-6F);
    EXPECT_NEAR(ninety.y, 0.0F, 1.0e-6F);
    EXPECT_NEAR(ninety.z, 0.0F, 1.0e-6F);

    const NavigationVec3 oneEighty = policy.sceneFacingDirectionFromConvertedYaw(180.0F);
    EXPECT_NEAR(oneEighty.x, 0.0F, 1.0e-6F);
    EXPECT_NEAR(oneEighty.y, 0.0F, 1.0e-6F);
    EXPECT_NEAR(oneEighty.z, -1.0F, 1.0e-6F);
}

TEST(NavigationStartResolver, ReportsEffectivelyTiedDistinctSurfacesAsAmbiguous) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 }), 7),
        makeSurface(2, 200, makeMesh(
            { { 0.0F, 2.0F, 0.0F }, { 1.0F, 2.0F, 0.0F }, { 0.0F, 2.0F, 1.0F } },
            { 0, 1, 2 }), 7),
    });
    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);
    const NavigationStartOption option = makeStartOption(7, { 0.25F, 1.0F, 0.25F });

    const NavigationStartResolveResult result = NavigationStartResolver{}.resolve(area, graph, option);

    EXPECT_EQ(result.status, NavigationStartResolveStatus::AmbiguousSurface);
    EXPECT_FALSE(result.hasAnchor());
}

TEST(NavigationStartResolver, ResolvesCopiedSeamGeometryAtTheSameWorldPointDeterministically) {
    const NavigationMesh triangle = makeMesh(
        { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
        { 0, 1, 2 });
    auto area = makeCompleteArea({
        makeSurface(1, 100, triangle, 8),
        makeSurface(2, 200, triangle, 8),
    });
    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);
    const NavigationStartOption option = makeStartOption(8, { 0.25F, 0.0F, 0.25F });

    const NavigationStartResolveResult result = NavigationStartResolver{}.resolve(area, graph, option);

    ASSERT_TRUE(result.hasAnchor());
    EXPECT_EQ(result.anchor->triangle.surfaceIndex, 0U);
}

TEST(NavigationStartResolver, BreaksTriangleTiesDeterministicallyWithinOneSurface) {
    auto area = makeCompleteArea({ makeSurface(1, 100, makeMesh(
        {
            { -2.0F, 0.0F, 0.0F }, { -1.0F, 0.0F, 0.0F }, { -2.0F, 0.0F, 1.0F },
            { 1.0F, 0.0F, 0.0F }, { 2.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 1.0F },
        },
        { 0, 1, 2, 3, 4, 5 }), 9) });
    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);
    const NavigationStartOption option = makeStartOption(9, { 0.0F, 0.0F, 0.0F });

    const NavigationStartResolveResult result = NavigationStartResolver{}.resolve(area, graph, option);

    ASSERT_TRUE(result.hasAnchor());
    EXPECT_EQ(result.anchor->nodeIndex, 0U);
    EXPECT_FALSE(result.yawDegrees.has_value());
}

TEST(NavigationStartResolver, ReportsMissingIncompleteGraphAndGroundFailuresExplicitly) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 }), 1),
        makeSurface(2, 200, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 99 }), 5),
    });
    NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    const auto missingCandidate = NavigationStartResolver{}.resolve(area, graph, nullptr);
    EXPECT_EQ(missingCandidate.status, NavigationStartResolveStatus::MissingCandidate);

    NavigationStartOption incomplete{};
    incomplete.availability = NavigationStartAvailability::Incomplete;
    incomplete.unavailableReason = "Position is dynamic.";
    const auto incompletePlacement = NavigationStartResolver{}.resolve(area, graph, incomplete);
    EXPECT_EQ(incompletePlacement.status, NavigationStartResolveStatus::IncompletePlacement);

    const NavigationStartOption missingGroundOption = makeStartOption(99, { 0.0F, 0.0F, 0.0F });
    const auto missingGround = NavigationStartResolver{}.resolve(area, graph, missingGroundOption);
    EXPECT_EQ(missingGround.status, NavigationStartResolveStatus::MissingGround);

    const NavigationStartOption unusableGroundOption = makeStartOption(5, { 0.0F, 0.0F, 0.0F });
    const auto anchoringFailure = NavigationStartResolver{}.resolve(area, graph, unusableGroundOption);
    EXPECT_EQ(anchoringFailure.status, NavigationStartResolveStatus::AnchoringFailure);

    graph.hasCompleteWallGeometry = false;
    const NavigationStartOption validOption = makeStartOption(1, { 0.0F, 0.0F, 0.0F });
    const auto incompleteGraph = NavigationStartResolver{}.resolve(area, graph, validOption);
    EXPECT_EQ(incompleteGraph.status, NavigationStartResolveStatus::IncompleteGraph);
}

TEST(NavigationTriggerGoalResolver, RejectsInvalidNonTriggerAndUnusableRegions) {
    auto area = makeCompleteArea({ makeSurface(1, 100, makeMesh(
        { { 0.0F, 0.0F, 0.0F }, { 2.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 2.0F } },
        { 0, 1, 2 })) });
    area.regions = {
        makeRegion(NavigationRegionKind::Collision, { makeRegionMesh(makeMesh(
            { { 0.0F, 1.0F, 0.0F }, { 1.0F, 1.0F, 0.0F }, { 0.0F, 1.0F, 1.0F } },
            { 0, 1, 2 })) }),
        makeRegion(NavigationRegionKind::Trigger),
        makeRegion(NavigationRegionKind::Trigger, { makeRegionMesh(makeMesh(
            { { 0.0F, 1.0F, 0.0F }, { 1.0F, 1.0F, 0.0F }, { 2.0F, 1.0F, 0.0F } },
            { 0, 1, 2, 0, 1, 99 })) }),
    };
    NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);
    const NavigationTriggerGoalResolver resolver{};

    EXPECT_EQ(resolver.resolve(area, graph, 99).status,
        NavigationTriggerGoalResolveStatus::RegionNotFound);
    EXPECT_EQ(resolver.resolve(area, graph, 0).status,
        NavigationTriggerGoalResolveStatus::NotTrigger);
    EXPECT_EQ(resolver.resolve(area, graph, 1).status,
        NavigationTriggerGoalResolveStatus::MissingGeometry);
    EXPECT_EQ(resolver.resolve(area, graph, 2).status,
        NavigationTriggerGoalResolveStatus::MissingGeometry);

    area.regions.push_back(makeRegion(NavigationRegionKind::Trigger, { makeRegionMesh(makeMesh(
        { { 0.0F, 1.0F, 0.0F }, { 1.0F, 1.0F, 0.0F }, { 0.0F, 1.0F, 1.0F } },
        { 0, 1, 2 })) }));
    graph.hasCompleteWallGeometry = false;
    EXPECT_EQ(resolver.resolve(area, graph, 3).status,
        NavigationTriggerGoalResolveStatus::IncompleteGraph);
}

TEST(NavigationTriggerGoalResolver, SelectsLargestPlanarAabbThenTriangleCountAndStableIndex) {
    auto area = makeCompleteArea({ makeSurface(1, 100, makeMesh(
        {
            { -5.0F, 0.0F, -5.0F }, { 5.0F, 0.0F, -5.0F },
            { 5.0F, 0.0F, 5.0F }, { -5.0F, 0.0F, 5.0F },
        },
        { 0, 1, 2, 0, 2, 3 })) });
    const NavigationMesh largeOneTriangle = makeMesh(
        {
            { -2.0F, 2.0F, -2.0F }, { 2.0F, 2.0F, -2.0F },
            { 2.0F, 2.0F, 2.0F }, { -2.0F, 2.0F, 2.0F },
        },
        { 0, 1, 2 });
    const NavigationMesh largeTwoTriangles = makeMesh(
        {
            { -2.0F, 2.0F, -2.0F }, { 2.0F, 2.0F, -2.0F },
            { 2.0F, 2.0F, 2.0F }, { -2.0F, 2.0F, 2.0F },
        },
        { 0, 1, 2, 0, 2, 3 });
    area.regions.push_back(makeRegion(NavigationRegionKind::Trigger, {
        makeRegionMesh(makeMesh(
            {
                { 0.0F, 2.0F, 0.0F }, { 1.0F, 2.0F, 0.0F },
                { 0.0F, 2.0F, 1.0F }, { 1000.0F, 1000.0F, 1000.0F },
            },
            { 0, 1, 2 })),
        makeRegionMesh(largeOneTriangle),
        makeRegionMesh(largeTwoTriangles),
        makeRegionMesh(largeTwoTriangles),
    }));
    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    const NavigationTriggerGoalResolveResult result =
        NavigationTriggerGoalResolver{}.resolve(area, graph, 0);

    ASSERT_TRUE(result.hasTarget());
    ASSERT_TRUE(result.target->displayBounds.valid);
    EXPECT_EQ(result.target->primaryMeshIndex, 2U);
    EXPECT_FLOAT_EQ(result.target->primaryMeshAabbSquaredDiagonal, 32.0F);
    EXPECT_EQ(result.target->primaryMeshReferencedTriangleCount, 2U);
    EXPECT_FLOAT_EQ(result.target->displayBounds.minimum.x, -2.0F);
    EXPECT_FLOAT_EQ(result.target->displayBounds.minimum.y, 2.0F);
    EXPECT_FLOAT_EQ(result.target->displayBounds.minimum.z, -2.0F);
    EXPECT_FLOAT_EQ(result.target->displayBounds.maximum.x, 2.0F);
    EXPECT_FLOAT_EQ(result.target->displayBounds.maximum.y, 2.0F);
    EXPECT_FLOAT_EQ(result.target->displayBounds.maximum.z, 2.0F);
    EXPECT_NEAR(result.target->anchor.snapDistance, 2.0F, 1.0e-5F);
}

TEST(NavigationTriggerGoalResolver, AnchorsFromTriggerTrianglesRatherThanTheirBoundsCenter) {
    auto area = makeCompleteArea({ makeSurface(1, 100, makeMesh(
        { { 0.0F, 0.0F, 0.0F }, { 2.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 2.0F } },
        { 0, 1, 2 })) });
    area.regions.push_back(makeRegion(NavigationRegionKind::Trigger, { makeRegionMesh(makeMesh(
        {
            { 0.25F, -1.0F, 0.25F },
            { 0.25F, 1.0F, 0.25F },
            { 0.25F, 1.0F, 1.25F },
        },
        { 0, 1, 2 })) }));
    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    const NavigationTriggerGoalResolveResult result =
        NavigationTriggerGoalResolver{}.resolve(area, graph, 0);

    ASSERT_TRUE(result.hasTarget());
    EXPECT_NEAR(result.target->anchor.snapDistance, 0.0F, 1.0e-5F);
    EXPECT_NEAR(result.target->anchor.snappedPoint.y, 0.0F, 1.0e-5F);
    EXPECT_NEAR(result.target->anchor.exactPoint.y, 0.0F, 1.0e-5F);
}

TEST(NavigationTriggerGoalResolver, ReportsTiedStackedSurfacesAtDistinctPointsAsAmbiguous) {
    const NavigationMesh ground = makeMesh(
        { { 0.0F, 0.0F, 0.0F }, { 2.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 2.0F } },
        { 0, 1, 2 });
    const NavigationMesh upperGround = makeMesh(
        { { 0.0F, 2.0F, 0.0F }, { 2.0F, 2.0F, 0.0F }, { 0.0F, 2.0F, 2.0F } },
        { 0, 1, 2 });
    auto area = makeCompleteArea({
        makeSurface(1, 100, ground),
        makeSurface(2, 200, upperGround),
    });
    area.regions.push_back(makeRegion(NavigationRegionKind::Trigger, { makeRegionMesh(makeMesh(
        { { 0.0F, 1.0F, 0.0F }, { 2.0F, 1.0F, 0.0F }, { 0.0F, 1.0F, 2.0F } },
        { 0, 1, 2 })) }));
    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    const NavigationTriggerGoalResolveResult result =
        NavigationTriggerGoalResolver{}.resolve(area, graph, 0);

    EXPECT_EQ(result.status, NavigationTriggerGoalResolveStatus::AmbiguousSurface);
    EXPECT_FALSE(result.hasTarget());
}

TEST(NavigationTriggerGoalResolver, ResolvesCoincidentSurfaceCopiesInStableNodeOrder) {
    const NavigationMesh ground = makeMesh(
        { { 0.0F, 0.0F, 0.0F }, { 2.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 2.0F } },
        { 0, 1, 2 });
    auto area = makeCompleteArea({
        makeSurface(1, 100, ground),
        makeSurface(2, 200, ground),
    });
    area.regions.push_back(makeRegion(NavigationRegionKind::Trigger, { makeRegionMesh(makeMesh(
        { { 0.0F, 1.0F, 0.0F }, { 2.0F, 1.0F, 0.0F }, { 0.0F, 1.0F, 2.0F } },
        { 0, 1, 2 })) }));
    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    const NavigationTriggerGoalResolveResult result =
        NavigationTriggerGoalResolver{}.resolve(area, graph, 0);

    ASSERT_TRUE(result.hasTarget());
    EXPECT_EQ(result.target->anchor.nodeIndex, 0U);
    EXPECT_EQ(result.target->anchor.triangle.surfaceIndex, 0U);
}

TEST(NavigationPathfinder, FollowsDirectedPortalAndRejectsReverseTraversal) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
        makeSurface(2, 200, makeMesh(
            { { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
    });
    addFallback(area, 0, { { 1 } });
    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);
    const auto start = NavigationGraphAnchorer{}.anchorToSurface(graph, 0, NavigationVec3{ 0.1F, 0.0F, 0.1F });
    const auto goal = NavigationGraphAnchorer{}.anchorToSurface(graph, 1, NavigationVec3{ 0.9F, 0.0F, 0.9F });
    ASSERT_TRUE(start.hasAnchor());
    ASSERT_TRUE(goal.hasAnchor());

    const NavigationPathResult forward = NavigationPathfinder{}.findPath(
        graph,
        NavigationPathQuery{ .start = *start.anchor, .goal = *goal.anchor });
    ASSERT_TRUE(forward.hasPath());
    EXPECT_EQ(forward.trianglePath.size(), 2U);
    ASSERT_EQ(forward.edgePath.size(), 1U);
    ASSERT_LT(forward.edgePath[0], graph.nodes[start.anchor->nodeIndex].edges.size());
    EXPECT_EQ(
        graph.nodes[start.anchor->nodeIndex].edges[forward.edgePath[0]].kind,
        NavigationGraphEdgeKind::CollisionHandoff);
    EXPECT_EQ(forward.polyline.size(), 3U);
    EXPECT_GT(forward.routeLength, 0.0F);

    const NavigationPathResult reverse = NavigationPathfinder{}.findPath(
        graph,
        NavigationPathQuery{ .start = *goal.anchor, .goal = *start.anchor });
    EXPECT_EQ(reverse.status, NavigationPathStatus::Unreachable);
    EXPECT_FALSE(reverse.hasPath());
}

TEST(NavigationPathfinder, UsesStableNodeOrderToBreakEqualCostTies) {
    NavigationTraversalGraph graph{};
    graph.hasCompleteGroundGeometry = true;
    graph.hasCompleteWallGeometry = true;
    graph.statistics.sourceSurfaceCount = 1;
    graph.nodes = {
        NavigationGraphNode{ .key = { 0, 0 }, .centroid = { 0.0F, 0.0F, 0.0F } },
        NavigationGraphNode{ .key = { 0, 1 }, .centroid = { 1.0F, 0.0F, 1.0F } },
        NavigationGraphNode{ .key = { 0, 2 }, .centroid = { 1.0F, 0.0F, -1.0F } },
        NavigationGraphNode{ .key = { 0, 3 }, .centroid = { 2.0F, 0.0F, 0.0F } },
    };
    graph.nodes[0].edges = {
        NavigationGraphEdge{ .targetNodeIndex = 2, .portal = { { 0.5F, 0.0F, -0.5F }, { 0.5F, 0.0F, -0.5F } } },
        NavigationGraphEdge{ .targetNodeIndex = 1, .portal = { { 0.5F, 0.0F, 0.5F }, { 0.5F, 0.0F, 0.5F } } },
    };
    graph.nodes[1].edges = {
        NavigationGraphEdge{ .targetNodeIndex = 3, .portal = { { 1.5F, 0.0F, 0.5F }, { 1.5F, 0.0F, 0.5F } } },
    };
    graph.nodes[2].edges = {
        NavigationGraphEdge{ .targetNodeIndex = 3, .portal = { { 1.5F, 0.0F, -0.5F }, { 1.5F, 0.0F, -0.5F } } },
    };

    const NavigationPathResult result = NavigationPathfinder{}.findPath(
        graph,
        NavigationPathQuery{
            .start = anchorForNode(graph, 0, graph.nodes[0].centroid),
            .goal = anchorForNode(graph, 3, graph.nodes[3].centroid),
            .slopeCostMultiplier = 0.0F,
        });

    ASSERT_TRUE(result.hasPath());
    ASSERT_EQ(result.trianglePath.size(), 3U);
    ASSERT_EQ(result.edgePath.size(), 2U);
    EXPECT_EQ(result.edgePath[0], 1U);
    EXPECT_EQ(result.trianglePath[1], (NavigationTriangleKey{ 0, 1 }));
}

TEST(NavigationPathfinder, ReportsSameTriangleInvalidAnchorAndIncompleteGeometry) {
    auto area = makeCompleteArea({ makeSurface(1, 100, makeMesh(
        { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
        { 0, 1, 2 })) });
    NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);
    const auto start = NavigationGraphAnchorer{}.anchorToSurface(graph, 0, NavigationVec3{ 0.1F, 0.0F, 0.1F });
    const auto goal = NavigationGraphAnchorer{}.anchorToSurface(graph, 0, NavigationVec3{ 0.2F, 0.0F, 0.2F });
    ASSERT_TRUE(start.hasAnchor());
    ASSERT_TRUE(goal.hasAnchor());

    const NavigationPathResult sameTriangle = NavigationPathfinder{}.findPath(
        graph,
        NavigationPathQuery{ .start = *start.anchor, .goal = *goal.anchor });
    EXPECT_EQ(sameTriangle.status, NavigationPathStatus::Success);
    EXPECT_EQ(sameTriangle.trianglePath.size(), 1U);
    EXPECT_TRUE(sameTriangle.edgePath.empty());
    EXPECT_EQ(sameTriangle.polyline.size(), 2U);

    NavigationGraphAnchor invalidStart = *start.anchor;
    invalidStart.nodeIndex = 99;
    const NavigationPathResult invalid = NavigationPathfinder{}.findPath(
        graph,
        NavigationPathQuery{ .start = invalidStart, .goal = *goal.anchor });
    EXPECT_EQ(invalid.status, NavigationPathStatus::InvalidStart);

    graph.hasCompleteWallGeometry = false;
    const NavigationPathResult incomplete = NavigationPathfinder{}.findPath(
        graph,
        NavigationPathQuery{ .start = *start.anchor, .goal = *goal.anchor });
    EXPECT_EQ(incomplete.status, NavigationPathStatus::IncompleteGeometry);
}

} // namespace
} // namespace savor::navigation
