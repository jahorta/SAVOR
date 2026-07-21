#pragma once

#include "NavigationAreaIdentity.h"
#include "NavigationAreaModel.h"
#include "NavigationScriptModel.h"
#include "../Graph/NavigationTraversalGraph.h"

#include <vector>

namespace savor::navigation {

struct NavigationScenarioModel {
    NavigationAreaIdentity identity{};
    NavigationAreaModel area{};
    NavigationScriptModel script{};
    NavigationTraversalGraph traversalGraph{};
    std::vector<NavigationDiagnostic> diagnostics{};

    [[nodiscard]] bool isPathfindingReady() const noexcept {
        return traversalGraph.isPathfindingReady();
    }
};

} // namespace savor::navigation
