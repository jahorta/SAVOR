#include "NavigationAreaIdentity.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace savor::navigation {
namespace {

[[nodiscard]] std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] bool isAreaKey(const std::string_view value) {
    return value.size() == 4U &&
        std::isdigit(static_cast<unsigned char>(value[0])) != 0 &&
        std::isdigit(static_cast<unsigned char>(value[1])) != 0 &&
        std::isdigit(static_cast<unsigned char>(value[2])) != 0 &&
        std::isalpha(static_cast<unsigned char>(value[3])) != 0;
}

[[nodiscard]] bool hasExtension(const std::filesystem::path& path, const std::string_view expected) {
    return lowerAscii(path.extension().string()) == expected;
}

} // namespace

std::filesystem::path NavigationAreaIdentity::expectedSctPath() const {
    if (!recognized) {
        return {};
    }
    return mldPath.parent_path() / expectedSctFileName;
}

bool NavigationAreaIdentity::matchesSctPath(const std::filesystem::path& path) const {
    const auto candidateKey = navigationSctAreaKey(path);
    return recognized && candidateKey.has_value() && *candidateKey == areaKey;
}

NavigationAreaIdentity resolveNavigationAreaIdentity(const std::filesystem::path& mldPath) {
    NavigationAreaIdentity identity{};
    identity.mldPath = mldPath;

    if (!hasExtension(mldPath, ".mld")) {
        return identity;
    }

    const std::string stem = lowerAscii(mldPath.stem().string());
    if (stem.size() != 5U || stem.front() != 'a') {
        return identity;
    }

    const std::string key = stem.substr(1U);
    if (!isAreaKey(key)) {
        return identity;
    }

    identity.areaKey = key;
    identity.expectedSctFileName = "me" + key + ".sct";
    identity.recognized = true;
    return identity;
}

std::optional<std::string> navigationSctAreaKey(const std::filesystem::path& sctPath) {
    if (!hasExtension(sctPath, ".sct")) {
        return std::nullopt;
    }

    const std::string stem = lowerAscii(sctPath.stem().string());
    if (stem.size() != 6U || !stem.starts_with("me")) {
        return std::nullopt;
    }

    const std::string key = stem.substr(2U);
    if (!isAreaKey(key)) {
        return std::nullopt;
    }
    return key;
}

} // namespace savor::navigation
