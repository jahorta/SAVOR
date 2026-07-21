#pragma once

#include "NavigationAreaLoader.h"
#include "NavigationScriptLoader.h"
#include "../Model/NavigationScenarioModel.h"

#include <filesystem>
#include <optional>
#include <vector>

namespace savor::navigation {

enum class NavigationScenarioLoadStatus {
    Complete,
    Partial,
    Failed,
};

struct NavigationScenarioLoadResult {
    NavigationScenarioLoadStatus status = NavigationScenarioLoadStatus::Failed;
    std::optional<NavigationScenarioModel> model{};
    std::vector<NavigationDiagnostic> diagnostics{};

    [[nodiscard]] bool hasModel() const noexcept {
        return model.has_value();
    }
};

class NavigationScenarioLoader final {
public:
    [[nodiscard]] NavigationScenarioLoadResult loadFile(
        const std::filesystem::path& mldPath) const;

    [[nodiscard]] NavigationScriptLoadResult loadRelatedScript(
        const NavigationAreaIdentity& identity,
        const std::filesystem::path& sctPath) const;
};

} // namespace savor::navigation
