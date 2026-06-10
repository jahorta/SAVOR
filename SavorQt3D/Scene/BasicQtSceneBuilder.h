#pragma once

#include "IQtSceneBuilder.h"

namespace savor::qt3d::scene {

class BasicQtSceneBuilder final : public IQtSceneBuilder {
public:
    [[nodiscard]] SceneBuildResult buildScene(const savor::mld::model::GeometryBuildResult& geometry) const override;
};

} // namespace savor::qt3d::scene
