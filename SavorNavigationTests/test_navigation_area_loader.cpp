#include "Loading/NavigationAreaLoader.h"
#include "Loading/NavigationGroundFallbackNormalizer.h"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace savor::navigation {
namespace {

class TemporaryFile final {
public:
    explicit TemporaryFile(const std::string_view suffix) {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("savor-navigation-" + std::to_string(nonce) + std::string(suffix));
    }

    ~TemporaryFile() {
        std::error_code error{};
        std::filesystem::remove(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_{};
};

[[nodiscard]] bool hasDiagnosticContaining(
    const NavigationAreaLoadResult& result,
    const std::string_view needle) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::string normalizeFxn(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

[[nodiscard]] const NavigationAuthoredGroundFallbackChain* findFallbackChain(
    const NavigationAreaModel& model,
    const std::size_t sourceTableIndex) {
    const auto found = std::find_if(model.authoredGroundFallbackChains.begin(),
        model.authoredGroundFallbackChains.end(), [sourceTableIndex](const auto& chain) {
            return chain.sourceTableIndex == sourceTableIndex;
        });
    return found == model.authoredGroundFallbackChains.end() ? nullptr : &*found;
}

[[nodiscard]] NavigationSurface makeFallbackSurface(
    const std::size_t tableIndex,
    const std::uint32_t entryId,
    const std::uint32_t blockOffset) {
    return NavigationSurface{
        .sourceKey = NavigationSurfaceSourceKey{
            .sourceEntryId = entryId,
            .sourceBlockOffset = blockOffset,
        },
        .sourceTableIndex = tableIndex,
    };
}

TEST(NavigationAreaLoader, MissingFileFailsWithoutModel) {
    const auto result = NavigationAreaLoader{}.loadFile(
        std::filesystem::temp_directory_path() / "savor-navigation-file-that-does-not-exist.mld");

    EXPECT_EQ(result.status, NavigationAreaLoadStatus::Failed);
    EXPECT_FALSE(result.hasModel());
    EXPECT_TRUE(hasDiagnosticContaining(result, "Could not inspect MLD file"));
}

TEST(NavigationAreaLoader, EmptyFileFailsWithoutModel) {
    TemporaryFile file("-empty.mld");
    std::ofstream(file.path(), std::ios::binary);

    const auto result = NavigationAreaLoader{}.loadFile(file.path());

    EXPECT_EQ(result.status, NavigationAreaLoadStatus::Failed);
    EXPECT_FALSE(result.hasModel());
    EXPECT_TRUE(hasDiagnosticContaining(result, "is empty"));
}

TEST(NavigationAreaLoader, TruncatedAklzReportsDecompressionFailure) {
    TemporaryFile file("-truncated-aklz.mld");
    constexpr std::array<unsigned char, 16> bytes{
        'A', 'K', 'L', 'Z', '~', '?', 'Q', 'd', '=', 0xCC, 0xCC, 0xCD,
        0x00, 0x00, 0x00, 0x04,
    };
    {
        std::ofstream output(file.path(), std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    const auto result = NavigationAreaLoader{}.loadFile(file.path());

    EXPECT_EQ(result.status, NavigationAreaLoadStatus::Failed);
    EXPECT_FALSE(result.hasModel());
    EXPECT_TRUE(hasDiagnosticContaining(result, "AKLZ decompression failed"));
}

TEST(NavigationGroundFallbackNormalizer, PreservesEntryZeroOrderAndDuplicateTargets) {
    const std::vector<detail::NavigationGroundFallbackEntryEvidence> entries{
        detail::NavigationGroundFallbackEntryEvidence{ .tableIndex = 7U, .entryId = 0U },
        detail::NavigationGroundFallbackEntryEvidence{
            .tableIndex = 8U,
            .entryId = 1U,
            .targetEntryIds = { 0U, 2U, 2U },
        },
        detail::NavigationGroundFallbackEntryEvidence{ .tableIndex = 9U, .entryId = 2U },
    };
    const std::vector<NavigationSurface> surfaces{
        makeFallbackSurface(7U, 0U, 70U),
        makeFallbackSurface(8U, 1U, 80U),
        makeFallbackSurface(9U, 2U, 90U),
    };

    const auto normalized = detail::NavigationGroundFallbackNormalizer{}.normalize(entries, surfaces);

    ASSERT_EQ(normalized.chains.size(), 1U);
    const auto& chain = normalized.chains[0];
    ASSERT_EQ(chain.targets.size(), 3U);
    EXPECT_EQ(chain.targets[0].targetEntryId, 0U);
    EXPECT_EQ(chain.targets[0].targetTableIndex, 7U);
    EXPECT_EQ(chain.targets[1].targetEntryId, 2U);
    EXPECT_EQ(chain.targets[2].targetEntryId, 2U);
    for (std::size_t ordinal = 0; ordinal < chain.targets.size(); ++ordinal) {
        EXPECT_EQ(chain.targets[ordinal].authoredOrdinal, ordinal);
        EXPECT_EQ(chain.targets[ordinal].status,
            NavigationAuthoredGroundFallbackTargetStatus::Resolved);
        EXPECT_FALSE(chain.targets[ordinal].targetSurfaces.empty());
    }
}

TEST(NavigationGroundFallbackNormalizer, MissingEntryTruncatesAndRetainsSuppressedProvenance) {
    const std::vector<detail::NavigationGroundFallbackEntryEvidence> entries{
        detail::NavigationGroundFallbackEntryEvidence{ .tableIndex = 10U, .entryId = 5U },
        detail::NavigationGroundFallbackEntryEvidence{
            .tableIndex = 11U,
            .entryId = 1U,
            .targetEntryIds = { 0U, 5U, 0U },
        },
        detail::NavigationGroundFallbackEntryEvidence{ .tableIndex = 12U, .entryId = 0U },
        detail::NavigationGroundFallbackEntryEvidence{ .tableIndex = 13U, .entryId = 0U },
    };
    const std::vector<NavigationSurface> surfaces{
        makeFallbackSurface(11U, 1U, 110U),
        makeFallbackSurface(12U, 0U, 120U),
        makeFallbackSurface(13U, 0U, 130U),
    };

    const auto normalized = detail::NavigationGroundFallbackNormalizer{}.normalize(entries, surfaces);

    ASSERT_EQ(normalized.chains.size(), 1U);
    const auto& targets = normalized.chains[0].targets;
    ASSERT_EQ(targets.size(), 3U);
    ASSERT_TRUE(targets[0].targetTableIndex.has_value());
    EXPECT_EQ(*targets[0].targetTableIndex, 12U);
    EXPECT_EQ(targets[0].status, NavigationAuthoredGroundFallbackTargetStatus::Resolved);
    // EntryID 5 exists in slot 0, but 5 is out of range for a four-slot table;
    // LinkGrounds rejects it before the linear scan.
    EXPECT_EQ(targets[1].targetEntryId, 5U);
    EXPECT_EQ(targets[1].status, NavigationAuthoredGroundFallbackTargetStatus::MissingEntry);
    EXPECT_EQ(targets[2].targetEntryId, 0U);
    EXPECT_EQ(targets[2].status,
        NavigationAuthoredGroundFallbackTargetStatus::SuppressedAfterMissingEntry);
    EXPECT_TRUE(std::any_of(normalized.diagnostics.begin(), normalized.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.message.find("effective runtime fallback chain stops here") != std::string::npos;
    }));
}

TEST(NavigationGroundFallbackNormalizer, MissingGeometryAllowsALaterResolvedTarget) {
    const std::vector<detail::NavigationGroundFallbackEntryEvidence> entries{
        detail::NavigationGroundFallbackEntryEvidence{ .tableIndex = 0U, .entryId = 0U },
        detail::NavigationGroundFallbackEntryEvidence{
            .tableIndex = 1U,
            .entryId = 1U,
            .targetEntryIds = { 2U, 0U },
        },
        detail::NavigationGroundFallbackEntryEvidence{ .tableIndex = 2U, .entryId = 2U },
    };
    const std::vector<NavigationSurface> surfaces{
        makeFallbackSurface(0U, 0U, 10U),
        makeFallbackSurface(1U, 1U, 11U),
    };

    const auto normalized = detail::NavigationGroundFallbackNormalizer{}.normalize(entries, surfaces);

    ASSERT_EQ(normalized.chains.size(), 1U);
    const auto& targets = normalized.chains[0].targets;
    ASSERT_EQ(targets.size(), 2U);
    EXPECT_EQ(targets[0].status, NavigationAuthoredGroundFallbackTargetStatus::MissingGeometry);
    EXPECT_EQ(targets[0].targetTableIndex, 2U);
    EXPECT_TRUE(targets[0].targetSurfaces.empty());
    EXPECT_EQ(targets[1].status, NavigationAuthoredGroundFallbackTargetStatus::Resolved);
    EXPECT_EQ(targets[1].targetTableIndex, 0U);
    EXPECT_FALSE(targets[1].targetSurfaces.empty());
}

TEST(NavigationAreaLoader, A101bIncludesGrndAndGroundRoleGobjSurfaces) {
    const char* fixturePath = std::getenv("SAVOR_NAV_A101B_MLD");
    if (fixturePath == nullptr || *fixturePath == '\0') {
        GTEST_SKIP() << "Set SAVOR_NAV_A101B_MLD to the extracted a101b.mld fixture.";
    }
    if (!std::filesystem::is_regular_file(fixturePath)) {
        GTEST_SKIP() << "SAVOR_NAV_A101B_MLD does not identify a regular file.";
    }

    const auto result = NavigationAreaLoader{}.loadFile(fixturePath);
    ASSERT_EQ(result.status, NavigationAreaLoadStatus::Complete);
    ASSERT_TRUE(result.hasModel());
    const auto& model = *result.model;

    std::size_t grndSurfaceCount = 0;
    std::size_t gobjSurfaceCount = 0;
    std::size_t vertexCount = 0;
    std::size_t triangleCount = 0;
    for (const auto& surface : model.surfaces) {
        grndSurfaceCount += surface.sourceKind == NavigationSurfaceSourceKind::Grnd ? 1U : 0U;
        gobjSurfaceCount += surface.sourceKind == NavigationSurfaceSourceKind::Gobj ? 1U : 0U;
        vertexCount += surface.mesh.vertices.size();
        triangleCount += surface.mesh.indices.size() / 3U;
    }

    std::size_t collisionCount = 0;
    std::size_t triggerCount = 0;
    std::size_t wallRegionCount = 0;
    std::size_t wallMeshCount = 0;
    std::size_t wallVertexCount = 0;
    std::size_t wallTriangleCount = 0;
    std::size_t triggerMeshCount = 0;
    std::size_t triggerVertexCount = 0;
    std::size_t triggerTriangleCount = 0;
    std::size_t movingObjectRegionCount = 0;
    std::size_t movingObjectMeshCount = 0;
    std::size_t movingObjectVertexCount = 0;
    std::size_t movingObjectTriangleCount = 0;
    std::size_t unknownRegionCount = 0;
    struct TriggerGeometryCounts {
        std::size_t regions = 0;
        std::size_t meshes = 0;
        std::size_t vertices = 0;
        std::size_t triangles = 0;
    };
    std::unordered_map<std::string, TriggerGeometryCounts> triggerGeometryByFxn{};
    std::set<std::uint32_t> wallObjectAddresses{};
    std::unordered_map<std::uint32_t, std::vector<std::array<double, 3>>> centroidsByObjectAddress{};
    for (const auto& region : model.regions) {
        collisionCount += region.kind == NavigationRegionKind::Collision ? 1U : 0U;
        triggerCount += region.kind == NavigationRegionKind::Trigger ? 1U : 0U;
        unknownRegionCount += region.kind == NavigationRegionKind::Unknown ? 1U : 0U;
        if (region.kind == NavigationRegionKind::MovingObject) {
            ++movingObjectRegionCount;
            EXPECT_EQ(normalizeFxn(region.fxnName), "motscpt");
            EXPECT_FALSE(region.meshes.empty());
            for (const auto& regionMesh : region.meshes) {
                ++movingObjectMeshCount;
                movingObjectVertexCount += regionMesh.mesh.vertices.size();
                movingObjectTriangleCount += regionMesh.mesh.indices.size() / 3U;
            }
        }
        if (region.kind == NavigationRegionKind::Trigger) {
            auto& counts = triggerGeometryByFxn[normalizeFxn(region.fxnName)];
            ++counts.regions;
            EXPECT_FALSE(region.meshes.empty());
            for (const auto& regionMesh : region.meshes) {
                ++counts.meshes;
                counts.vertices += regionMesh.mesh.vertices.size();
                counts.triangles += regionMesh.mesh.indices.size() / 3U;
                ++triggerMeshCount;
                triggerVertexCount += regionMesh.mesh.vertices.size();
                triggerTriangleCount += regionMesh.mesh.indices.size() / 3U;
            }
        }
        if (region.kind != NavigationRegionKind::Collision || normalizeFxn(region.fxnName) != "wall") {
            continue;
        }
        ++wallRegionCount;
        EXPECT_FALSE(region.meshes.empty());
        for (const auto& regionMesh : region.meshes) {
            ++wallMeshCount;
            wallVertexCount += regionMesh.mesh.vertices.size();
            wallTriangleCount += regionMesh.mesh.indices.size() / 3U;
            wallObjectAddresses.insert(regionMesh.sourceObjectAddress);
            std::array<double, 3> centroid{};
            for (const auto& vertex : regionMesh.mesh.vertices) {
                centroid[0] += vertex.position.x;
                centroid[1] += vertex.position.y;
                centroid[2] += vertex.position.z;
            }
            if (!regionMesh.mesh.vertices.empty()) {
                const auto divisor = static_cast<double>(regionMesh.mesh.vertices.size());
                centroid[0] /= divisor;
                centroid[1] /= divisor;
                centroid[2] /= divisor;
            }
            centroidsByObjectAddress[regionMesh.sourceObjectAddress].push_back(centroid);
        }
    }

    bool foundDistinctSharedInstance = false;
    for (const auto& [_, centroids] : centroidsByObjectAddress) {
        for (std::size_t lhs = 0; lhs < centroids.size() && !foundDistinctSharedInstance; ++lhs) {
            for (std::size_t rhs = lhs + 1U; rhs < centroids.size(); ++rhs) {
                const double distance = std::abs(centroids[lhs][0] - centroids[rhs][0]) +
                    std::abs(centroids[lhs][1] - centroids[rhs][1]) +
                    std::abs(centroids[lhs][2] - centroids[rhs][2]);
                if (distance > 1.0e-4) {
                    foundDistinctSharedInstance = true;
                    break;
                }
            }
        }
    }

    EXPECT_TRUE(model.hasCompleteGroundGeometry);
    EXPECT_TRUE(model.hasCompleteWallGeometry);
    EXPECT_TRUE(model.hasCompleteTriggerGeometry);
    EXPECT_TRUE(model.hasCompleteMovingObjectGeometry);
    EXPECT_TRUE(model.bounds.valid);
    EXPECT_EQ(model.failedGroundResourceCount, 0U);
    EXPECT_EQ(model.failedWallRegionCount, 0U);
    EXPECT_EQ(model.failedTriggerRegionCount, 0U);
    EXPECT_EQ(model.failedMovingObjectRegionCount, 0U);
    EXPECT_EQ(model.surfaces.size(), 12U);
    EXPECT_EQ(grndSurfaceCount, 6U);
    EXPECT_EQ(gobjSurfaceCount, 6U);
    EXPECT_EQ(vertexCount, 504U);
    EXPECT_EQ(triangleCount, 401U);
    EXPECT_TRUE(std::all_of(model.surfaces.begin(), model.surfaces.end(), [](const auto& surface) {
        return surface.traversalAvailability == NavigationSurfaceTraversalAvailability::Static;
    }));
    ASSERT_EQ(model.authoredGroundFallbackChains.size(), 7U);
    std::size_t authoredFallbackTargetCount = 0;
    for (const auto& chain : model.authoredGroundFallbackChains) {
        authoredFallbackTargetCount += chain.targets.size();
        EXPECT_FALSE(chain.sourceSurfaces.empty());
        for (const auto& target : chain.targets) {
            EXPECT_EQ(target.status, NavigationAuthoredGroundFallbackTargetStatus::Resolved);
            EXPECT_TRUE(target.targetTableIndex.has_value());
            EXPECT_FALSE(target.targetSurfaces.empty());
        }
    }
    EXPECT_EQ(authoredFallbackTargetCount, 12U);

    const auto* lowerFloorChain = findFallbackChain(model, 33U);
    ASSERT_NE(lowerFloorChain, nullptr);
    ASSERT_EQ(lowerFloorChain->sourceEntryId, 33U);
    ASSERT_EQ(lowerFloorChain->sourceSurfaces.size(), 2U);
    ASSERT_EQ(lowerFloorChain->targets.size(), 3U);
    EXPECT_EQ(lowerFloorChain->targets[0].targetEntryId, 32U);
    EXPECT_EQ(lowerFloorChain->targets[1].targetEntryId, 34U);
    EXPECT_EQ(lowerFloorChain->targets[2].targetEntryId, 35U);
    for (std::size_t index = 0; index < lowerFloorChain->targets.size(); ++index) {
        EXPECT_EQ(lowerFloorChain->targets[index].authoredOrdinal, index);
    }

    const auto* staircaseChain = findFallbackChain(model, 34U);
    ASSERT_NE(staircaseChain, nullptr);
    ASSERT_EQ(staircaseChain->targets.size(), 2U);
    EXPECT_EQ(staircaseChain->targets[0].targetEntryId, 33U);
    EXPECT_EQ(staircaseChain->targets[1].targetEntryId, 36U);
    EXPECT_EQ(collisionCount, 59U);
    EXPECT_EQ(triggerCount, 16U);
    EXPECT_EQ(model.unknownEntryCount, 21U);
    EXPECT_EQ(unknownRegionCount, 21U);
    EXPECT_EQ(wallRegionCount, 51U);
    EXPECT_EQ(wallMeshCount, 517U);
    EXPECT_EQ(wallVertexCount, 6795U);
    EXPECT_EQ(wallTriangleCount, 8347U);
    EXPECT_EQ(wallObjectAddresses.size(), 18U);
    EXPECT_TRUE(foundDistinctSharedInstance);
    EXPECT_EQ(triggerMeshCount, 38U);
    EXPECT_EQ(triggerVertexCount, 320U);
    EXPECT_EQ(triggerTriangleCount, 384U);
    EXPECT_EQ(movingObjectRegionCount, 11U);
    EXPECT_EQ(movingObjectMeshCount, 55U);
    EXPECT_EQ(movingObjectVertexCount, 462U);
    EXPECT_EQ(movingObjectTriangleCount, 566U);

    ASSERT_EQ(triggerGeometryByFxn.size(), 3U);
    const auto goscript = triggerGeometryByFxn.find("goscript");
    const auto treasure = triggerGeometryByFxn.find("treasure");
    const auto wallmot = triggerGeometryByFxn.find("wallmot");
    ASSERT_NE(goscript, triggerGeometryByFxn.end());
    ASSERT_NE(treasure, triggerGeometryByFxn.end());
    ASSERT_NE(wallmot, triggerGeometryByFxn.end());
    EXPECT_EQ(goscript->second.regions, 11U);
    EXPECT_EQ(goscript->second.meshes, 11U);
    EXPECT_EQ(goscript->second.vertices, 88U);
    EXPECT_EQ(goscript->second.triangles, 132U);
    EXPECT_EQ(treasure->second.regions, 4U);
    EXPECT_EQ(treasure->second.meshes, 20U);
    EXPECT_EQ(treasure->second.vertices, 144U);
    EXPECT_EQ(treasure->second.triangles, 168U);
    EXPECT_EQ(wallmot->second.regions, 1U);
    EXPECT_EQ(wallmot->second.meshes, 7U);
    EXPECT_EQ(wallmot->second.vertices, 88U);
    EXPECT_EQ(wallmot->second.triangles, 84U);
}

TEST(NavigationAreaLoader, A005bPreservesEntryIdZeroInAuthoredFallbackOrder) {
    const char* fixturePath = std::getenv("SAVOR_NAV_A005B_MLD");
    if (fixturePath == nullptr || *fixturePath == '\0') {
        GTEST_SKIP() << "Set SAVOR_NAV_A005B_MLD to the extracted a005b.mld fixture.";
    }
    if (!std::filesystem::is_regular_file(fixturePath)) {
        GTEST_SKIP() << "SAVOR_NAV_A005B_MLD does not identify a regular file.";
    }

    const auto result = NavigationAreaLoader{}.loadFile(fixturePath);
    ASSERT_NE(result.status, NavigationAreaLoadStatus::Failed);
    ASSERT_TRUE(result.hasModel());
    const auto& model = *result.model;

    const auto* chain = findFallbackChain(model, 1U);
    ASSERT_NE(chain, nullptr);
    EXPECT_EQ(chain->sourceEntryId, 1U);
    ASSERT_EQ(chain->targets.size(), 2U);

    const auto& entryZeroTarget = chain->targets[0];
    EXPECT_EQ(entryZeroTarget.authoredOrdinal, 0U);
    EXPECT_EQ(entryZeroTarget.targetEntryId, 0U);
    EXPECT_EQ(entryZeroTarget.status, NavigationAuthoredGroundFallbackTargetStatus::Resolved);
    ASSERT_TRUE(entryZeroTarget.targetTableIndex.has_value());
    EXPECT_EQ(*entryZeroTarget.targetTableIndex, 0U);
    EXPECT_FALSE(entryZeroTarget.targetSurfaces.empty());

    const auto& secondTarget = chain->targets[1];
    EXPECT_EQ(secondTarget.authoredOrdinal, 1U);
    EXPECT_EQ(secondTarget.targetEntryId, 2U);
    EXPECT_EQ(secondTarget.status, NavigationAuthoredGroundFallbackTargetStatus::Resolved);
}

} // namespace
} // namespace savor::navigation
