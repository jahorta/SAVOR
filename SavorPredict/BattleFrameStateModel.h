#pragma once

#include "MovementModel.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

struct BattleFrameVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct BattleFrameCombatantState {
    int slot = -1;
    bool present = false;
    bool alive = false;
    bool is_player = false;
    int width = 1;
    int depth = 1;
    MovementGridPosition grid_position{};
    MovementGridPosition previous_grid_position{};
    BattleFrameVec3 pos_holder{};
    BattleFrameVec3 combatant_cur_pos_0x1c{};
    BattleFrameVec3 instruction_field_0xf8{};
    BattleFrameVec3 pos_to_move_to_0x110{};
    BattleFrameVec3 move_increment_0x104{};
    BattleFrameVec3 last_applied_move_increment{};
    float motion_base_speed_0x12c = 0.0f;
    float motion_alt_speed_0x130 = 0.0f;
    float selected_motion_speed = 0.0f;
    bool motion_speeds_known = false;
    std::uint32_t selected_action_row_flags = 0;
    int selected_action_row_index = -1;
    bool pending_frame_start_position_sync = false;
    std::uint32_t instruction_flags_0xec = 0;
    std::uint32_t instruction_flags_0xf0 = 0;
    int instruction_compare_0x15c = 0;
    std::int16_t combatant_action_mode = 0;
};

struct BattleFrameThreadState {
    int node_id = -1;
    int slot = -1;
    bool active = false;
    std::string callback_name;
};

struct BattleFrameState {
    bool initialized = false;
    int enemy_event_id = -1;
    int frame_index = 0;
    std::array<std::uint8_t, 121> active_grid{};
    std::array<std::uint8_t, 121> base_grid{};
    std::vector<BattleFrameCombatantState> combatants;
    std::vector<BattleFrameThreadState> packed_thread_order;
    std::vector<std::string> warnings;
};

int first_battle_grid_to_raw_stage_coord(int grid, int footprint);
BattleFrameVec3 first_battle_grid_to_raw_stage_position(
    const MovementGridPosition& grid,
    int width,
    int depth);

std::optional<BattleFrameState> initialize_first_battle_frame_state(
    int enemy_event_id,
    const std::vector<MovementSlotState>& slots);

BattleFrameCombatantState* find_frame_combatant(BattleFrameState& state, int slot);
const BattleFrameCombatantState* find_frame_combatant(const BattleFrameState& state, int slot);

bool commit_movement_grid_8008178c(
    BattleFrameState& state,
    int slot,
    const MovementGridPosition& destination);

bool sync_action_motion_position_8001ab60(
    BattleFrameState& state,
    int current_slot,
    int source_slot);

bool sync_frame_start_position_from_pos_holder(
    BattleFrameState& state,
    int slot);

bool move_combatant_increment_80061340(
    BattleFrameVec3& current_position,
    const BattleFrameVec3& target_position,
    const BattleFrameVec3& increment);

} // namespace savor::predict
