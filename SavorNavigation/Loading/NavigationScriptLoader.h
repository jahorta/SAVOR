#pragma once

#include "../Model/NavigationAreaIdentity.h"
#include "../Model/NavigationScriptModel.h"

#include <filesystem>
#include <vector>

namespace savor::navigation {

struct NavigationScriptLoadResult {
    NavigationScriptModel model{};
    std::vector<NavigationDiagnostic> diagnostics{};

    [[nodiscard]] bool hasDocument() const noexcept {
        return model.hasDocument();
    }
};

class NavigationScriptLoader final {
public:
    [[nodiscard]] NavigationScriptLoadResult discoverAndLoad(
        const NavigationAreaIdentity& identity) const;

    [[nodiscard]] NavigationScriptLoadResult loadMatchingFile(
        const NavigationAreaIdentity& identity,
        const std::filesystem::path& sctPath) const;
};

} // namespace savor::navigation
