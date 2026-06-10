#pragma once

#include "StaticMeshGeometry.h"
#include "../Scene/QtSceneData.h"

#include "../../SavorMLD/Parsing/MldParser.h"

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
    QVector3D center{ 0.0f, 0.0f, 0.0f };
    float extent = 200.0f;
    std::vector<std::string> diagnostics{};
    std::vector<std::unique_ptr<StaticMeshGeometry>> geometries{};
};

class RuntimeSceneConverter final {
public:
    [[nodiscard]] RuntimeSceneData convert(const savor::mld::parsing::ParseResult& parse,
        const savor::qt3d::scene::SceneBuildResult& scene) const;
};

} // namespace savor::qt3d::gui
