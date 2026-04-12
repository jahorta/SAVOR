#include "RuntimeSceneConverter.h"

#include <QColor>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <unordered_map>

namespace soasim::qt3d::gui {
namespace {

constexpr float kMarkerSize = 15.0f;

PackedVertex makeVertex(const float x, const float y, const float z,
    const float nx = 0.0f, const float ny = 1.0f, const float nz = 0.0f) {
    return PackedVertex{
        .px = x,
        .py = y,
        .pz = z,
        .nx = nx,
        .ny = ny,
        .nz = nz,
    };
}

std::vector<std::uint32_t> cubeIndices() {
    return {
        0, 1, 2, 0, 2, 3,
        4, 6, 5, 4, 7, 6,
        0, 4, 5, 0, 5, 1,
        3, 2, 6, 3, 6, 7,
        1, 5, 6, 1, 6, 2,
        0, 3, 7, 0, 7, 4,
    };
}

std::vector<PackedVertex> cubeVertices(const QVector3D& center, const float halfSize) {
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

QVector3D centroidFromVertices(const std::vector<scene::SceneVertex>& vertices) {
    if (vertices.empty()) {
        return QVector3D(0.0f, 0.0f, 0.0f);
    }

    QVector3D sum(0.0f, 0.0f, 0.0f);
    for (const auto& vtx : vertices) {
        sum += QVector3D(vtx.px, vtx.py, vtx.pz);
    }
    return sum / static_cast<float>(vertices.size());
}

std::unique_ptr<StaticMeshGeometry> createTriangleGeometry(const std::vector<PackedVertex>& vertices,
    const std::vector<std::uint32_t>& indices) {
    auto geom = std::make_unique<StaticMeshGeometry>();
    geom->setTriangleMesh(vertices, indices);
    return geom;
}

std::unique_ptr<StaticMeshGeometry> createLineGeometry(const std::vector<PackedVertex>& vertices,
    const std::vector<std::uint32_t>& indices) {
    auto geom = std::make_unique<StaticMeshGeometry>();
    geom->setLineMesh(vertices, indices);
    return geom;
}

} // namespace

RuntimeSceneData RuntimeSceneConverter::convert(const soasim::mld::parsing::ParseResult& parse,
    const soasim::qt3d::scene::SceneBuildResult& scene) const {
    RuntimeSceneData out{};

    QVector3D minBound(std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    QVector3D maxBound(std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest());

    auto updateBounds = [&](const float x, const float y, const float z) {
        minBound.setX(std::min(minBound.x(), x));
        minBound.setY(std::min(minBound.y(), y));
        minBound.setZ(std::min(minBound.z(), z));
        maxBound.setX(std::max(maxBound.x(), x));
        maxBound.setY(std::max(maxBound.y(), y));
        maxBound.setZ(std::max(maxBound.z(), z));
    };

    std::unordered_map<std::uint32_t, QVector3D> grndCenters{};

    for (const auto& node : scene.grounds) {
        std::vector<PackedVertex> vertices{};
        vertices.reserve(node.mesh.vertices.size());
        for (const auto& vtx : node.mesh.vertices) {
            vertices.push_back(makeVertex(vtx.px, vtx.py, vtx.pz, vtx.nx, vtx.ny, vtx.nz));
            updateBounds(vtx.px, vtx.py, vtx.pz);
        }

        auto geom = createTriangleGeometry(vertices, node.mesh.indices);
        QVariantMap map{};
        map.insert("geometry", QVariant::fromValue(static_cast<QObject*>(geom.get())));
        map.insert("color", QColor(QStringLiteral("#6FA8DC")));
        map.insert("label", QString("GRND_%1").arg(node.grndId));
        out.grounds.push_back(map);
        out.geometries.push_back(std::move(geom));

        grndCenters[node.grndId] = centroidFromVertices(node.mesh.vertices);
    }

    std::set<std::pair<std::uint32_t, std::uint32_t>> linkDedup{};
    for (const auto& grnd : parse.world.grndSurfaces) {
        const auto srcIt = grndCenters.find(grnd.id);
        if (srcIt == grndCenters.end()) {
            continue;
        }
        for (const auto dstId : grnd.linkedGrndIds) {
            const auto dstIt = grndCenters.find(dstId);
            if (dstIt == grndCenters.end()) {
                continue;
            }
            const auto key = std::minmax(grnd.id, dstId);
            if (!linkDedup.insert(key).second) {
                continue;
            }
            const auto& a = srcIt->second;
            const auto& b = dstIt->second;

            std::vector<PackedVertex> vertices{
                makeVertex(a.x(), a.y(), a.z()),
                makeVertex(b.x(), b.y(), b.z()),
            };
            std::vector<std::uint32_t> indices{ 0U, 1U };

            auto geom = createLineGeometry(vertices, indices);
            QVariantMap map{};
            map.insert("geometry", QVariant::fromValue(static_cast<QObject*>(geom.get())));
            map.insert("color", QColor(QStringLiteral("#F1C232")));
            map.insert("label", QString("Link_%1_%2").arg(grnd.id).arg(dstId));
            out.links.push_back(map);
            out.geometries.push_back(std::move(geom));
        }
    }

    for (const auto& trigger : parse.world.triggers) {
        const QVector3D center(trigger.transform.position.x,
            trigger.transform.position.y,
            trigger.transform.position.z);
        const auto vertices = cubeVertices(center, kMarkerSize * 0.5f);
        const auto indices = cubeIndices();

        auto geom = createTriangleGeometry(vertices, indices);
        QVariantMap map{};
        map.insert("geometry", QVariant::fromValue(static_cast<QObject*>(geom.get())));
        map.insert("color", QColor(QStringLiteral("#E06666")));
        map.insert("label", QString("Trigger_%1_fxn_%2")
            .arg(trigger.sourceEntryId)
            .arg(trigger.fxn));
        out.triggers.push_back(map);
        out.geometries.push_back(std::move(geom));

        updateBounds(center.x(), center.y(), center.z());
    }

    for (const auto& unknown : parse.world.unknownEntries) {
        const QVector3D center(unknown.transform.position.x,
            unknown.transform.position.y,
            unknown.transform.position.z);
        const auto vertices = cubeVertices(center, kMarkerSize * 0.5f);
        const auto indices = cubeIndices();

        auto geom = createTriangleGeometry(vertices, indices);
        QVariantMap map{};
        map.insert("geometry", QVariant::fromValue(static_cast<QObject*>(geom.get())));
        map.insert("color", QColor(QStringLiteral("#CC00FF")));
        map.insert("label", QString("Unknown_%1_fxn_%2")
            .arg(unknown.sourceEntryId)
            .arg(unknown.fxn));
        out.unknowns.push_back(map);
        out.geometries.push_back(std::move(geom));

        updateBounds(center.x(), center.y(), center.z());
    }

    if (minBound.x() > maxBound.x()) {
        minBound = QVector3D(-100.0f, -100.0f, -100.0f);
        maxBound = QVector3D(100.0f, 100.0f, 100.0f);
    }
    out.center = (minBound + maxBound) * 0.5f;
    out.extent = std::max({
        std::abs(maxBound.x() - minBound.x()),
        std::abs(maxBound.y() - minBound.y()),
        std::abs(maxBound.z() - minBound.z()),
        120.0f,
    });

    out.diagnostics.push_back("Parse diagnostics:");
    for (const auto& diag : parse.diagnostics) {
        out.diagnostics.push_back(diag.message);
    }
    out.diagnostics.push_back("Scene summary: grounds=" + std::to_string(scene.grounds.size()) +
        ", links=" + std::to_string(out.links.size()) +
        ", triggers=" + std::to_string(parse.world.triggers.size()) +
        ", unknown=" + std::to_string(parse.world.unknownEntries.size()));

    return out;
}

} // namespace soasim::qt3d::gui
