#pragma once

#include "Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace soasim::mld::model {

struct WalkSurfaceNode {
    std::uint32_t grndId = 0;
    std::vector<std::uint32_t> neighborGrndIds{};
    MeshData mesh{};
};

struct EncounterOrTriggerRegion {
    std::uint32_t sourceEntryId = 0;
    std::string fxnName{};
    std::uint32_t tblId = 0;
    Transform transform{};
};

struct SearchWorldModel {
    std::vector<WalkSurfaceNode> surfaces{};
    std::vector<EncounterOrTriggerRegion> regions{};
};

} // namespace soasim::mld::model
