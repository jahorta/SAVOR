#pragma once

#include "../Model/NavigationAreaModel.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace savor::navigation::detail {

// Ordered MLD entry-table evidence used to normalize the runtime ground
// fallback contract without exposing SPICE parser types beyond the loader.
struct NavigationGroundFallbackEntryEvidence {
    std::size_t tableIndex = 0;
    std::uint32_t entryId = 0;
    std::vector<std::uint32_t> targetEntryIds{};
};

struct NavigationGroundFallbackNormalizationResult {
    std::vector<NavigationAuthoredGroundFallbackChain> chains{};
    std::vector<NavigationDiagnostic> diagnostics{};
};

class NavigationGroundFallbackNormalizer final {
public:
    // entries must retain MLD table-slot order because the runtime first treats
    // an in-range EntryID as a slot before performing its first-match scan.
    [[nodiscard]] NavigationGroundFallbackNormalizationResult normalize(
        std::span<const NavigationGroundFallbackEntryEvidence> entries,
        std::span<const NavigationSurface> surfaces) const;
};

} // namespace savor::navigation::detail
