#pragma once

#include "StaticMeshGeometry.h"
#include "../Scene/QtSceneData.h"

#include "../../SoaSimMLD/Parsing/MldParser.h"

#include <QVector3D>
#include <QVariant>

#include <memory>
#include <string>
#include <vector>

namespace soasim::qt3d::gui {

struct RuntimeSceneData {
    QVariantList grounds{};
    QVariantList links{};
    QVariantList triggers{};
    QVariantList unknowns{};
    QVector3D center{ 0.0f, 0.0f, 0.0f };
    float extent = 200.0f;
    std::vector<std::string> diagnostics{};
    std::vector<std::unique_ptr<StaticMeshGeometry>> geometries{};
};

class RuntimeSceneConverter final {
public:
    [[nodiscard]] RuntimeSceneData convert(const soasim::mld::parsing::ParseResult& parse,
        const soasim::qt3d::scene::SceneBuildResult& scene) const;
};

} // namespace soasim::qt3d::gui
