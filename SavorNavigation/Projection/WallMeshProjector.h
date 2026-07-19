#pragma once

#include "../Model/NavigationAreaModel.h"

#include "SpiceMLD/Model/BlenderIrModel.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace savor::navigation {

struct ProjectedWallRegion {
    std::uint32_t sourceEntryId = 0;
    std::size_t sourceTableIndex = 0;
    std::int32_t tblId = 0;
    std::vector<NavigationRegionMesh> meshes{};
    bool complete = false;
};

struct WallMeshProjectionResult {
    std::vector<ProjectedWallRegion> regions{};
    std::vector<NavigationDiagnostic> diagnostics{};
};

// Internal SPICE adapter. Its output is SAVOR-owned so Blender IR does not cross
// the SavorNavigation boundary into Qt or future pathfinding code.
class WallMeshProjector final {
public:
    [[nodiscard]] WallMeshProjectionResult project(
        const spice::mld::model::BlenderIrScene& scene) const;
};

} // namespace savor::navigation
