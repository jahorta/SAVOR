#include "Loading/NavigationAreaLoader.h"

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
    std::set<std::uint32_t> wallObjectAddresses{};
    std::unordered_map<std::uint32_t, std::vector<std::array<double, 3>>> centroidsByObjectAddress{};
    for (const auto& region : model.regions) {
        collisionCount += region.kind == NavigationRegionKind::Collision ? 1U : 0U;
        triggerCount += region.kind == NavigationRegionKind::Trigger ? 1U : 0U;
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
    EXPECT_TRUE(model.bounds.valid);
    EXPECT_EQ(model.failedGroundResourceCount, 0U);
    EXPECT_EQ(model.failedWallRegionCount, 0U);
    EXPECT_EQ(model.surfaces.size(), 12U);
    EXPECT_EQ(grndSurfaceCount, 6U);
    EXPECT_EQ(gobjSurfaceCount, 6U);
    EXPECT_EQ(vertexCount, 504U);
    EXPECT_EQ(triangleCount, 401U);
    EXPECT_EQ(collisionCount, 59U);
    EXPECT_EQ(triggerCount, 16U);
    EXPECT_EQ(model.unknownEntryCount, 32U);
    EXPECT_EQ(wallRegionCount, 51U);
    EXPECT_EQ(wallMeshCount, 517U);
    EXPECT_EQ(wallVertexCount, 6795U);
    EXPECT_EQ(wallTriangleCount, 8347U);
    EXPECT_EQ(wallObjectAddresses.size(), 18U);
    EXPECT_TRUE(foundDistinctSharedInstance);
}

} // namespace
} // namespace savor::navigation
