#pragma once

#include "../Model/NavigationAreaModel.h"

#include "SpiceMLD/Model/BlenderIrModel.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace savor::navigation {

struct RegionMeshProjectionTarget {
    NavigationRegionKind kind = NavigationRegionKind::Unknown;
    std::uint32_t sourceEntryId = 0;
    std::int32_t tblId = 0;
};

struct ProjectedNavigationRegion {
    NavigationRegionKind kind = NavigationRegionKind::Unknown;
    std::uint32_t sourceEntryId = 0;
    std::size_t sourceTableIndex = 0;
    std::int32_t tblId = 0;
    std::vector<NavigationRegionMesh> meshes{};
    bool complete = false;
};

struct RegionMeshProjectionResult {
    std::vector<ProjectedNavigationRegion> regions{};
    std::vector<NavigationDiagnostic> diagnostics{};
};

// Internal SPICE adapter. Its output is SAVOR-owned so Blender IR does not cross
// the SavorNavigation boundary into Qt or future pathfinding code.
class RegionMeshProjector final {
public:
    [[nodiscard]] RegionMeshProjectionResult project(
        const spice::mld::model::BlenderIrScene& scene,
        std::span<const RegionMeshProjectionTarget> targets) const;
};

} // namespace savor::navigation
