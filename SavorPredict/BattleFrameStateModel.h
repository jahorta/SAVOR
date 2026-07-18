#pragma once

#include "CombatantVisualDispatcherModel.h"
#include "MovementModel.h"

#include <Core/Memory/Soa/SoaConstants.h>

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
    std::uint32_t status_flags = 0;
    std::uint16_t movement_flags = 0;
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
    std::uint32_t combatant_facing_angle_0x2c = 0;
    std::uint32_t last_written_facing_angle_0x2c = 0;
    float turn_current_degrees_0x11c = 0.0f;
    float turn_target_degrees_0x120 = 0.0f;
    float turn_step_degrees_0x124 = 0.0f;
    float turn_speed_degrees_0x128 = 0.0f;
    std::uint32_t turn_speed_bits_0x128 = 0;
    bool turn_speed_known = false;
    bool turn_state_known = false;
    std::uint32_t selected_action_row_flags = 0;
    int selected_action_row_index = -1;
    std::int16_t selected_action_row_action_id = -1;
    std::int16_t selected_action_row_callback_index = -1;
    std::int16_t selected_action_row_callback_ordinal = -1;
    bool selected_action_row_known = false;
    std::uint32_t selected_action_row_duration_bits = 0;
    bool selected_action_row_duration_known = false;
    bool pending_frame_start_position_sync = false;
    std::uint32_t instruction_flags_0xec = 0;
    std::uint32_t instruction_flags_0xf0 = 0;
    int instruction_target_slot_0x4 = -1;
    std::int16_t visual_instruction_mode_0x6 = 0;
    std::int16_t visual_instruction_subtype_0x8 = -1;
    // These are only populated by an evidence-backed producer. The delay gate
    // must remain MissingInput for special branches until that producer exists.
    std::optional<std::int16_t> visual_instruction_alternate_a_mode_0x4a;
    std::optional<std::int16_t> visual_instruction_alternate_a_subtype_0x4c;
    std::optional<std::int16_t> visual_instruction_alternate_b_mode_0x56;
    std::optional<std::int16_t> visual_instruction_alternate_b_subtype_0x58;
    CombatantVisualInstructionKnowledge visual_instruction_knowledge =
        CombatantVisualInstructionKnowledge::Unknown;
    std::uint64_t visual_instruction_revision = 0;
    int visual_instruction_action_ordinal = -1;
    std::string visual_instruction_provenance;
    int instruction_compare_0x15c = 0;
    bool instruction_compare_known = false;
    std::int16_t combatant_action_mode = 0;
    int queued_controller_state = 0;
};

struct BattleFrameState {
    bool initialized = false;
    int enemy_event_id = -1;
    soa::battle::TurnType initial_turn_type = soa::battle::TurnType::Normal;
    int frame_index = 0;
    std::array<std::uint8_t, 121> active_grid{};
    std::array<std::uint8_t, 121> base_grid{};
    std::vector<BattleFrameCombatantState> combatants;
    std::vector<std::string> warnings;
};

int first_battle_grid_to_raw_stage_coord(int grid, int footprint);
BattleFrameVec3 first_battle_grid_to_raw_stage_position(
    const MovementGridPosition& grid,
    int width,
    int depth);

std::optional<BattleFrameState> initialize_first_battle_frame_state(
    int enemy_event_id,
    const std::vector<MovementSlotState>& slots,
    const std::optional<std::array<std::uint8_t, 81>>& terrain_source_9x9 = std::nullopt,
    soa::battle::TurnType initial_turn_type = soa::battle::TurnType::Normal);

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

float battle_frame_angle_short_to_degrees_8006116c(std::uint32_t angle_word);
std::uint32_t battle_frame_degrees_to_angle_short_8001b1b0(float degrees);
void normalize_turn_shortest_path_80061080(float& current_degrees, float& target_degrees);
bool apply_rotation_increment_80061114(
    float& current_degrees,
    float target_degrees,
    float step_degrees);
float battle_frame_target_facing_degrees_xz(
    const BattleFrameVec3& from,
    const BattleFrameVec3& to,
    float fallback_degrees);

} // namespace savor::predict
