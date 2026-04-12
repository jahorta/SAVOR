#pragma once

#include <QtQuick3D/qquick3dgeometry.h>

#include <QVector3D>

#include <cstdint>
#include <vector>

namespace soasim::qt3d::gui {

struct PackedVertex {
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 0.0f;
};

class StaticMeshGeometry final : public QQuick3DGeometry {
public:
    explicit StaticMeshGeometry(QQuick3DObject* parent = nullptr);

    void setTriangleMesh(const std::vector<PackedVertex>& vertices,
        const std::vector<std::uint32_t>& indices);

    void setLineMesh(const std::vector<PackedVertex>& vertices,
        const std::vector<std::uint32_t>& indices);

private:
    void applyMesh(PrimitiveType primitive,
        const std::vector<PackedVertex>& vertices,
        const std::vector<std::uint32_t>& indices);
};

} // namespace soasim::qt3d::gui
