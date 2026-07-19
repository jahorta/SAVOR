#include "Projection/RegionMeshProjector.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

namespace savor::navigation {
namespace {

using namespace spice::mld::model;

[[nodiscard]] BlenderIrMesh makeQuadMesh() {
    BlenderIrMesh mesh{};
    mesh.label = "synthetic-wall";
    mesh.sourceObjectAddress = 0x1200U;
    mesh.sourceChunkOffset = 0x1210U;
    mesh.sourceAttachOffset = 0x1300U;
    mesh.vertices = {
        BlenderIrVertex{ .position = Vec3{ 0.0F, 0.0F, 0.0F }, .normal = Vec3{ 0.0F, 1.0F, 0.0F }, .hasPosition = true, .hasNormal = true },
        BlenderIrVertex{ .position = Vec3{ 1.0F, 0.0F, 0.0F }, .normal = Vec3{ 0.0F, 1.0F, 0.0F }, .hasPosition = true, .hasNormal = true },
        BlenderIrVertex{ .position = Vec3{ 1.0F, 0.0F, 1.0F }, .normal = Vec3{ 0.0F, 1.0F, 0.0F }, .hasPosition = true, .hasNormal = true },
        BlenderIrVertex{ .position = Vec3{ 0.0F, 0.0F, 1.0F }, .normal = Vec3{ 0.0F, 1.0F, 0.0F }, .hasPosition = true, .hasNormal = true },
    };
    BlenderIrTriangleSet first{};
    first.corners = {
        BlenderIrCorner{ .vertexIndex = 0U },
        BlenderIrCorner{ .vertexIndex = 1U },
        BlenderIrCorner{ .vertexIndex = 2U },
    };
    first.triangleMetadata.push_back(TriangleMetadata{ .rawU16 = { 1U, 2U, 3U } });
    BlenderIrTriangleSet second{};
    second.corners = {
        BlenderIrCorner{ .vertexIndex = 0U },
        BlenderIrCorner{ .vertexIndex = 2U },
        BlenderIrCorner{ .vertexIndex = 3U },
    };
    second.triangleMetadata.push_back(TriangleMetadata{ .rawU16 = { 4U, 5U, 6U } });
    mesh.triangleSets = { std::move(first), std::move(second) };
    return mesh;
}

[[nodiscard]] BlenderIrObjectTree makeTree() {
    BlenderIrObjectTree tree{};
    tree.sourceObjectAddress = 0x1200U;
    BlenderIrNode root{};
    root.sourceNodeOffset = 0x1220U;
    root.localTransform.position.x = 10.0F;
    root.childNodeIndices = { 1U };
    BlenderIrNode child{};
    child.sourceNodeOffset = 0x1240U;
    child.sourceAttachOffset = 0x1300U;
    child.localTransform.position.x = 2.0F;
    child.parentNodeIndex = 0U;
    child.meshIndex = 0U;
    tree.nodes = { std::move(root), std::move(child) };
    tree.rootNodeIndices = { 0U };
    return tree;
}

[[nodiscard]] BlenderIrInstance makeInstance(
    const std::uint32_t entryId,
    const float x,
    const std::string_view fxn = "wall") {
    BlenderIrInstance instance{};
    instance.sourceEntryId = entryId;
    instance.tableIndex = entryId;
    instance.tblId = static_cast<std::int32_t>(entryId + 100U);
    instance.fxnName = fxn;
    instance.transform.position.x = x;
    instance.objectTreeIndices = { 0U };
    return instance;
}

[[nodiscard]] bool hasDiagnosticContaining(
    const RegionMeshProjectionResult& result,
    const std::string_view needle) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] RegionMeshProjectionTarget makeTarget(
    const BlenderIrInstance& instance,
    const NavigationRegionKind kind) {
    return RegionMeshProjectionTarget{
        .kind = kind,
        .sourceEntryId = instance.sourceEntryId,
        .tblId = instance.tblId,
    };
}

[[nodiscard]] RegionMeshProjectionResult project(
    const BlenderIrScene& scene,
    const std::initializer_list<RegionMeshProjectionTarget> targets) {
    const std::vector<RegionMeshProjectionTarget> targetVector(targets);
    return RegionMeshProjector{}.project(scene, targetVector);
}

TEST(RegionMeshProjector, ProjectsTargetThroughEntryAndNodeHierarchyAndExcludesNonTargets) {
    BlenderIrScene scene{};
    scene.meshes.push_back(makeQuadMesh());
    scene.objectTrees.push_back(makeTree());
    scene.indexEntries.push_back(makeInstance(7U, 100.0F, " WALL "));
    scene.indexEntries.push_back(makeInstance(8U, 0.0F, "walluv"));
    scene.indexEntries.push_back(makeInstance(9U, 0.0F, "unknown"));

    const auto result = project(scene, {
        makeTarget(scene.indexEntries[0], NavigationRegionKind::Collision),
    });

    ASSERT_EQ(result.regions.size(), 1U);
    ASSERT_TRUE(result.regions.front().complete);
    EXPECT_EQ(result.regions.front().kind, NavigationRegionKind::Collision);
    ASSERT_EQ(result.regions.front().meshes.size(), 1U);
    const auto& projected = result.regions.front().meshes.front();
    ASSERT_EQ(projected.mesh.vertices.size(), 4U);
    EXPECT_EQ(projected.mesh.indices.size(), 6U);
    ASSERT_EQ(projected.mesh.triangleMetadata.size(), 2U);
    EXPECT_EQ(projected.mesh.triangleMetadata[0].rawU16[0], 1U);
    EXPECT_EQ(projected.mesh.triangleMetadata[1].rawU16[0], 4U);
    EXPECT_FLOAT_EQ(projected.mesh.vertices.front().position.x, 112.0F);
    EXPECT_EQ(projected.sourceObjectAddress, 0x1200U);
    EXPECT_EQ(projected.sourceNodeOffset, 0x1240U);
    EXPECT_EQ(projected.sourceAttachOffset, 0x1300U);
}

TEST(RegionMeshProjector, PreservesRepeatedTriggerInstancesAtDistinctWorldTransforms) {
    BlenderIrScene scene{};
    scene.meshes.push_back(makeQuadMesh());
    scene.objectTrees.push_back(makeTree());
    scene.indexEntries.push_back(makeInstance(1U, 1.0F, "goscript"));
    scene.indexEntries.push_back(makeInstance(2U, 10.0F, "treasure"));

    const auto result = project(scene, {
        makeTarget(scene.indexEntries[0], NavigationRegionKind::Trigger),
        makeTarget(scene.indexEntries[1], NavigationRegionKind::Trigger),
    });

    ASSERT_EQ(result.regions.size(), 2U);
    EXPECT_EQ(result.regions[0].kind, NavigationRegionKind::Trigger);
    EXPECT_EQ(result.regions[1].kind, NavigationRegionKind::Trigger);
    ASSERT_FALSE(result.regions[0].meshes.empty());
    ASSERT_FALSE(result.regions[1].meshes.empty());
    EXPECT_FLOAT_EQ(result.regions[0].meshes[0].mesh.vertices[0].position.x, 13.0F);
    EXPECT_FLOAT_EQ(result.regions[1].meshes[0].mesh.vertices[0].position.x, 22.0F);
}

TEST(RegionMeshProjector, ProjectsMultipleMeshesForOneTriggerTarget) {
    BlenderIrScene scene{};
    scene.meshes.push_back(makeQuadMesh());
    auto secondMesh = makeQuadMesh();
    secondMesh.sourceObjectAddress = 0x2200U;
    scene.meshes.push_back(std::move(secondMesh));

    auto tree = makeTree();
    auto secondNode = tree.nodes[1];
    secondNode.sourceNodeOffset = 0x2240U;
    secondNode.meshIndex = 1U;
    tree.nodes.push_back(std::move(secondNode));
    scene.objectTrees.push_back(std::move(tree));
    scene.indexEntries.push_back(makeInstance(30U, 0.0F, "treasure"));

    const auto result = project(scene, {
        makeTarget(scene.indexEntries[0], NavigationRegionKind::Trigger),
    });

    ASSERT_EQ(result.regions.size(), 1U);
    ASSERT_TRUE(result.regions[0].complete);
    ASSERT_EQ(result.regions[0].meshes.size(), 2U);
    EXPECT_EQ(result.regions[0].meshes[0].sourceObjectAddress, 0x1200U);
    EXPECT_EQ(result.regions[0].meshes[1].sourceObjectAddress, 0x2200U);
}

TEST(RegionMeshProjector, ProjectsExplicitMovingObjectTarget) {
    BlenderIrScene scene{};
    scene.meshes.push_back(makeQuadMesh());
    scene.objectTrees.push_back(makeTree());
    scene.indexEntries.push_back(makeInstance(31U, 4.0F, "motscpt"));

    const auto result = project(scene, {
        makeTarget(scene.indexEntries[0], NavigationRegionKind::MovingObject),
    });

    ASSERT_EQ(result.regions.size(), 1U);
    ASSERT_TRUE(result.regions[0].complete);
    EXPECT_EQ(result.regions[0].kind, NavigationRegionKind::MovingObject);
    ASSERT_EQ(result.regions[0].meshes.size(), 1U);
    EXPECT_FLOAT_EQ(result.regions[0].meshes[0].mesh.vertices[0].position.x, 16.0F);
}

TEST(RegionMeshProjector, UsesWeightedBindingRootForStaticBindPosePlacement) {
    BlenderIrScene scene{};
    auto mesh = makeQuadMesh();
    mesh.weightedBinding = BlenderIrWeightedBinding{
        .rootNodeIndex = 0U,
        .sourceNodeIndex = 1U,
        .nodeIndices = { 0U, 1U },
    };
    scene.meshes.push_back(std::move(mesh));
    scene.objectTrees.push_back(makeTree());
    scene.indexEntries.push_back(makeInstance(3U, 100.0F, "wallmot"));

    const auto result = project(scene, {
        makeTarget(scene.indexEntries[0], NavigationRegionKind::Trigger),
    });

    ASSERT_EQ(result.regions.size(), 1U);
    ASSERT_FALSE(result.regions[0].meshes.empty());
    EXPECT_FLOAT_EQ(result.regions[0].meshes[0].mesh.vertices[0].position.x, 110.0F);
}

TEST(RegionMeshProjector, RejectsOnlyTrianglesWithInvalidVertices) {
    BlenderIrScene scene{};
    auto mesh = makeQuadMesh();
    mesh.triangleSets[1].corners[2].vertexIndex = 99U;
    scene.meshes.push_back(std::move(mesh));
    scene.objectTrees.push_back(makeTree());
    scene.indexEntries.push_back(makeInstance(4U, 0.0F, "goscript"));

    const auto result = project(scene, {
        makeTarget(scene.indexEntries[0], NavigationRegionKind::Trigger),
    });

    ASSERT_EQ(result.regions.size(), 1U);
    ASSERT_TRUE(result.regions[0].complete);
    ASSERT_EQ(result.regions[0].meshes.size(), 1U);
    EXPECT_EQ(result.regions[0].meshes[0].mesh.indices.size(), 3U);
    EXPECT_TRUE(hasDiagnosticContaining(result, "rejected 1 triangle"));
}

TEST(RegionMeshProjector, ReportsIncompleteTargetsWhenTreeOrWeightedRootIsMissing) {
    BlenderIrScene missingTree{};
    auto missingTreeInstance = makeInstance(5U, 0.0F);
    missingTreeInstance.objectTreeIndices = { 12U };
    missingTree.indexEntries.push_back(std::move(missingTreeInstance));

    const auto treeResult = project(missingTree, {
        makeTarget(missingTree.indexEntries[0], NavigationRegionKind::Collision),
    });
    ASSERT_EQ(treeResult.regions.size(), 1U);
    EXPECT_FALSE(treeResult.regions[0].complete);
    EXPECT_TRUE(hasDiagnosticContaining(treeResult, "missing object tree"));

    BlenderIrScene badWeightedRoot{};
    auto mesh = makeQuadMesh();
    mesh.weightedBinding = BlenderIrWeightedBinding{ .rootNodeIndex = 99U, .sourceNodeIndex = 1U };
    badWeightedRoot.meshes.push_back(std::move(mesh));
    badWeightedRoot.objectTrees.push_back(makeTree());
    badWeightedRoot.indexEntries.push_back(makeInstance(6U, 0.0F, "treasure"));

    const auto weightedResult = project(badWeightedRoot, {
        makeTarget(badWeightedRoot.indexEntries[0], NavigationRegionKind::Trigger),
    });
    ASSERT_EQ(weightedResult.regions.size(), 1U);
    EXPECT_FALSE(weightedResult.regions[0].complete);
    EXPECT_TRUE(hasDiagnosticContaining(weightedResult, "invalid weighted root"));
}

TEST(RegionMeshProjector, ReportsMissingMeshAndUnmatchedTriggerWithoutProjectingOtherInstances) {
    BlenderIrScene scene{};
    auto tree = makeTree();
    tree.nodes[1].meshIndex = 99U;
    scene.objectTrees.push_back(std::move(tree));
    scene.indexEntries.push_back(makeInstance(10U, 0.0F, "goscript"));
    scene.indexEntries.push_back(makeInstance(11U, 0.0F, "walluv"));

    const RegionMeshProjectionTarget unmatched{
        .kind = NavigationRegionKind::Trigger,
        .sourceEntryId = 12U,
        .tblId = 112,
    };
    const auto result = project(scene, {
        makeTarget(scene.indexEntries[0], NavigationRegionKind::Trigger),
        unmatched,
    });

    ASSERT_EQ(result.regions.size(), 2U);
    EXPECT_FALSE(result.regions[0].complete);
    EXPECT_FALSE(result.regions[1].complete);
    EXPECT_TRUE(hasDiagnosticContaining(result, "Trigger entry=10 references missing mesh"));
    EXPECT_TRUE(hasDiagnosticContaining(result, "Trigger entry=12 tbl=112 has no matching"));
}

TEST(RegionMeshProjector, IncompleteTriggerDoesNotChangeCompleteWallProjection) {
    BlenderIrScene scene{};
    scene.meshes.push_back(makeQuadMesh());
    scene.objectTrees.push_back(makeTree());
    scene.indexEntries.push_back(makeInstance(20U, 0.0F, "wall"));
    auto trigger = makeInstance(21U, 0.0F, "goscript");
    trigger.objectTreeIndices = { 99U };
    scene.indexEntries.push_back(std::move(trigger));

    const auto result = project(scene, {
        makeTarget(scene.indexEntries[0], NavigationRegionKind::Collision),
        makeTarget(scene.indexEntries[1], NavigationRegionKind::Trigger),
    });

    ASSERT_EQ(result.regions.size(), 2U);
    EXPECT_TRUE(result.regions[0].complete);
    EXPECT_EQ(result.regions[0].kind, NavigationRegionKind::Collision);
    EXPECT_FALSE(result.regions[1].complete);
    EXPECT_EQ(result.regions[1].kind, NavigationRegionKind::Trigger);
}

} // namespace
} // namespace savor::navigation
