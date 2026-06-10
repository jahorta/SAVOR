#include "StaticMeshGeometry.h"

#include <QByteArray>
#include <QtQuick3D/qquick3dgeometry.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace savor::qt3d::gui {

StaticMeshGeometry::StaticMeshGeometry(QQuick3DObject* parent)
    : QQuick3DGeometry(parent) {
}

void StaticMeshGeometry::setTriangleMesh(const std::vector<PackedVertex>& vertices,
    const std::vector<std::uint32_t>& indices) {
    applyMesh(QQuick3DGeometry::PrimitiveType::Triangles, vertices, indices);
}

void StaticMeshGeometry::setLineMesh(const std::vector<PackedVertex>& vertices,
    const std::vector<std::uint32_t>& indices) {
    applyMesh(QQuick3DGeometry::PrimitiveType::Lines, vertices, indices);
}

void StaticMeshGeometry::applyMesh(const PrimitiveType primitive,
    const std::vector<PackedVertex>& vertices,
    const std::vector<std::uint32_t>& indices) {
    clear();
    setPrimitiveType(primitive);

    if (vertices.empty() || indices.empty()) {
        update();
        return;
    }

    QByteArray vertexData;
    vertexData.resize(static_cast<int>(vertices.size() * sizeof(PackedVertex)));
    std::memcpy(vertexData.data(), vertices.data(), vertices.size() * sizeof(PackedVertex));

    QByteArray indexData;
    indexData.resize(static_cast<int>(indices.size() * sizeof(std::uint32_t)));
    std::memcpy(indexData.data(), indices.data(), indices.size() * sizeof(std::uint32_t));

    setStride(static_cast<int>(sizeof(PackedVertex)));
    setVertexData(vertexData);
    setIndexData(indexData);

    addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
    addAttribute(Attribute::NormalSemantic, 3 * static_cast<int>(sizeof(float)), Attribute::F32Type);
    addAttribute(Attribute::IndexSemantic, 0, Attribute::U32Type);

    QVector3D minBound(std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    QVector3D maxBound(std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest());
    for (const auto& vtx : vertices) {
        minBound.setX(std::min(minBound.x(), vtx.px));
        minBound.setY(std::min(minBound.y(), vtx.py));
        minBound.setZ(std::min(minBound.z(), vtx.pz));

        maxBound.setX(std::max(maxBound.x(), vtx.px));
        maxBound.setY(std::max(maxBound.y(), vtx.py));
        maxBound.setZ(std::max(maxBound.z(), vtx.pz));
    }
    setBounds(minBound, maxBound);
    update();
}

} // namespace savor::qt3d::gui
