#pragma once

#include "QtSceneData.h"

#include "../../SavorMLD/Model/GeometryModel.h"

namespace savor::qt3d::scene {

class IQtSceneBuilder {
public:
    virtual ~IQtSceneBuilder() = default;

    [[nodiscard]] virtual SceneBuildResult buildScene(const savor::mld::model::GeometryBuildResult& geometry) const = 0;
};

} // namespace savor::qt3d::scene
