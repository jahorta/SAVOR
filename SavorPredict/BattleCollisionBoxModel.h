#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace savor::predict {

enum class BattleCollisionModelStatus {
    Matched,
    Provisional,
    MissingInput,
    Unsupported,
};

struct BattleCollisionVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct BattleCollisionRotationRaw {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
};

struct BattleCollisionTransformRequest {
    BattleCollisionVec3 local{};
    BattleCollisionVec3 origin{};
    BattleCollisionRotationRaw rotation{};
};

struct BattleCollisionTransformResult {
    BattleCollisionModelStatus status = BattleCollisionModelStatus::MissingInput;
    BattleCollisionVec3 world{};
    std::uint32_t world_x_bits = 0;
    std::uint32_t world_y_bits = 0;
    std::uint32_t world_z_bits = 0;
    std::string provenance;
};

struct BattleCollisionOccupancyRuntime {
    std::array<std::int8_t, 81> cells{};
    std::uint64_t revision = 0;
    std::array<std::uint64_t, 12> slot_revisions{};
    std::array<BattleCollisionModelStatus, 12> slot_status{};
};

struct BattleCollisionOccupancyRefreshRequest {
    int slot = -1;
    bool present = false;
    bool alive = false;
    BattleCollisionVec3 position{};
    std::uint32_t instruction_flags_0xec = 0;
    std::optional<bool> mld_slot_valid;
};

struct BattleCollisionOccupancyRefreshResult {
    BattleCollisionModelStatus status = BattleCollisionModelStatus::MissingInput;
    int slot = -1;
    int grid_x = -1;
    int grid_z = -1;
    int cells_written = 0;
    bool excluded = false;
    std::uint64_t revision = 0;
    std::string provenance;
};

BattleCollisionOccupancyRuntime make_battle_collision_occupancy_runtime();

BattleCollisionOccupancyRefreshResult refresh_battle_collision_occupancy(
    BattleCollisionOccupancyRuntime& runtime,
    const BattleCollisionOccupancyRefreshRequest& request);

std::int8_t lookup_battle_collision_occupancy(
    const BattleCollisionOccupancyRuntime& runtime,
    float world_x,
    float world_z);

BattleCollisionTransformResult transform_battle_collision_point(
    const BattleCollisionTransformRequest& request);

BattleCollisionVec3 advance_battle_collision_vector(
    const BattleCollisionVec3& current,
    const BattleCollisionVec3& velocity);

BattleCollisionVec3 reverse_half_step_battle_collision_vector(
    const BattleCollisionVec3& current,
    const BattleCollisionVec3& velocity);

const char* battle_collision_model_status_name(BattleCollisionModelStatus status);

} // namespace savor::predict
