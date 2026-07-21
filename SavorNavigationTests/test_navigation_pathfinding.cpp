#include "Graph/NavigationTraversalGraph.h"
#include "Pathfinding/NavigationPathfinder.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
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
    const std::int32_t tblId = 0) {
    return NavigationSurface{
        .sourceKey = NavigationSurfaceSourceKey{
            .sourceEntryId = entryId,
            .sourceBlockOffset = blockOffset,
            .sourceNodeOffset = 0,
        },
        .sourceKind = NavigationSurfaceSourceKind::Grnd,
        .tblId = tblId,
        .mesh = std::move(mesh),
    };
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

TEST(NavigationGraphBuilder, LeavesStackedSurfacesDisconnectedWithoutGroundLink) {
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
    EXPECT_EQ(graph.statistics.groundLinkPortalCount, 0U);
}

TEST(NavigationGraphBuilder, CreatesOnlyTheDirectedPortalAuthorizedByGroundLink) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
        makeSurface(2, 200, makeMesh(
            { { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
    });
    area.groundLinks.push_back(NavigationGroundLink{
        .sourceEntryId = 1,
        .targetEntryId = 2,
        .resolution = NavigationGroundLinkResolution::Resolved,
        .sourceSurfaces = { area.surfaces[0].sourceKey },
        .targetSurfaces = { area.surfaces[1].sourceKey },
    });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    ASSERT_EQ(graph.nodes.size(), 2U);
    ASSERT_EQ(graph.nodes[0].edges.size(), 1U);
    EXPECT_EQ(graph.nodes[0].edges[0].targetNodeIndex, 1U);
    EXPECT_EQ(graph.nodes[0].edges[0].kind, NavigationGraphEdgeKind::GroundLink);
    EXPECT_TRUE(graph.nodes[1].edges.empty());
    EXPECT_EQ(graph.statistics.groundLinkPortalCount, 1U);
}

TEST(NavigationGraphBuilder, UsesGeometryToResolveAnAmbiguousGroundLinkCandidateSet) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
        makeSurface(2, 200, makeMesh(
            { { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F }, { 1.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
        makeSurface(2, 300, makeMesh(
            { { 10.0F, 0.0F, 0.0F }, { 11.0F, 0.0F, 0.0F }, { 10.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
    });
    area.groundLinks.push_back(NavigationGroundLink{
        .sourceEntryId = 1,
        .targetEntryId = 2,
        .resolution = NavigationGroundLinkResolution::Ambiguous,
        .sourceSurfaces = { area.surfaces[0].sourceKey },
        .targetSurfaces = { area.surfaces[1].sourceKey, area.surfaces[2].sourceKey },
    });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    ASSERT_EQ(graph.nodes.size(), 3U);
    ASSERT_EQ(graph.nodes[0].edges.size(), 1U);
    EXPECT_EQ(graph.nodes[0].edges[0].targetNodeIndex, 1U);
    EXPECT_EQ(graph.statistics.groundLinkPortalCount, 1U);
    EXPECT_EQ(graph.statistics.unresolvedGroundLinkCount, 0U);
}

TEST(NavigationGraphBuilder, WarnsInsteadOfGuessingWhenLinkedBoundariesDoNotOverlap) {
    auto area = makeCompleteArea({
        makeSurface(1, 100, makeMesh(
            { { 0.0F, 0.0F, 0.0F }, { 1.0F, 0.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
        makeSurface(2, 200, makeMesh(
            { { 10.0F, 0.0F, 0.0F }, { 11.0F, 0.0F, 0.0F }, { 10.0F, 0.0F, 1.0F } },
            { 0, 1, 2 })),
    });
    area.groundLinks.push_back(NavigationGroundLink{
        .sourceEntryId = 1,
        .targetEntryId = 2,
        .resolution = NavigationGroundLinkResolution::Resolved,
        .sourceSurfaces = { area.surfaces[0].sourceKey },
        .targetSurfaces = { area.surfaces[1].sourceKey },
    });

    const NavigationTraversalGraph graph = NavigationGraphBuilder{}.build(area);

    EXPECT_EQ(graph.statistics.unresolvedGroundLinkCount, 1U);
    EXPECT_TRUE(hasDiagnosticContaining(graph, "no geometrically overlapping boundary"));
    EXPECT_TRUE(graph.nodes[0].edges.empty());
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
    area.groundLinks.push_back(NavigationGroundLink{
        .sourceEntryId = 1,
        .targetEntryId = 2,
        .resolution = NavigationGroundLinkResolution::Resolved,
        .sourceSurfaces = { area.surfaces[0].sourceKey },
        .targetSurfaces = { area.surfaces[1].sourceKey },
    });
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
