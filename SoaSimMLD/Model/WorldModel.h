#pragma once

#include "Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace soasim::mld::model {

struct GrndSurface {
    std::uint32_t id = 0;
    std::uint32_t sourceOffset = 0;
    MeshData mesh{};
    Transform transform{};
    std::vector<std::uint32_t> linkedGrndIds{};
};

struct CollisionVolume {
    std::uint32_t sourceEntryId = 0;
    std::string fxnName{};
    std::uint32_t tblId = 0;
    Transform transform{};
    std::vector<std::uint32_t> objectAddresses{};
    MeshData debugMesh{};
};

struct TriggerVolume {
    std::uint32_t sourceEntryId = 0;
    std::string fxnName{};
    std::uint32_t tblId = 0;
    Transform transform{};
    std::vector<std::uint32_t> objectAddresses{};
    MeshData debugMesh{};
};

struct UnknownEntry {
    std::uint32_t sourceEntryId = 0;
    std::string fxnName{};
    std::uint32_t tblId = 0;
    Transform transform{};
    std::vector<std::uint8_t> rawPayload{};
};

struct WorldModel {
    std::vector<GrndSurface> grndSurfaces{};
    std::vector<CollisionVolume> collisions{};
    std::vector<TriggerVolume> triggers{};
    std::vector<UnknownEntry> unknownEntries{};
};

} // namespace soasim::mld::model
