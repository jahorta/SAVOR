#pragma once

#include "StaticMeshGeometry.h"

#include "../../SavorNavigation/Model/NavigationScenarioModel.h"
#include "../../SavorNavigation/Model/NavigationAreaModel.h"
#include "../../SavorNavigation/Pathfinding/NavigationPathfinder.h"

#include <QVector3D>
#include <QVariant>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace savor::qt3d::gui {

struct RuntimeSceneData {
    QVariantList grounds{};
    QVariantList links{};
    QVariantList collisions{};
    QVariantList triggers{};
    QVariantList movingObjects{};
    QVariantList unknowns{};
    QVector3D center{ 0.0F, 0.0F, 0.0F };
    float extent = 200.0F;
    std::vector<std::string> diagnostics{};
    std::vector<std::unique_ptr<StaticMeshGeometry>> geometries{};
};

struct RuntimeRouteData {
    QVariantList routes{};
    std::vector<std::string> diagnostics{};
    std::vector<std::unique_ptr<StaticMeshGeometry>> geometries{};
};

class RuntimeSceneConverter final {
public:
    [[nodiscard]] RuntimeSceneData convert(const savor::navigation::NavigationAreaModel& model) const;
    [[nodiscard]] RuntimeSceneData convert(const savor::navigation::NavigationScenarioModel& model) const;
    [[nodiscard]] RuntimeRouteData convertRoute(
        const std::optional<savor::navigation::NavigationGraphAnchor>& start,
        const std::optional<float>& startFacingYawDegrees,
        const std::optional<savor::navigation::NavigationGraphAnchor>& goal,
        const std::optional<savor::navigation::NavigationTriggerGoalTarget>& triggerGoal,
        const std::optional<savor::navigation::NavigationPathResult>& route,
        float sceneExtent,
        savor::navigation::NavigationCoordinatePolicy coordinatePolicy =
            savor::navigation::NavigationCoordinatePolicy::identity()) const;
};

} // namespace savor::qt3d::gui
