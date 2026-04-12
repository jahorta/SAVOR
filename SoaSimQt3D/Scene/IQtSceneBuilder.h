#pragma once

#include "QtSceneData.h"

#include "../../SoaSimMLD/Model/GeometryModel.h"

namespace soasim::qt3d::scene {

class IQtSceneBuilder {
public:
    virtual ~IQtSceneBuilder() = default;

    [[nodiscard]] virtual SceneBuildResult buildScene(const soasim::mld::model::GeometryBuildResult& geometry) const = 0;
};

} // namespace soasim::qt3d::scene
