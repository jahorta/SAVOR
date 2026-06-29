#include "BattleFrameStateModel.h"

#include "EnemyEventDataModel.h"
#include "FirstBattleDataModel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace savor::predict {
namespace {

int footprint_adjustment(int footprint) {
    switch (footprint) {
    case 1:
        return 0;
    case 2:
        return 7;
    case 3:
        return 15;
    default:
        return footprint <= 1 ? 0 : (footprint - 1) * 7;
    }
}

std::uint8_t footprint_marker(int slot) {
    return static_cast<std::uint8_t>(slot + 0x50);
}

void initialize_flat_first_battle_grid(BattleFrameState& state) {
    state.active_grid.fill(0);
    state.base_grid.fill(0);
    for (int z = 0; z < 11; ++z) {
        for (int x = 0; x < 11; ++x) {
            if (x == 0 || z == 0 || x == 10 || z == 10) {
                state.active_grid[static_cast<std::size_t>(z * 11 + x)] = 0x7f;
                state.base_grid[static_cast<std::size_t>(z * 11 + x)] = 0x7f;
            }
        }
    }
}

bool in_grid(const MovementGridPosition& position) {
    return position.grid_x >= 0 && position.grid_x < 11
        && position.grid_z >= 0 && position.grid_z < 11;
}

void write_footprint(BattleFrameState& state, const BattleFrameCombatantState& combatant) {
    if (!in_grid(combatant.grid_position)) {
        return;
    }
    const auto marker = footprint_marker(combatant.slot);
    for (int dz = 0; dz < std::max(1, combatant.depth); ++dz) {
        for (int dx = 0; dx < std::max(1, combatant.width); ++dx) {
            const int x = combatant.grid_position.grid_x + dx;
            const int z = combatant.grid_position.grid_z + dz;
            if (x >= 0 && x < 11 && z >= 0 && z < 11) {
                state.active_grid[static_cast<std::size_t>(z * 11 + x)] = marker;
            }
        }
    }
}

constexpr float kPi = 3.14159265358979323846f;
constexpr float kAngleUnitsPerDegree = 65536.0f / 360.0f;

float normalize_degrees_0_360(float degrees) {
    while (degrees < 0.0f) {
        degrees += 360.0f;
    }
    while (degrees >= 360.0f) {
        degrees -= 360.0f;
    }
    return degrees;
}

void apply_first_battle_motion_defaults(BattleFrameCombatantState& combatant) {
    const auto defaults = first_battle_actor_by_slot(combatant.slot);
    if (!defaults.has_value()) {
        return;
    }
    if (!combatant.motion_speeds_known && defaults->motion_speeds_known) {
        combatant.motion_base_speed_0x12c = defaults->motion_base_speed;
        combatant.motion_alt_speed_0x130 = defaults->motion_alt_speed;
        combatant.motion_speeds_known = true;
    }
    if (!combatant.turn_speed_known && defaults->motion_turn_speed_known) {
        combatant.turn_speed_degrees_0x128 = defaults->motion_turn_speed;
        combatant.turn_speed_bits_0x128 = defaults->motion_turn_speed_bits;
        combatant.turn_speed_known = true;
    }
}

} // namespace

int first_battle_grid_to_raw_stage_coord(int grid, int footprint) {
    return grid * 0x0f - 0x4b + footprint_adjustment(footprint);
}

BattleFrameVec3 first_battle_grid_to_raw_stage_position(
    const MovementGridPosition& grid,
    int width,
    int depth) {
    return BattleFrameVec3{
        .x = static_cast<float>(first_battle_grid_to_raw_stage_coord(grid.grid_x, width)),
        .y = 0.0f,
        .z = static_cast<float>(first_battle_grid_to_raw_stage_coord(grid.grid_z, depth)),
    };
}

std::optional<BattleFrameState> initialize_first_battle_frame_state(
    int enemy_event_id,
    const std::vector<MovementSlotState>& slots) {
    if (enemy_event_id != 0) {
        return std::nullopt;
    }

    BattleFrameState state;
    state.initialized = true;
    state.enemy_event_id = enemy_event_id;
    initialize_flat_first_battle_grid(state);

    int node_id = 0;
    for (const auto& slot : slots) {
        if (!slot.present) {
            continue;
        }

        auto start = slot.start_position;
        if (!start.has_value()) {
            start = enemy_event_start_position_for_slot(enemy_event_id, slot.slot);
        }
        if (!start.has_value() || !start->present || start->grid_x < 0 || start->grid_z < 0) {
            state.warnings.push_back(
                "missing first-battle start position for slot " + std::to_string(slot.slot));
            continue;
        }

        BattleFrameCombatantState combatant;
        combatant.slot = slot.slot;
        combatant.present = slot.present;
        combatant.alive = slot.alive;
        combatant.is_player = slot.is_player;
        combatant.width = std::max(1, slot.width);
        combatant.depth = std::max(1, slot.depth);
        combatant.motion_base_speed_0x12c = slot.motion_base_speed;
        combatant.motion_alt_speed_0x130 = slot.motion_alt_speed;
        combatant.motion_speeds_known = slot.motion_speeds_known;
        combatant.turn_speed_degrees_0x128 = slot.motion_turn_speed;
        combatant.turn_speed_bits_0x128 = slot.motion_turn_speed_bits;
        combatant.turn_speed_known = slot.motion_turn_speed_known;
        apply_first_battle_motion_defaults(combatant);
        combatant.grid_position = MovementGridPosition{.grid_x = start->grid_x, .grid_z = start->grid_z};
        combatant.previous_grid_position = combatant.grid_position;
        combatant.pos_holder = first_battle_grid_to_raw_stage_position(
            combatant.grid_position,
            combatant.width,
            combatant.depth);
        combatant.combatant_cur_pos_0x1c = combatant.pos_holder;
        combatant.instruction_field_0xf8 = combatant.pos_holder;
        combatant.pos_to_move_to_0x110 = combatant.pos_holder;
        combatant.turn_current_degrees_0x11c =
            battle_frame_angle_short_to_degrees_8006116c(combatant.combatant_facing_angle_0x2c);
        combatant.turn_target_degrees_0x120 = combatant.turn_current_degrees_0x11c;
        combatant.last_written_facing_angle_0x2c = combatant.combatant_facing_angle_0x2c;
        combatant.instruction_compare_0x15c = combatant.slot;
        combatant.selected_action_row_flags = 0x01000000u;
        combatant.selected_action_row_index = 0;
        state.combatants.push_back(combatant);
    }

    std::sort(
        state.combatants.begin(),
        state.combatants.end(),
        [](const BattleFrameCombatantState& a, const BattleFrameCombatantState& b) {
            return a.slot < b.slot;
        });

    for (const auto& combatant : state.combatants) {
        write_footprint(state, combatant);
        state.packed_thread_order.push_back(BattleFrameThreadState{
            .node_id = node_id++,
            .slot = combatant.slot,
            .active = combatant.present,
            .callback_name = "Thread_BattleCombatant",
        });
    }

    return state;
}

BattleFrameCombatantState* find_frame_combatant(BattleFrameState& state, int slot) {
    const auto it = std::find_if(
        state.combatants.begin(),
        state.combatants.end(),
        [slot](const BattleFrameCombatantState& combatant) {
            return combatant.slot == slot;
        });
    return it == state.combatants.end() ? nullptr : &*it;
}

const BattleFrameCombatantState* find_frame_combatant(const BattleFrameState& state, int slot) {
    const auto it = std::find_if(
        state.combatants.begin(),
        state.combatants.end(),
        [slot](const BattleFrameCombatantState& combatant) {
            return combatant.slot == slot;
        });
    return it == state.combatants.end() ? nullptr : &*it;
}

bool commit_movement_grid_8008178c(
    BattleFrameState& state,
    int slot,
    const MovementGridPosition& destination) {
    auto* combatant = find_frame_combatant(state, slot);
    if (combatant == nullptr || !in_grid(destination)) {
        return false;
    }

    combatant->previous_grid_position = combatant->grid_position;
    combatant->grid_position = destination;
    combatant->pos_holder = first_battle_grid_to_raw_stage_position(
        destination,
        combatant->width,
        combatant->depth);
    combatant->pending_frame_start_position_sync = true;
    write_footprint(state, *combatant);
    return true;
}

bool sync_action_motion_position_8001ab60(
    BattleFrameState& state,
    int current_slot,
    int source_slot) {
    auto* current = find_frame_combatant(state, current_slot);
    const auto* source = find_frame_combatant(state, source_slot);
    if (current == nullptr || source == nullptr) {
        return false;
    }
    current->combatant_cur_pos_0x1c.x = source->pos_holder.x;
    current->combatant_cur_pos_0x1c.z = source->pos_holder.z;
    current->instruction_field_0xf8 = current->combatant_cur_pos_0x1c;
    current->pending_frame_start_position_sync = false;
    return true;
}

bool sync_frame_start_position_from_pos_holder(
    BattleFrameState& state,
    int slot) {
    auto* combatant = find_frame_combatant(state, slot);
    if (combatant == nullptr) {
        return false;
    }
    combatant->combatant_cur_pos_0x1c.x = combatant->pos_holder.x;
    combatant->combatant_cur_pos_0x1c.z = combatant->pos_holder.z;
    combatant->instruction_field_0xf8 = combatant->combatant_cur_pos_0x1c;
    combatant->pending_frame_start_position_sync = false;
    return true;
}

bool move_combatant_increment_80061340(
    BattleFrameVec3& current_position,
    const BattleFrameVec3& target_position,
    const BattleFrameVec3& increment) {
    if (increment.x == 0.0f) {
        current_position.x = target_position.x;
    }
    if (increment.z == 0.0f) {
        current_position.z = target_position.z;
    }

    if (current_position.x == target_position.x
        && current_position.z == target_position.z) {
        return true;
    }

    if (current_position.x != target_position.x) {
        current_position.x += increment.x;
        if ((increment.x <= 0.0f && current_position.x <= target_position.x)
            || (increment.x > 0.0f && current_position.x >= target_position.x)) {
            current_position.x = target_position.x;
        }
    }

    if (current_position.z != target_position.z) {
        current_position.z += increment.z;
        if ((increment.z <= 0.0f && current_position.z <= target_position.z)
            || (increment.z > 0.0f && current_position.z >= target_position.z)) {
            current_position.z = target_position.z;
        }
    }

    return current_position.x == target_position.x
        && current_position.z == target_position.z;
}

float battle_frame_angle_short_to_degrees_8006116c(std::uint32_t angle_word) {
    const auto angle = static_cast<std::uint16_t>(angle_word & 0xffffu);
    return static_cast<float>(angle) * 360.0f / 65536.0f;
}

std::uint32_t battle_frame_degrees_to_angle_short_8001b1b0(float degrees) {
    const float normalized = normalize_degrees_0_360(degrees);
    const auto angle = static_cast<std::uint32_t>(normalized * kAngleUnitsPerDegree);
    return angle & 0xffffu;
}

void normalize_turn_shortest_path_80061080(float& current_degrees, float& target_degrees) {
    current_degrees = normalize_degrees_0_360(current_degrees);
    target_degrees = normalize_degrees_0_360(target_degrees);
    while (target_degrees - current_degrees > 180.0f) {
        target_degrees -= 360.0f;
    }
    while (target_degrees - current_degrees < -180.0f) {
        target_degrees += 360.0f;
    }
}

bool apply_rotation_increment_80061114(
    float& current_degrees,
    float target_degrees,
    float step_degrees) {
    if (current_degrees == target_degrees) {
        return true;
    }
    if (step_degrees == 0.0f
        || (target_degrees > current_degrees && step_degrees < 0.0f)
        || (target_degrees < current_degrees && step_degrees > 0.0f)) {
        current_degrees = target_degrees;
        return true;
    }

    const float next = current_degrees + step_degrees;
    if ((step_degrees > 0.0f && next >= target_degrees)
        || (step_degrees < 0.0f && next <= target_degrees)) {
        current_degrees = target_degrees;
        return true;
    }
    current_degrees = next;
    return false;
}

float battle_frame_target_facing_degrees_xz(
    const BattleFrameVec3& from,
    const BattleFrameVec3& to,
    float fallback_degrees) {
    const float dx = to.x - from.x;
    const float dz = to.z - from.z;
    if (dx == 0.0f && dz == 0.0f) {
        return normalize_degrees_0_360(fallback_degrees);
    }

    const float degrees = std::atan2(dx, -dz) * 180.0f / kPi;
    return normalize_degrees_0_360(degrees);
}

} // namespace savor::predict
