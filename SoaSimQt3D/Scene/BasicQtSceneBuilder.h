#pragma once

#include "IQtSceneBuilder.h"

namespace soasim::qt3d::scene {

class BasicQtSceneBuilder final : public IQtSceneBuilder {
public:
    [[nodiscard]] SceneBuildResult buildScene(const soasim::mld::model::GeometryBuildResult& geometry) const override;
};

} // namespace soasim::qt3d::scene
