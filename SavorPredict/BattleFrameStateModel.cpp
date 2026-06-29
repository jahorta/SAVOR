#include "BattleFrameStateModel.h"

#include "EnemyEventDataModel.h"
#include "FirstBattleDataModel.h"

#include <algorithm>
#include <cmath>

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

void apply_first_battle_motion_speed_defaults(BattleFrameCombatantState& combatant) {
    const auto defaults = first_battle_actor_by_slot(combatant.slot);
    if (!defaults.has_value() || !defaults->motion_speeds_known) {
        return;
    }
    combatant.motion_base_speed_0x12c = defaults->motion_base_speed;
    combatant.motion_alt_speed_0x130 = defaults->motion_alt_speed;
    combatant.motion_speeds_known = true;
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
        if (!combatant.motion_speeds_known) {
            apply_first_battle_motion_speed_defaults(combatant);
        }
        combatant.grid_position = MovementGridPosition{.grid_x = start->grid_x, .grid_z = start->grid_z};
        combatant.previous_grid_position = combatant.grid_position;
        combatant.pos_holder = first_battle_grid_to_raw_stage_position(
            combatant.grid_position,
            combatant.width,
            combatant.depth);
        combatant.combatant_cur_pos_0x1c = combatant.pos_holder;
        combatant.instruction_field_0xf8 = combatant.pos_holder;
        combatant.pos_to_move_to_0x110 = combatant.pos_holder;
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

} // namespace savor::predict
