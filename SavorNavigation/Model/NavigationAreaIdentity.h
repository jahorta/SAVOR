#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace savor::navigation {

struct NavigationAreaIdentity {
    std::filesystem::path mldPath{};
    std::string areaKey{};
    std::string expectedSctFileName{};
    bool recognized = false;

    [[nodiscard]] std::filesystem::path expectedSctPath() const;
    [[nodiscard]] bool matchesSctPath(const std::filesystem::path& path) const;
};

[[nodiscard]] NavigationAreaIdentity resolveNavigationAreaIdentity(
    const std::filesystem::path& mldPath);

[[nodiscard]] std::optional<std::string> navigationSctAreaKey(
    const std::filesystem::path& sctPath);

} // namespace savor::navigation
