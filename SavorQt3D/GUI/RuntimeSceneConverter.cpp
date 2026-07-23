#include "RuntimeSceneConverter.h"

#include <QColor>
#include <QQmlEngine>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <utility>

namespace savor::qt3d::gui {
namespace {

constexpr float kMarkerSize = 15.0F;
constexpr float kEndpointMarkerHalfSize = kMarkerSize * 0.5F;
constexpr float kFacingRayLength = kMarkerSize * 2.0F;
constexpr float kFacingRayHalfWidth = kMarkerSize * 0.08F;
constexpr float kVectorEpsilon = 1.0e-5F;

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

[[nodiscard]] std::unique_ptr<StaticMeshGeometry> createLineGeometry(
    const std::vector<PackedVertex>& vertices,
    const std::vector<std::uint32_t>& indices) {
    auto geometry = std::make_unique<StaticMeshGeometry>();
    QQmlEngine::setObjectOwnership(geometry.get(), QQmlEngine::CppOwnership);
    geometry->setLineMesh(vertices, indices);
    return geometry;
}

[[nodiscard]] QVector3D toVector(const savor::navigation::NavigationVec3& value) {
    return QVector3D(value.x, value.y, value.z);
}

void appendBoxSegment(std::vector<PackedVertex>& vertices,
    std::vector<std::uint32_t>& indices,
    QVector3D first,
    QVector3D second,
    const float halfWidth,
    QVector3D up,
    const bool liftAboveSurface = true) {
    const QVector3D delta = second - first;
    if (delta.lengthSquared() <= kVectorEpsilon * kVectorEpsilon) {
        return;
    }

    const QVector3D direction = delta.normalized();
    if (up.lengthSquared() <= kVectorEpsilon * kVectorEpsilon) {
        up = QVector3D(0.0F, 1.0F, 0.0F);
    } else {
        up.normalize();
    }
    QVector3D side = QVector3D::crossProduct(direction, up);
    if (side.lengthSquared() <= kVectorEpsilon * kVectorEpsilon) {
        const QVector3D alternate = std::abs(direction.x()) < 0.9F
            ? QVector3D(1.0F, 0.0F, 0.0F)
            : QVector3D(0.0F, 0.0F, 1.0F);
        side = QVector3D::crossProduct(direction, alternate);
    }
    side.normalize();
    QVector3D thicknessAxis = QVector3D::crossProduct(side, direction).normalized();

    // Lift the route slightly above the walk surface so it remains visible without
    // altering the SAVOR-owned path coordinates.
    if (liftAboveSurface) {
        const QVector3D lift = up * (halfWidth * 1.25F);
        first += lift;
        second += lift;
    }
    const QVector3D sideOffset = side * halfWidth;
    const QVector3D thicknessOffset = thicknessAxis * halfWidth;
    const std::array<QVector3D, 8> corners{
        first - sideOffset - thicknessOffset,
        first + sideOffset - thicknessOffset,
        first + sideOffset + thicknessOffset,
        first - sideOffset + thicknessOffset,
        second - sideOffset - thicknessOffset,
        second + sideOffset - thicknessOffset,
        second + sideOffset + thicknessOffset,
        second - sideOffset + thicknessOffset,
    };

    const auto base = static_cast<std::uint32_t>(vertices.size());
    for (const QVector3D& corner : corners) {
        vertices.push_back(makeVertex(corner.x(), corner.y(), corner.z()));
    }
    for (const std::uint32_t index : cubeIndices()) {
        indices.push_back(base + index);
    }
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

[[nodiscard]] QString handoffKindLabel(
    const savor::navigation::NavigationCollisionHandoffKind kind) {
    switch (kind) {
    case savor::navigation::NavigationCollisionHandoffKind::AuthoredFallback:
        return QStringLiteral("authored-fallback");
    case savor::navigation::NavigationCollisionHandoffKind::SameEntryBundle:
    default:
        return QStringLiteral("same-entry");
    }
}

[[nodiscard]] QString handoffAvailabilityLabel(
    const savor::navigation::NavigationCollisionHandoffAvailability availability) {
    switch (availability) {
    case savor::navigation::NavigationCollisionHandoffAvailability::RequiresRuntimeState:
        return QStringLiteral("runtime-dependent");
    case savor::navigation::NavigationCollisionHandoffAvailability::ActiveStatic:
    default:
        return QStringLiteral("active-static");
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
    std::size_t triggerRegionCount = 0;
    std::size_t triggerMeshCount = 0;
    std::size_t triggerVertexCount = 0;
    std::size_t triggerTriangleCount = 0;
    std::size_t movingObjectRegionCount = 0;
    std::size_t movingObjectMeshCount = 0;
    std::size_t movingObjectVertexCount = 0;
    std::size_t movingObjectTriangleCount = 0;
    std::size_t runtimeDependentSurfaceCount = 0;
    std::size_t authoredFallbackTargetCount = 0;
    std::size_t resolvedFallbackTargetCount = 0;
    std::size_t missingEntryFallbackTargetCount = 0;
    std::size_t missingGeometryFallbackTargetCount = 0;
    std::size_t suppressedFallbackTargetCount = 0;

    for (std::size_t surfaceIndex = 0; surfaceIndex < model.surfaces.size(); ++surfaceIndex) {
        const auto& surface = model.surfaces[surfaceIndex];
        auto geometry = createTriangleGeometry(meshVertices(surface.mesh), surface.mesh.indices);
        QVariantMap item{};
        item.insert("geometry", QVariant::fromValue(static_cast<QObject*>(geometry.get())));
        item.insert("color", surface.sourceKind == savor::navigation::NavigationSurfaceSourceKind::Grnd
            ? QColor(QStringLiteral("#FFD84D"))
            : QColor(QStringLiteral("#FFC04D")));
        item.insert("label", surfaceLabel(surface));
        item.insert("surfaceIndex", static_cast<qulonglong>(surfaceIndex));
        out.grounds.push_back(item);
        out.geometries.push_back(std::move(geometry));

        grndCount += surface.sourceKind == savor::navigation::NavigationSurfaceSourceKind::Grnd ? 1U : 0U;
        gobjCount += surface.sourceKind == savor::navigation::NavigationSurfaceSourceKind::Gobj ? 1U : 0U;
        vertexCount += surface.mesh.vertices.size();
        triangleCount += surface.mesh.indices.size() / 3U;
        runtimeDependentSurfaceCount +=
            surface.traversalAvailability ==
                savor::navigation::NavigationSurfaceTraversalAvailability::RequiresRuntimeState
            ? 1U
            : 0U;
    }

    for (const auto& chain : model.authoredGroundFallbackChains) {
        authoredFallbackTargetCount += chain.targets.size();
        for (const auto& target : chain.targets) {
            switch (target.status) {
            case savor::navigation::NavigationAuthoredGroundFallbackTargetStatus::Resolved:
                ++resolvedFallbackTargetCount;
                break;
            case savor::navigation::NavigationAuthoredGroundFallbackTargetStatus::MissingEntry:
                ++missingEntryFallbackTargetCount;
                break;
            case savor::navigation::NavigationAuthoredGroundFallbackTargetStatus::MissingGeometry:
                ++missingGeometryFallbackTargetCount;
                break;
            case savor::navigation::NavigationAuthoredGroundFallbackTargetStatus::SuppressedAfterMissingEntry:
                ++suppressedFallbackTargetCount;
                break;
            }
        }
    }

    for (std::size_t regionIndex = 0; regionIndex < model.regions.size(); ++regionIndex) {
        const auto& region = model.regions[regionIndex];
        const bool exactWall = region.kind == savor::navigation::NavigationRegionKind::Collision &&
            isExactWall(region.fxnName);
        wallRegionCount += exactWall ? 1U : 0U;
        triggerRegionCount += region.kind == savor::navigation::NavigationRegionKind::Trigger ? 1U : 0U;
        movingObjectRegionCount += region.kind == savor::navigation::NavigationRegionKind::MovingObject ? 1U : 0U;

        if (!region.meshes.empty()) {
            for (std::size_t regionMeshIndex = 0; regionMeshIndex < region.meshes.size(); ++regionMeshIndex) {
                const auto& regionMesh = region.meshes[regionMeshIndex];
                auto geometry = createTriangleGeometry(meshVertices(regionMesh.mesh), regionMesh.mesh.indices);
                QVariantMap item{};
                item.insert("geometry", QVariant::fromValue(static_cast<QObject*>(geometry.get())));
                item.insert("label", QStringLiteral("entry=%1 fxn=%2 tbl=%3 object=0x%4 block=0x%5 node=0x%6 attach=0x%7")
                    .arg(region.sourceEntryId)
                    .arg(QString::fromStdString(region.fxnName))
                    .arg(region.tblId)
                    .arg(regionMesh.sourceObjectAddress, 0, 16)
                    .arg(regionMesh.sourceChunkOffset, 0, 16)
                    .arg(regionMesh.sourceNodeOffset, 0, 16)
                    .arg(regionMesh.sourceAttachOffset, 0, 16));

                switch (region.kind) {
                case savor::navigation::NavigationRegionKind::Collision:
                    item.insert("color", QColor(QStringLiteral("#4EA7C8")));
                    out.collisions.push_back(item);
                    ++wallMeshCount;
                    wallVertexCount += regionMesh.mesh.vertices.size();
                    wallTriangleCount += regionMesh.mesh.indices.size() / 3U;
                    break;
                case savor::navigation::NavigationRegionKind::Trigger:
                    item.insert("color", QColor(QStringLiteral("#E06666")));
                    item.insert("regionIndex", static_cast<qulonglong>(regionIndex));
                    item.insert("regionMeshIndex", static_cast<qulonglong>(regionMeshIndex));
                    item.insert("goalPickable", true);
                    out.triggers.push_back(item);
                    ++triggerMeshCount;
                    triggerVertexCount += regionMesh.mesh.vertices.size();
                    triggerTriangleCount += regionMesh.mesh.indices.size() / 3U;
                    break;
                case savor::navigation::NavigationRegionKind::MovingObject:
                    item.insert("color", QColor(QStringLiteral("#F6B26B")));
                    out.movingObjects.push_back(item);
                    ++movingObjectMeshCount;
                    movingObjectVertexCount += regionMesh.mesh.vertices.size();
                    movingObjectTriangleCount += regionMesh.mesh.indices.size() / 3U;
                    break;
                case savor::navigation::NavigationRegionKind::Unknown:
                default:
                    item.insert("color", QColor(QStringLiteral("#CC00FF")));
                    out.unknowns.push_back(item);
                    break;
                }
                out.geometries.push_back(std::move(geometry));
            }
            continue;
        }
        // Exact wall entries are collision boundaries. A failed projection remains visible in
        // diagnostics, but a cube would misrepresent its extent and obstruct calibration.
        if (exactWall) {
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
            item.insert("goalPickable", false);
            out.triggers.push_back(item);
            break;
        case savor::navigation::NavigationRegionKind::MovingObject:
            item.insert("color", QColor(QStringLiteral("#F6B26B")));
            out.movingObjects.push_back(item);
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
            << ", triggerRegions=" << triggerRegionCount
            << ", triggerMeshes=" << triggerMeshCount
            << ", triggerVertices=" << triggerVertexCount
            << ", triggerTriangles=" << triggerTriangleCount
            << ", failedTriggerRegions=" << model.failedTriggerRegionCount
            << ", movingObjectRegions=" << movingObjectRegionCount
            << ", movingObjectMeshes=" << movingObjectMeshCount
            << ", movingObjectVertices=" << movingObjectVertexCount
            << ", movingObjectTriangles=" << movingObjectTriangleCount
            << ", failedMovingObjectRegions=" << model.failedMovingObjectRegionCount
            << ", runtimeDependentSurfaces=" << runtimeDependentSurfaceCount
            << ", authoredFallbackChains=" << model.authoredGroundFallbackChains.size()
            << ", authoredFallbackTargets=" << authoredFallbackTargetCount
            << ", resolvedFallbackTargets=" << resolvedFallbackTargetCount
            << ", missingEntryFallbackTargets=" << missingEntryFallbackTargetCount
            << ", missingGeometryFallbackTargets=" << missingGeometryFallbackTargetCount
            << ", suppressedFallbackTargets=" << suppressedFallbackTargetCount
            << ", groundGeometryComplete=" << (model.hasCompleteGroundGeometry ? "yes" : "no")
            << ", wallGeometryComplete=" << (model.hasCompleteWallGeometry ? "yes" : "no")
            << ", triggerGeometryComplete=" << (model.hasCompleteTriggerGeometry ? "yes" : "no")
            << ", movingObjectGeometryComplete=" << (model.hasCompleteMovingObjectGeometry ? "yes" : "no") << '.';
    out.diagnostics.push_back(summary.str());
    return out;
}

RuntimeSceneData RuntimeSceneConverter::convert(
    const savor::navigation::NavigationScenarioModel& model) const {
    RuntimeSceneData out = convert(model.area);

    std::size_t directedEdgeCount = 0;
    for (const auto& node : model.traversalGraph.nodes) {
        directedEdgeCount += node.edges.size();
    }

    std::size_t renderedActiveHandoffCount = 0;
    std::size_t renderedConditionalHandoffCount = 0;
    for (const auto& handoff : model.traversalGraph.collisionHandoffs) {
        const std::vector<PackedVertex> vertices{
            makeVertex(handoff.portal.first.x, handoff.portal.first.y, handoff.portal.first.z),
            makeVertex(handoff.portal.second.x, handoff.portal.second.y, handoff.portal.second.z),
        };
        auto geometry = createLineGeometry(vertices, { 0U, 1U });
        QVariantMap item{};
        item.insert("geometry", QVariant::fromValue(static_cast<QObject*>(geometry.get())));
        const bool runtimeDependent =
            handoff.availability ==
            savor::navigation::NavigationCollisionHandoffAvailability::RequiresRuntimeState;
        item.insert("color", runtimeDependent
            ? QColor(QStringLiteral("#C58C55"))
            : QColor(QStringLiteral("#FFD966")));
        item.insert("label",
            QStringLiteral("Handoff entry=%1 surface=%2 triangle=%3 -> entry=%4 surface=%5 triangle=%6; kind=%7; ordinal=%8; availability=%9")
                .arg(handoff.sourceEntryId)
                .arg(static_cast<qulonglong>(handoff.sourceTriangle.surfaceIndex))
                .arg(static_cast<qulonglong>(handoff.sourceTriangle.triangleIndex))
                .arg(handoff.targetEntryId)
                .arg(static_cast<qulonglong>(handoff.targetTriangle.surfaceIndex))
                .arg(static_cast<qulonglong>(handoff.targetTriangle.triangleIndex))
                .arg(handoffKindLabel(handoff.kind))
                .arg(handoff.authoredOrdinal.has_value()
                    ? QString::number(static_cast<qulonglong>(*handoff.authoredOrdinal))
                    : QStringLiteral("n/a"))
                .arg(handoffAvailabilityLabel(handoff.availability)));
        out.links.push_back(item);
        out.geometries.push_back(std::move(geometry));
        if (runtimeDependent) {
            ++renderedConditionalHandoffCount;
        } else {
            ++renderedActiveHandoffCount;
        }
    }

    for (const auto& diagnostic : model.traversalGraph.diagnostics) {
        out.diagnostics.push_back('[' + severityLabel(diagnostic.severity) + "] " + diagnostic.message);
    }

    const auto& statistics = model.traversalGraph.statistics;
    std::ostringstream summary{};
    summary << "Traversal graph summary: nodes=" << model.traversalGraph.nodes.size()
            << ", directedEdges=" << directedEdgeCount
            << ", intraSurfaceConnections=" << statistics.intraSurfaceConnectionCount
            << ", authoredFallbackChains=" << statistics.authoredFallbackChainCount
            << ", authoredFallbackTargets=" << statistics.authoredFallbackTargetCount
            << ", sameEntryHandoffs=" << statistics.sameEntryHandoffCount
            << ", authoredFallbackHandoffs=" << statistics.authoredFallbackHandoffCount
            << ", conditionalHandoffs=" << statistics.conditionalHandoffCount
            << ", renderedActiveHandoffs=" << renderedActiveHandoffCount
            << ", renderedConditionalHandoffs=" << renderedConditionalHandoffCount
            << ", unresolvedHandoffs=" << statistics.unresolvedHandoffCount
            << ", runtimeDependentSurfaces=" << statistics.runtimeDependentSurfaceCount
            << ", runtimeStateBlockedIntervals=" << statistics.runtimeStateBlockedIntervalCount
            << ", priorityShadowedCandidates=" << statistics.priorityShadowedCandidateCount
            << ", skippedInvalidTriangles=" << statistics.skippedInvalidTriangleCount
            << ", skippedDegenerateTriangles=" << statistics.skippedDegenerateTriangleCount
            << ", skippedDuplicateTriangles=" << statistics.skippedDuplicateTriangleCount
            << ", skippedNonManifoldTriangles=" << statistics.skippedNonManifoldTriangleCount
            << ", nonManifoldEdges=" << statistics.nonManifoldEdgeCount
            << ", pathfindingReady=" << (model.isPathfindingReady() ? "yes" : "no") << '.';
    out.diagnostics.push_back(summary.str());
    return out;
}

RuntimeRouteData RuntimeSceneConverter::convertRoute(
    const std::optional<savor::navigation::NavigationGraphAnchor>& start,
    const std::optional<float>& startFacingYawDegrees,
    const std::optional<savor::navigation::NavigationGraphAnchor>& goal,
    const std::optional<savor::navigation::NavigationTriggerGoalTarget>& triggerGoal,
    const std::optional<savor::navigation::NavigationPathResult>& route,
    const float sceneExtent,
    const savor::navigation::NavigationCoordinatePolicy coordinatePolicy) const {
    RuntimeRouteData out{};
    constexpr float markerHalfSize = kEndpointMarkerHalfSize;
    const float routeHalfWidth = std::max(sceneExtent * 0.0025F, 0.35F);
    QVector3D up = toVector(coordinatePolicy.upAxis());
    if (up.lengthSquared() <= kVectorEpsilon * kVectorEpsilon) {
        up = QVector3D(0.0F, 1.0F, 0.0F);
    } else {
        up.normalize();
    }

    const auto appendMarker = [&](const savor::navigation::NavigationGraphAnchor& anchor,
                                  const QColor& color,
                                  const QString& label) {
        const QVector3D center = toVector(anchor.snappedPoint) + (up * markerHalfSize);
        auto geometry = createTriangleGeometry(cubeVertices(center, markerHalfSize), cubeIndices());
        QVariantMap item{};
        item.insert("geometry", QVariant::fromValue(static_cast<QObject*>(geometry.get())));
        item.insert("color", color);
        item.insert("label", label);
        out.routes.push_back(item);
        out.geometries.push_back(std::move(geometry));
    };

    if (start.has_value()) {
        appendMarker(*start, QColor(QStringLiteral("#45D06F")), QStringLiteral("Start"));
        if (startFacingYawDegrees.has_value()) {
            QVector3D facing = toVector(
                coordinatePolicy.sceneFacingDirectionFromConvertedYaw(*startFacingYawDegrees));
            if (facing.lengthSquared() > kVectorEpsilon * kVectorEpsilon) {
                facing.normalize();
                const QVector3D center = toVector(start->snappedPoint) + (up * markerHalfSize);
                std::vector<PackedVertex> vertices{};
                std::vector<std::uint32_t> indices{};
                appendBoxSegment(vertices,
                    indices,
                    center + (facing * markerHalfSize),
                    center + (facing * (markerHalfSize + kFacingRayLength)),
                    kFacingRayHalfWidth,
                    up);
                if (!vertices.empty() && !indices.empty()) {
                    auto geometry = createTriangleGeometry(vertices, indices);
                    QVariantMap item{};
                    item.insert("geometry", QVariant::fromValue(static_cast<QObject*>(geometry.get())));
                    item.insert("color", QColor(QStringLiteral("#45D06F")));
                    item.insert("label", QStringLiteral("Start facing (%1 degrees)")
                        .arg(*startFacingYawDegrees, 0, 'g', 7));
                    out.routes.push_back(item);
                    out.geometries.push_back(std::move(geometry));
                }
            }
        }
    }
    if (goal.has_value() && !triggerGoal.has_value()) {
        appendMarker(*goal, QColor(QStringLiteral("#D957E8")), QStringLiteral("Goal"));
    }

    if (triggerGoal.has_value() && triggerGoal->displayBounds.valid) {
        const auto& bounds = triggerGoal->displayBounds;
        const QVector3D minimum = toVector(bounds.minimum);
        const QVector3D maximum = toVector(bounds.maximum);
        const std::array<QVector3D, 8> corners{
            QVector3D(minimum.x(), minimum.y(), minimum.z()),
            QVector3D(maximum.x(), minimum.y(), minimum.z()),
            QVector3D(maximum.x(), maximum.y(), minimum.z()),
            QVector3D(minimum.x(), maximum.y(), minimum.z()),
            QVector3D(minimum.x(), minimum.y(), maximum.z()),
            QVector3D(maximum.x(), minimum.y(), maximum.z()),
            QVector3D(maximum.x(), maximum.y(), maximum.z()),
            QVector3D(minimum.x(), maximum.y(), maximum.z()),
        };
        constexpr std::array<std::array<std::size_t, 2>, 12> edges{
            std::array<std::size_t, 2>{ 0U, 1U },
            std::array<std::size_t, 2>{ 1U, 2U },
            std::array<std::size_t, 2>{ 2U, 3U },
            std::array<std::size_t, 2>{ 3U, 0U },
            std::array<std::size_t, 2>{ 4U, 5U },
            std::array<std::size_t, 2>{ 5U, 6U },
            std::array<std::size_t, 2>{ 6U, 7U },
            std::array<std::size_t, 2>{ 7U, 4U },
            std::array<std::size_t, 2>{ 0U, 4U },
            std::array<std::size_t, 2>{ 1U, 5U },
            std::array<std::size_t, 2>{ 2U, 6U },
            std::array<std::size_t, 2>{ 3U, 7U },
        };
        std::vector<PackedVertex> vertices{};
        std::vector<std::uint32_t> indices{};
        const float boundsHalfWidth = std::max(sceneExtent * 0.0008F, 0.18F);
        for (const auto& edge : edges) {
            appendBoxSegment(vertices,
                indices,
                corners[edge[0]],
                corners[edge[1]],
                boundsHalfWidth,
                up,
                false);
        }
        if (!vertices.empty() && !indices.empty()) {
            auto geometry = createTriangleGeometry(vertices, indices);
            QVariantMap item{};
            item.insert("geometry", QVariant::fromValue(static_cast<QObject*>(geometry.get())));
            item.insert("color", QColor(QStringLiteral("#D957E8")));
            item.insert("label", QStringLiteral("Trigger goal region=%1 primaryMesh=%2")
                .arg(static_cast<qulonglong>(triggerGoal->regionIndex))
                .arg(static_cast<qulonglong>(triggerGoal->primaryMeshIndex)));
            out.routes.push_back(item);
            out.geometries.push_back(std::move(geometry));
        }
    }

    if (route.has_value() && route->hasPath() && route->polyline.size() >= 2U) {
        std::vector<PackedVertex> vertices{};
        std::vector<std::uint32_t> indices{};
        for (std::size_t index = 1; index < route->polyline.size(); ++index) {
            appendBoxSegment(vertices,
                indices,
                toVector(route->polyline[index - 1U]),
                toVector(route->polyline[index]),
                routeHalfWidth,
                up);
        }
        if (!vertices.empty() && !indices.empty()) {
            auto geometry = createTriangleGeometry(vertices, indices);
            QVariantMap item{};
            item.insert("geometry", QVariant::fromValue(static_cast<QObject*>(geometry.get())));
            item.insert("color", QColor(QStringLiteral("#FFF176")));
            item.insert("label", QStringLiteral("Route (%1 points)")
                .arg(static_cast<qulonglong>(route->polyline.size())));
            out.routes.push_back(item);
            out.geometries.push_back(std::move(geometry));
        }

        std::ostringstream summary{};
        summary << "Route overlay: triangles=" << route->trianglePath.size()
                << ", points=" << route->polyline.size()
                << ", length=" << route->routeLength
                << ", cost=" << route->totalCost << '.';
        out.diagnostics.push_back(summary.str());
    }
    return out;
}

} // namespace savor::qt3d::gui
