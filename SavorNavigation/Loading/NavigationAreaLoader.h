#pragma once

#include "../Model/NavigationAreaModel.h"

#include <filesystem>
#include <optional>
#include <vector>

namespace savor::navigation {

enum class NavigationAreaLoadStatus {
    Complete,
    Partial,
    Failed,
};

struct NavigationAreaLoadResult {
    NavigationAreaLoadStatus status = NavigationAreaLoadStatus::Failed;
    std::optional<NavigationAreaModel> model{};
    std::vector<NavigationDiagnostic> diagnostics{};

    [[nodiscard]] bool hasModel() const noexcept {
        return model.has_value();
    }
};

class NavigationAreaLoader final {
public:
    [[nodiscard]] NavigationAreaLoadResult loadFile(const std::filesystem::path& path) const;
};

} // namespace savor::navigation
