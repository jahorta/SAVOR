#include "RuntimeSceneConverter.h"

#include <QColor>
#include <QQmlEngine>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <utility>

namespace savor::qt3d::gui {
namespace {

constexpr float kMarkerSize = 15.0F;

[[nodiscard]] PackedVertex makeVertex(const float x, const float y, const float z,
    const float nx = 0.0F, const float ny = 1.0F, const float nz = 0.0F) {
    return PackedVertex{
        .px = x,
        .py = y,
        .pz = z,
        .nx = nx,
        .ny = ny,
        .nz = nz,
    };
}

[[nodiscard]] std::vector<std::uint32_t> cubeIndices() {
    return {
        0, 1, 2, 0, 2, 3,
        4, 6, 5, 4, 7, 6,
        0, 4, 5, 0, 5, 1,
        3, 2, 6, 3, 6, 7,
        1, 5, 6, 1, 6, 2,
        0, 3, 7, 0, 7, 4,
    };
}

[[nodiscard]] std::vector<PackedVertex> cubeVertices(const QVector3D& center, const float halfSize) {
    const auto cx = center.x();
    const auto cy = center.y();
    const auto cz = center.z();
    return {
        makeVertex(cx - halfSize, cy - halfSize, cz - halfSize),
        makeVertex(cx + halfSize, cy - halfSize, cz - halfSize),
        makeVertex(cx + halfSize, cy + halfSize, cz - halfSize),
        makeVertex(cx - halfSize, cy + halfSize, cz - halfSize),
        makeVertex(cx - halfSize, cy - halfSize, cz + halfSize),
        makeVertex(cx + halfSize, cy - halfSize, cz + halfSize),
        makeVertex(cx + halfSize, cy + halfSize, cz + halfSize),
        makeVertex(cx - halfSize, cy + halfSize, cz + halfSize),
    };
}

[[nodiscard]] std::unique_ptr<StaticMeshGeometry> createTriangleGeometry(
    const std::vector<PackedVertex>& vertices,
    const std::vector<std::uint32_t>& indices) {
    auto geometry = std::make_unique<StaticMeshGeometry>();
    QQmlEngine::setObjectOwnership(geometry.get(), QQmlEngine::CppOwnership);
    geometry->setTriangleMesh(vertices, indices);
    return geometry;
}

[[nodiscard]] QString surfaceLabel(const savor::navigation::NavigationSurface& surface) {
    const QString kind = surface.sourceKind == savor::navigation::NavigationSurfaceSourceKind::Grnd
        ? QStringLiteral("GRND")
        : QStringLiteral("GOBJ");
    return QStringLiteral("%1 entry=%2 block=0x%3 node=0x%4")
        .arg(kind)
        .arg(surface.sourceKey.sourceEntryId)
        .arg(surface.sourceKey.sourceBlockOffset, 0, 16)
        .arg(surface.sourceKey.sourceNodeOffset, 0, 16);
}

[[nodiscard]] std::vector<PackedVertex> meshVertices(const savor::navigation::NavigationMesh& mesh) {
    std::vector<PackedVertex> vertices{};
    vertices.reserve(mesh.vertices.size());
    for (const auto& vertex : mesh.vertices) {
        vertices.push_back(makeVertex(
            vertex.position.x,
            vertex.position.y,
            vertex.position.z,
            vertex.normal.x,
            vertex.normal.y,
            vertex.normal.z));
    }
    return vertices;
}

[[nodiscard]] bool isExactWall(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](const unsigned char c) {
        return !std::isspace(c);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](const unsigned char c) {
        return !std::isspace(c);
    }).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value == "wall";
}

[[nodiscard]] std::string severityLabel(const savor::navigation::NavigationDiagnosticSeverity severity) {
    switch (severity) {
    case savor::navigation::NavigationDiagnosticSeverity::Warning:
        return "warning";
    case savor::navigation::NavigationDiagnosticSeverity::Error:
        return "error";
    case savor::navigation::NavigationDiagnosticSeverity::Info:
    default:
        return "info";
    }
}

} // namespace

RuntimeSceneData RuntimeSceneConverter::convert(const savor::navigation::NavigationAreaModel& model) const {
    RuntimeSceneData out{};
    std::size_t grndCount = 0;
    std::size_t gobjCount = 0;
    std::size_t vertexCount = 0;
    std::size_t triangleCount = 0;
    std::size_t wallRegionCount = 0;
    std::size_t wallMeshCount = 0;
    std::size_t wallVertexCount = 0;
    std::size_t wallTriangleCount = 0;

    for (const auto& surface : model.surfaces) {
        auto geometry = createTriangleGeometry(meshVertices(surface.mesh), surface.mesh.indices);
        QVariantMap item{};
        item.insert("geometry", QVariant::fromValue(static_cast<QObject*>(geometry.get())));
        item.insert("color", surface.sourceKind == savor::navigation::NavigationSurfaceSourceKind::Grnd
            ? QColor(QStringLiteral("#6FA8DC"))
            : QColor(QStringLiteral("#93C47D")));
        item.insert("label", surfaceLabel(surface));
        out.grounds.push_back(item);
        out.geometries.push_back(std::move(geometry));

        grndCount += surface.sourceKind == savor::navigation::NavigationSurfaceSourceKind::Grnd ? 1U : 0U;
        gobjCount += surface.sourceKind == savor::navigation::NavigationSurfaceSourceKind::Gobj ? 1U : 0U;
        vertexCount += surface.mesh.vertices.size();
        triangleCount += surface.mesh.indices.size() / 3U;
    }

    for (const auto& region : model.regions) {
        if (!region.meshes.empty()) {
            ++wallRegionCount;
            for (const auto& regionMesh : region.meshes) {
                auto geometry = createTriangleGeometry(meshVertices(regionMesh.mesh), regionMesh.mesh.indices);
                QVariantMap item{};
                item.insert("geometry", QVariant::fromValue(static_cast<QObject*>(geometry.get())));
                item.insert("color", QColor(QStringLiteral("#4EA7C8")));
                item.insert("label", QStringLiteral("entry=%1 fxn=%2 tbl=%3 object=0x%4 block=0x%5 node=0x%6 attach=0x%7")
                    .arg(region.sourceEntryId)
                    .arg(QString::fromStdString(region.fxnName))
                    .arg(region.tblId)
                    .arg(regionMesh.sourceObjectAddress, 0, 16)
                    .arg(regionMesh.sourceChunkOffset, 0, 16)
                    .arg(regionMesh.sourceNodeOffset, 0, 16)
                    .arg(regionMesh.sourceAttachOffset, 0, 16));
                out.collisions.push_back(item);
                out.geometries.push_back(std::move(geometry));
                ++wallMeshCount;
                wallVertexCount += regionMesh.mesh.vertices.size();
                wallTriangleCount += regionMesh.mesh.indices.size() / 3U;
            }
            continue;
        }
        // Exact wall entries are collision boundaries. A failed projection remains visible in
        // diagnostics, but a cube would misrepresent its extent and obstruct calibration.
        if (region.kind == savor::navigation::NavigationRegionKind::Collision && isExactWall(region.fxnName)) {
            ++wallRegionCount;
            continue;
        }

        const QVector3D center(region.transform.position.x,
            region.transform.position.y,
            region.transform.position.z);
        const float halfSize = region.kind == savor::navigation::NavigationRegionKind::Collision
            ? kMarkerSize * 0.6F
            : kMarkerSize * 0.5F;
        auto geometry = createTriangleGeometry(cubeVertices(center, halfSize), cubeIndices());

        QVariantMap item{};
        item.insert("geometry", QVariant::fromValue(static_cast<QObject*>(geometry.get())));
        item.insert("label", QStringLiteral("entry=%1 fxn=%2 tbl=%3")
            .arg(region.sourceEntryId)
            .arg(QString::fromStdString(region.fxnName))
            .arg(region.tblId));

        switch (region.kind) {
        case savor::navigation::NavigationRegionKind::Collision:
            item.insert("color", QColor(QStringLiteral("#4EA7C8")));
            out.collisions.push_back(item);
            break;
        case savor::navigation::NavigationRegionKind::Trigger:
            item.insert("color", QColor(QStringLiteral("#E06666")));
            out.triggers.push_back(item);
            break;
        case savor::navigation::NavigationRegionKind::Unknown:
        default:
            item.insert("color", QColor(QStringLiteral("#CC00FF")));
            out.unknowns.push_back(item);
            break;
        }
        out.geometries.push_back(std::move(geometry));
    }

    if (model.bounds.valid) {
        const QVector3D minimum(model.bounds.minimum.x, model.bounds.minimum.y, model.bounds.minimum.z);
        const QVector3D maximum(model.bounds.maximum.x, model.bounds.maximum.y, model.bounds.maximum.z);
        out.center = (minimum + maximum) * 0.5F;
        out.extent = std::max({
            std::abs(maximum.x() - minimum.x()),
            std::abs(maximum.y() - minimum.y()),
            std::abs(maximum.z() - minimum.z()),
            120.0F,
        });
    }

    for (const auto& diagnostic : model.diagnostics) {
        out.diagnostics.push_back('[' + severityLabel(diagnostic.severity) + "] " + diagnostic.message);
    }
    std::ostringstream summary{};
    summary << "Runtime scene summary: GRND=" << grndCount
            << ", ground-role GOBJ=" << gobjCount
            << ", vertices=" << vertexCount
            << ", triangles=" << triangleCount
            << ", wallRegions=" << wallRegionCount
            << ", wallMeshes=" << wallMeshCount
            << ", wallVertices=" << wallVertexCount
            << ", wallTriangles=" << wallTriangleCount
            << ", failedWallRegions=" << model.failedWallRegionCount
            << ", provisionalLinks=" << model.groundLinks.size()
            << ", groundGeometryComplete=" << (model.hasCompleteGroundGeometry ? "yes" : "no")
            << ", wallGeometryComplete=" << (model.hasCompleteWallGeometry ? "yes" : "no") << '.';
    out.diagnostics.push_back(summary.str());
    return out;
}

} // namespace savor::qt3d::gui
