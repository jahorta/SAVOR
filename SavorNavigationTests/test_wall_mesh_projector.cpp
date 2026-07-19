#include "Projection/WallMeshProjector.h"

#include <gtest/gtest.h>

#include <string_view>

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

[[nodiscard]] BlenderIrInstance makeWallInstance(
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
    const WallMeshProjectionResult& result,
    const std::string_view needle) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

TEST(WallMeshProjector, ProjectsMultipleTriangleSetsThroughEntryAndNodeHierarchy) {
    BlenderIrScene scene{};
    scene.meshes.push_back(makeQuadMesh());
    scene.objectTrees.push_back(makeTree());
    scene.indexEntries.push_back(makeWallInstance(7U, 100.0F, " WALL "));
    scene.indexEntries.push_back(makeWallInstance(8U, 0.0F, "walluv"));

    const auto result = WallMeshProjector{}.project(scene);

    ASSERT_EQ(result.regions.size(), 1U);
    ASSERT_TRUE(result.regions.front().complete);
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

TEST(WallMeshProjector, PreservesRepeatedObjectInstancesAtDistinctWorldTransforms) {
    BlenderIrScene scene{};
    scene.meshes.push_back(makeQuadMesh());
    scene.objectTrees.push_back(makeTree());
    scene.indexEntries.push_back(makeWallInstance(1U, 1.0F));
    scene.indexEntries.push_back(makeWallInstance(2U, 10.0F));

    const auto result = WallMeshProjector{}.project(scene);

    ASSERT_EQ(result.regions.size(), 2U);
    ASSERT_FALSE(result.regions[0].meshes.empty());
    ASSERT_FALSE(result.regions[1].meshes.empty());
    EXPECT_FLOAT_EQ(result.regions[0].meshes[0].mesh.vertices[0].position.x, 13.0F);
    EXPECT_FLOAT_EQ(result.regions[1].meshes[0].mesh.vertices[0].position.x, 22.0F);
}

TEST(WallMeshProjector, UsesWeightedBindingRootForStaticBindPosePlacement) {
    BlenderIrScene scene{};
    auto mesh = makeQuadMesh();
    mesh.weightedBinding = BlenderIrWeightedBinding{
        .rootNodeIndex = 0U,
        .sourceNodeIndex = 1U,
        .nodeIndices = { 0U, 1U },
    };
    scene.meshes.push_back(std::move(mesh));
    scene.objectTrees.push_back(makeTree());
    scene.indexEntries.push_back(makeWallInstance(3U, 100.0F));

    const auto result = WallMeshProjector{}.project(scene);

    ASSERT_EQ(result.regions.size(), 1U);
    ASSERT_FALSE(result.regions[0].meshes.empty());
    EXPECT_FLOAT_EQ(result.regions[0].meshes[0].mesh.vertices[0].position.x, 110.0F);
}

TEST(WallMeshProjector, RejectsOnlyTrianglesWithInvalidVertices) {
    BlenderIrScene scene{};
    auto mesh = makeQuadMesh();
    mesh.triangleSets[1].corners[2].vertexIndex = 99U;
    scene.meshes.push_back(std::move(mesh));
    scene.objectTrees.push_back(makeTree());
    scene.indexEntries.push_back(makeWallInstance(4U, 0.0F));

    const auto result = WallMeshProjector{}.project(scene);

    ASSERT_EQ(result.regions.size(), 1U);
    ASSERT_TRUE(result.regions[0].complete);
    ASSERT_EQ(result.regions[0].meshes.size(), 1U);
    EXPECT_EQ(result.regions[0].meshes[0].mesh.indices.size(), 3U);
    EXPECT_TRUE(hasDiagnosticContaining(result, "rejected 1 triangle"));
}

TEST(WallMeshProjector, ReportsIncompleteWallWhenTreeOrWeightedRootIsMissing) {
    BlenderIrScene missingTree{};
    auto missingTreeInstance = makeWallInstance(5U, 0.0F);
    missingTreeInstance.objectTreeIndices = { 12U };
    missingTree.indexEntries.push_back(std::move(missingTreeInstance));

    const auto treeResult = WallMeshProjector{}.project(missingTree);
    ASSERT_EQ(treeResult.regions.size(), 1U);
    EXPECT_FALSE(treeResult.regions[0].complete);
    EXPECT_TRUE(hasDiagnosticContaining(treeResult, "missing object tree"));

    BlenderIrScene badWeightedRoot{};
    auto mesh = makeQuadMesh();
    mesh.weightedBinding = BlenderIrWeightedBinding{ .rootNodeIndex = 99U, .sourceNodeIndex = 1U };
    badWeightedRoot.meshes.push_back(std::move(mesh));
    badWeightedRoot.objectTrees.push_back(makeTree());
    badWeightedRoot.indexEntries.push_back(makeWallInstance(6U, 0.0F));

    const auto weightedResult = WallMeshProjector{}.project(badWeightedRoot);
    ASSERT_EQ(weightedResult.regions.size(), 1U);
    EXPECT_FALSE(weightedResult.regions[0].complete);
    EXPECT_TRUE(hasDiagnosticContaining(weightedResult, "invalid weighted root"));
}

} // namespace
} // namespace savor::navigation
