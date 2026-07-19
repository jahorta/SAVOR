#include "BattleCollisionBoxModel.h"

#include <bit>
#include <cmath>

namespace savor::predict {
namespace {

constexpr float kCollisionGridOrigin = 67.5f;
constexpr float kCollisionGridCellSize = 15.0f;
constexpr std::uint32_t kCollisionAngleScaleBits = 0x38C90FD8U;

float ppc_f32(float value) {
    volatile float rounded = value;
    return rounded;
}

float ppc_add(float left, float right) {
    return ppc_f32(ppc_f32(left) + ppc_f32(right));
}

float ppc_sub(float left, float right) {
    return ppc_f32(ppc_f32(left) - ppc_f32(right));
}

float ppc_mul(float left, float right) {
    return ppc_f32(ppc_f32(left) * ppc_f32(right));
}

std::optional<int> occupancy_index(float value) {
    if (!std::isfinite(value) || value < -kCollisionGridOrigin
        || value >= kCollisionGridOrigin) {
        return std::nullopt;
    }
    const auto translated = ppc_add(value, kCollisionGridOrigin);
    const auto quotient = ppc_f32(translated / kCollisionGridCellSize);
    const int index = static_cast<int>(quotient);
    if (index < 0 || index >= 9) {
        return std::nullopt;
    }
    return index;
}

std::size_t cell_index(int grid_x, int grid_z) {
    return static_cast<std::size_t>(grid_x * 9 + grid_z);
}

} // namespace

BattleCollisionOccupancyRuntime make_battle_collision_occupancy_runtime() {
    BattleCollisionOccupancyRuntime runtime;
    runtime.cells.fill(static_cast<std::int8_t>(-1));
    runtime.slot_status.fill(BattleCollisionModelStatus::MissingInput);
    return runtime;
}

BattleCollisionOccupancyRefreshResult refresh_battle_collision_occupancy(
    BattleCollisionOccupancyRuntime& runtime,
    const BattleCollisionOccupancyRefreshRequest& request) {
    BattleCollisionOccupancyRefreshResult result;
    result.slot = request.slot;
    if (request.slot < 0 || request.slot >= 12) {
        result.status = BattleCollisionModelStatus::MissingInput;
        result.provenance = "FUN_800184F0 requires a combatant slot in [0, 11]";
        return result;
    }

    for (auto& cell : runtime.cells) {
        if (cell == request.slot) {
            cell = static_cast<std::int8_t>(-1);
        }
    }
    ++runtime.revision;
    runtime.slot_revisions[static_cast<std::size_t>(request.slot)] = runtime.revision;

    if (!request.present || !request.alive) {
        result.status = BattleCollisionModelStatus::Matched;
        result.excluded = true;
        result.revision = runtime.revision;
        result.provenance =
            "FUN_800184F0 cleared the absent or dead combatant from the occupancy grid";
        runtime.slot_status[static_cast<std::size_t>(request.slot)] = result.status;
        return result;
    }

    if ((request.instruction_flags_0xec & 0x00000100U) != 0) {
        if (!request.mld_slot_valid.has_value()) {
            result.status = BattleCollisionModelStatus::MissingInput;
            result.excluded = true;
            result.revision = runtime.revision;
            result.provenance =
                "FUN_800184F0 requires the MLD-slot lookup when EC bit 0x100 is set";
            runtime.slot_status[static_cast<std::size_t>(request.slot)] = result.status;
            return result;
        }
        if (!*request.mld_slot_valid) {
            result.status = BattleCollisionModelStatus::Matched;
            result.excluded = true;
            result.revision = runtime.revision;
            result.provenance =
                "FUN_800184F0 excluded the combatant because the required MLD slot was invalid";
            runtime.slot_status[static_cast<std::size_t>(request.slot)] = result.status;
            return result;
        }
    }

    const auto grid_x = occupancy_index(request.position.x);
    const auto grid_z = occupancy_index(request.position.z);
    if (!grid_x.has_value() || !grid_z.has_value()) {
        result.status = BattleCollisionModelStatus::Matched;
        result.excluded = true;
        result.revision = runtime.revision;
        result.provenance =
            "FUN_800184F0 position was outside the bounded 9x9 occupancy grid";
        runtime.slot_status[static_cast<std::size_t>(request.slot)] = result.status;
        return result;
    }

    result.grid_x = *grid_x;
    result.grid_z = *grid_z;
    const bool expanded =
        (request.instruction_flags_0xec & 0x00200000U) != 0;
    const int minimum_x = expanded ? *grid_x - 1 : *grid_x;
    const int maximum_x = expanded ? *grid_x + 1 : *grid_x;
    const int minimum_z = expanded ? *grid_z - 1 : *grid_z;
    const int maximum_z = expanded ? *grid_z + 1 : *grid_z;
    for (int x = minimum_x; x <= maximum_x; ++x) {
        for (int z = minimum_z; z <= maximum_z; ++z) {
            if (x < 0 || x >= 9 || z < 0 || z >= 9) {
                continue;
            }
            runtime.cells[cell_index(x, z)] =
                static_cast<std::int8_t>(request.slot);
            ++result.cells_written;
        }
    }
    result.status = BattleCollisionModelStatus::Matched;
    result.revision = runtime.revision;
    result.provenance = expanded
        ? "FUN_800184F0 wrote the bounded 3x3 EC-bit-0x200000 footprint"
        : "FUN_800184F0 wrote the combatant's single occupancy cell";
    runtime.slot_status[static_cast<std::size_t>(request.slot)] = result.status;
    return result;
}

std::int8_t lookup_battle_collision_occupancy(
    const BattleCollisionOccupancyRuntime& runtime,
    float world_x,
    float world_z) {
    const auto grid_x = occupancy_index(world_x);
    const auto grid_z = occupancy_index(world_z);
    if (!grid_x.has_value() || !grid_z.has_value()) {
        return static_cast<std::int8_t>(-1);
    }
    return runtime.cells[cell_index(*grid_x, *grid_z)];
}

BattleCollisionTransformResult transform_battle_collision_point(
    const BattleCollisionTransformRequest& request) {
    BattleCollisionTransformResult result;
    if (request.rotation.x != 0 || request.rotation.z != 0) {
        result.status = BattleCollisionModelStatus::Unsupported;
        result.provenance =
            "nonzero collision X/Z rotations were not exercised by the accepted corpus";
        return result;
    }

    const float angle_scale = std::bit_cast<float>(kCollisionAngleScaleBits);
    const float angle = ppc_mul(
        static_cast<float>(request.rotation.y), angle_scale);
    const float sine = ppc_f32(std::sin(angle));
    const float cosine = ppc_f32(std::cos(angle));
    const float rotated_x = ppc_add(
        ppc_mul(request.local.x, cosine),
        ppc_mul(request.local.z, sine));
    const float rotated_z = ppc_add(
        ppc_mul(ppc_f32(-request.local.x), sine),
        ppc_mul(request.local.z, cosine));
    result.world = {
        .x = ppc_add(request.origin.x, rotated_x),
        .y = ppc_add(request.origin.y, request.local.y),
        .z = ppc_add(request.origin.z, rotated_z),
    };
    result.world_x_bits = std::bit_cast<std::uint32_t>(result.world.x);
    result.world_y_bits = std::bit_cast<std::uint32_t>(result.world.y);
    result.world_z_bits = std::bit_cast<std::uint32_t>(result.world.z);
    result.status = BattleCollisionModelStatus::Matched;
    result.provenance =
        "FUN_80292080 Y rotation used signed 32-bit units, float scale 0x38C90FD8, and PPC-order single-precision operations";
    return result;
}

BattleCollisionVec3 advance_battle_collision_vector(
    const BattleCollisionVec3& current,
    const BattleCollisionVec3& velocity) {
    return {
        .x = ppc_add(current.x, velocity.x),
        .y = ppc_add(current.y, velocity.y),
        .z = ppc_add(current.z, velocity.z),
    };
}

BattleCollisionVec3 reverse_half_step_battle_collision_vector(
    const BattleCollisionVec3& current,
    const BattleCollisionVec3& velocity) {
    constexpr float half = 0.5f;
    return {
        .x = ppc_sub(current.x, ppc_mul(velocity.x, half)),
        .y = ppc_sub(current.y, ppc_mul(velocity.y, half)),
        .z = ppc_sub(current.z, ppc_mul(velocity.z, half)),
    };
}

const char* battle_collision_model_status_name(BattleCollisionModelStatus status) {
    switch (status) {
    case BattleCollisionModelStatus::Matched: return "Matched";
    case BattleCollisionModelStatus::Provisional: return "Provisional";
    case BattleCollisionModelStatus::MissingInput: return "MissingInput";
    case BattleCollisionModelStatus::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

} // namespace savor::predict
