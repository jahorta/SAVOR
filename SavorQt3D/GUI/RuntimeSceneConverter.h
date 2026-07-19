#pragma once

#include "StaticMeshGeometry.h"

#include "../../SavorNavigation/Model/NavigationAreaModel.h"

#include <QVector3D>
#include <QVariant>

#include <memory>
#include <string>
#include <vector>

namespace savor::qt3d::gui {

struct RuntimeSceneData {
    QVariantList grounds{};
    QVariantList links{};
    QVariantList collisions{};
    QVariantList triggers{};
    QVariantList unknowns{};
    QVector3D center{ 0.0F, 0.0F, 0.0F };
    float extent = 200.0F;
    std::vector<std::string> diagnostics{};
    std::vector<std::unique_ptr<StaticMeshGeometry>> geometries{};
};

class RuntimeSceneConverter final {
public:
    [[nodiscard]] RuntimeSceneData convert(const savor::navigation::NavigationAreaModel& model) const;
};

} // namespace savor::qt3d::gui
