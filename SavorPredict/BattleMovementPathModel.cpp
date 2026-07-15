#include "BattleMovementPathModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <deque>

namespace savor::predict {
namespace {

constexpr std::array<MovementGridPosition, 8> kPathFallbackNeighbors = {{
    {.grid_x = -1, .grid_z = 0},
    {.grid_x = 1, .grid_z = 0},
    {.grid_x = 0, .grid_z = -1},
    {.grid_x = 0, .grid_z = 1},
    {.grid_x = -1, .grid_z = -1},
    {.grid_x = -1, .grid_z = 1},
    {.grid_x = 1, .grid_z = -1},
    {.grid_x = 1, .grid_z = 1},
}};

constexpr std::size_t grid_index(int x, int z) {
    return static_cast<std::size_t>(z) * kBattleMovementGridWidth
        + static_cast<std::size_t>(x);
}

bool in_grid(int x, int z) {
    return x >= 0 && x < static_cast<int>(kBattleMovementGridWidth)
        && z >= 0 && z < static_cast<int>(kBattleMovementGridWidth);
}

bool in_grid(const MovementGridPosition& position) {
    return in_grid(position.grid_x, position.grid_z);
}

bool same_grid(const MovementGridPosition& a, const MovementGridPosition& b) {
    return a.grid_x == b.grid_x && a.grid_z == b.grid_z;
}

bool footprint_in_grid(const BattleMovementPathCombatantInput& combatant, int x, int z) {
    return x >= 0 && z >= 0
        && x + std::max(1, combatant.width) <= static_cast<int>(kBattleMovementGridWidth)
        && z + std::max(1, combatant.depth) <= static_cast<int>(kBattleMovementGridWidth);
}

bool accepted_non_own_cell(std::uint8_t cell, std::uint16_t movement_flags) {
    switch (movement_flags & 0x0cu) {
    case 0x04u:
        return cell <= 0x4fu;
    case 0x08u:
        return cell >= 0x01u && cell <= 0x4fu;
    case 0x0cu:
        return cell <= 0x4fu || cell == 0x7cu;
    default:
        return true;
    }
}

bool footprint_is_traversable(
    const std::array<std::uint8_t, kBattleMovementGridCellCount>& grid,
    const BattleMovementPathCombatantInput& actor,
    int x,
    int z) {
    if (!footprint_in_grid(actor, x, z)) {
        return false;
    }
    const auto own_marker = static_cast<std::uint8_t>(actor.slot + 0x50);
    for (int dz = 0; dz < std::max(1, actor.depth); ++dz) {
        for (int dx = 0; dx < std::max(1, actor.width); ++dx) {
            const auto cell = grid[grid_index(x + dx, z + dz)];
            if (cell != own_marker && !accepted_non_own_cell(cell, actor.movement_flags)) {
                return false;
            }
        }
    }
    return true;
}

bool target_marker_adjacent(
    const std::array<std::uint8_t, kBattleMovementGridCellCount>& grid,
    const BattleMovementPathCombatantInput& actor,
    const BattleMovementPathCombatantInput& target) {
    const auto target_marker = static_cast<std::uint8_t>(target.slot + 0x50);
    const int left = actor.current_grid.grid_x - 1;
    const int right = actor.current_grid.grid_x + std::max(1, actor.width);
    const int top = actor.current_grid.grid_z - 1;
    const int bottom = actor.current_grid.grid_z + std::max(1, actor.depth);
    const auto contains_target = [&](int x, int z) {
        return in_grid(x, z) && grid[grid_index(x, z)] == target_marker;
    };
    for (int z = top; z <= bottom; ++z) {
        if (contains_target(left, z) || contains_target(right, z)) {
            return true;
        }
    }
    for (int x = actor.current_grid.grid_x; x < right; ++x) {
        if (contains_target(x, top) || contains_target(x, bottom)) {
            return true;
        }
    }
    return false;
}

void write_footprint(
    std::array<std::uint8_t, kBattleMovementGridCellCount>& grid,
    const BattleMovementPathCombatantInput& combatant,
    const MovementGridPosition& position,
    std::uint8_t value) {
    if (!footprint_in_grid(combatant, position.grid_x, position.grid_z)) {
        return;
    }
    for (int dz = 0; dz < std::max(1, combatant.depth); ++dz) {
        for (int dx = 0; dx < std::max(1, combatant.width); ++dx) {
            grid[grid_index(position.grid_x + dx, position.grid_z + dz)] = value;
        }
    }
}

void restore_actor_pathing_footprint(
    std::array<std::uint8_t, kBattleMovementGridCellCount>& scratch,
    const std::array<std::uint8_t, kBattleMovementGridCellCount>& base,
    const BattleMovementPathCombatantInput& actor) {
    const int width = std::max(1, actor.width);
    const int depth = std::max(1, actor.depth);
    for (int dz = 0; dz < depth; ++dz) {
        for (int dx = 0; dx < width; ++dx) {
            if (dx == 0 && dz == 0) {
                continue;
            }
            const int x = actor.current_grid.grid_x + dx;
            const int z = actor.current_grid.grid_z + dz;
            if (in_grid(x, z)) {
                scratch[grid_index(x, z)] = base[grid_index(x, z)];
            }
        }
    }
}

bool orthogonally_adjacent_to_actor(
    const MovementGridPosition& square,
    const MovementGridPosition& actor) {
    return std::abs(actor.grid_x - square.grid_x)
            + std::abs(actor.grid_z - square.grid_z)
        == 1;
}

bool diagonally_reaches_actor(
    const std::array<std::uint8_t, kBattleMovementGridCellCount>& scratch,
    const BattleMovementPathCombatantInput& actor,
    const MovementGridPosition& square) {
    const int dx = actor.current_grid.grid_x - square.grid_x;
    const int dz = actor.current_grid.grid_z - square.grid_z;
    if (std::abs(dx) != 1 || std::abs(dz) != 1) {
        return false;
    }
    return footprint_is_traversable(
               scratch,
               actor,
               square.grid_x,
               actor.current_grid.grid_z)
        && footprint_is_traversable(
               scratch,
               actor,
               actor.current_grid.grid_x,
               square.grid_z);
}

bool same_direction_multiple(
    const MovementGridPosition& origin,
    const MovementGridPosition& candidate,
    int dx,
    int dz,
    int multiple) {
    return candidate.grid_x - origin.grid_x == dx * multiple
        && candidate.grid_z - origin.grid_z == dz * multiple;
}

} // namespace

std::array<std::uint8_t, kBattleMovementGridCellCount>
map_battle_terrain_9x9_to_grid_11x11(
    const std::array<std::uint8_t, kBattleMovementTerrainCellCount>& terrain) {
    std::array<std::uint8_t, kBattleMovementGridCellCount> result{};
    result.fill(0x7fu);
    for (std::size_t z = 0; z < kBattleMovementTerrainWidth; ++z) {
        for (std::size_t x = 0; x < kBattleMovementTerrainWidth; ++x) {
            const auto source = terrain[z * kBattleMovementTerrainWidth + x];
            std::uint8_t mapped = 0;
            if (source == 1u) {
                mapped = 0x7du;
            } else if (source == 2u || source == 4u) {
                mapped = 0x7cu;
            }
            result[(z + 1) * kBattleMovementGridWidth + (x + 1)] = mapped;
        }
    }
    return result;
}

std::uint8_t select_battle_movement_straight_run_index(
    const MovementGridPosition& current_grid,
    const std::array<MovementGridPosition, kBattleMovementPathCapacity>& entries,
    std::size_t entry_count,
    std::uint8_t initial_path_index) {
    if (entry_count == 0 || initial_path_index >= entry_count) {
        return initial_path_index;
    }

    std::size_t selected = initial_path_index;
    const auto& first = entries[selected];
    const int first_dx = first.grid_x - current_grid.grid_x;
    const int first_dz = first.grid_z - current_grid.grid_z;
    while (selected + 1 < entry_count
        && same_direction_multiple(
            current_grid,
            entries[selected + 1],
            first_dx,
            first_dz,
            static_cast<int>(selected - initial_path_index + 2))) {
        ++selected;
    }

    if (selected + 1 >= entry_count) {
        return static_cast<std::uint8_t>(selected);
    }

    const auto& bend_origin = entries[selected];
    const auto& bend_first = entries[selected + 1];
    const int bend_dx = bend_first.grid_x - bend_origin.grid_x;
    const int bend_dz = bend_first.grid_z - bend_origin.grid_z;
    if (first_dx != bend_dx && first_dz != bend_dz) {
        return static_cast<std::uint8_t>(selected);
    }

    std::size_t bend_selected = selected + 1;
    while (bend_selected + 1 < entry_count
        && same_direction_multiple(
            bend_origin,
            entries[bend_selected + 1],
            bend_dx,
            bend_dz,
            static_cast<int>(bend_selected - selected + 1))) {
        ++bend_selected;
    }
    return static_cast<std::uint8_t>(bend_selected);
}

BattleMovementPathResult model_battle_movement_path(
    const BattleMovementPathInput& input) {
    BattleMovementPathResult result;
    result.scratch_grid = input.active_grid;
    result.active_grid_after = input.active_grid;
    result.provenance =
        "FUN_80083728/updatePathingGrid/FUN_800823E4/FUN_8007FE0C static contract";
    result.confidence = "provisional generic integration; first-battle width-1 path primitives validated";

    if (!input.actor.present || !in_grid(input.actor.current_grid)) {
        result.status = BattleMovementPathModelStatus::MissingInput;
        return result;
    }
    if (!input.target.present || !input.target.alive) {
        result.status = BattleMovementPathModelStatus::Unreachable;
        result.reachability = MovementReachabilityStatus::Failed0;
        result.confidence = "target is not a live pathing candidate";
        return result;
    }
    if (!in_grid(input.target.current_grid)) {
        result.status = BattleMovementPathModelStatus::MissingInput;
        return result;
    }
    if (input.actor.width <= 0 || input.actor.depth <= 0
        || input.target.width <= 0 || input.target.depth <= 0) {
        result.status = BattleMovementPathModelStatus::MissingInput;
        return result;
    }
    if ((input.actor.movement_flags & 0x0cu) != 0x04u
        && (input.actor.movement_flags & 0x0cu) != 0x08u
        && (input.actor.movement_flags & 0x0cu) != 0x0cu) {
        result.status = BattleMovementPathModelStatus::Unsupported;
        result.confidence = "unsupported movement-flag gate";
        return result;
    }

    if (target_marker_adjacent(input.active_grid, input.actor, input.target)) {
        result.status = BattleMovementPathModelStatus::Exact;
        result.reachability = MovementReachabilityStatus::Adjacent1;
        result.reachability_status_0x16 = 1;
        result.terminator_written = true;
        result.destination_source = MovementCommitDestinationSource::Unknown;
        result.confidence = "validated adjacent return";
        return result;
    }

    if (input.actor.queued_controller_state == 4
        && input.actor.previous_grid.grid_x != 0) {
        write_footprint(
            result.scratch_grid,
            input.actor,
            input.actor.previous_grid,
            0x7du);
    }
    write_footprint(
        result.scratch_grid,
        input.actor,
        input.actor.current_grid,
        static_cast<std::uint8_t>(input.actor.slot + 0x50));
    restore_actor_pathing_footprint(result.scratch_grid, input.base_grid, input.actor);

    std::deque<MovementGridPosition> queue;
    const auto seed = [&](int x, int z) {
        if (!in_grid(x, z)
            || !footprint_is_traversable(result.scratch_grid, input.actor, x, z)) {
            return;
        }
        auto& cell = result.scratch_grid[grid_index(x, z)];
        if (cell != 0 && cell != static_cast<std::uint8_t>(input.actor.slot + 0x50)) {
            return;
        }
        cell = 1;
        queue.push_back({.grid_x = x, .grid_z = z});
    };

    const int left = input.target.current_grid.grid_x - std::max(1, input.actor.width);
    const int right = input.target.current_grid.grid_x + std::max(1, input.target.width);
    const int top = input.target.current_grid.grid_z - std::max(1, input.actor.depth);
    const int bottom = input.target.current_grid.grid_z + std::max(1, input.target.depth);
    for (int z = top; z <= bottom; ++z) {
        seed(left, z);
    }
    for (int z = top; z <= bottom; ++z) {
        seed(right, z);
    }
    for (int x = left + 1; x < right; ++x) {
        seed(x, top);
    }
    for (int x = left + 1; x < right; ++x) {
        seed(x, bottom);
    }

    std::optional<MovementGridPosition> found;
    const auto try_enter = [&](int x, int z, std::uint8_t value) {
        if (!in_grid(x, z)
            || !footprint_is_traversable(result.scratch_grid, input.actor, x, z)) {
            return 0;
        }
        auto& cell = result.scratch_grid[grid_index(x, z)];
        if (cell == 0 || value < cell) {
            cell = value;
            queue.push_back({.grid_x = x, .grid_z = z});
            return 2;
        }
        return 1;
    };

    while (!queue.empty()) {
        const auto current = queue.front();
        queue.pop_front();
        if (orthogonally_adjacent_to_actor(current, input.actor.current_grid)
            || diagonally_reaches_actor(result.scratch_grid, input.actor, current)) {
            found = current;
            break;
        }

        const auto value = static_cast<std::uint8_t>(
            result.scratch_grid[grid_index(current.grid_x, current.grid_z)] + 1u);
        const bool west_blocked = try_enter(
            current.grid_x - 1,
            current.grid_z,
            value) == 0;
        const bool north_blocked = try_enter(
            current.grid_x,
            current.grid_z - 1,
            value) == 0;
        const bool south_blocked = try_enter(
            current.grid_x,
            current.grid_z + 1,
            value) == 0;
        const bool east_blocked = try_enter(
            current.grid_x + 1,
            current.grid_z,
            value) == 0;
        if (!west_blocked && !north_blocked) {
            try_enter(current.grid_x - 1, current.grid_z - 1, value);
        }
        if (!west_blocked && !south_blocked) {
            try_enter(current.grid_x - 1, current.grid_z + 1, value);
        }
        if (!east_blocked && !north_blocked) {
            try_enter(current.grid_x + 1, current.grid_z - 1, value);
        }
        if (!east_blocked && !south_blocked) {
            try_enter(current.grid_x + 1, current.grid_z + 1, value);
        }
    }

    if (!found.has_value()) {
        result.status = BattleMovementPathModelStatus::Unreachable;
        result.reachability = MovementReachabilityStatus::Failed0;
        result.reachability_status_0x16 = 0;
        return result;
    }

    result.distance = result.scratch_grid[grid_index(found->grid_x, found->grid_z)];
    result.reachability = MovementReachabilityStatus::Path4;
    result.reachability_status_0x16 = 4;
    MovementGridPosition current = input.actor.current_grid;
    MovementGridPosition previous_delta{.grid_x = 0, .grid_z = 0};
    for (int distance = result.distance;
         distance > 0 && result.entry_count < kBattleMovementPathCapacity;
         --distance) {
        std::array<MovementGridPosition, 9> candidates{};
        std::size_t candidate_count = 0;
        if (previous_delta.grid_x != 0 || previous_delta.grid_z != 0) {
            candidates[candidate_count++] = previous_delta;
        }
        for (const auto& fallback : kPathFallbackNeighbors) {
            if (candidate_count != 0
                && fallback.grid_x == candidates[0].grid_x
                && fallback.grid_z == candidates[0].grid_z) {
                continue;
            }
            candidates[candidate_count++] = fallback;
        }

        std::optional<MovementGridPosition> next;
        for (std::size_t i = 0; i < candidate_count; ++i) {
            const int x = current.grid_x + candidates[i].grid_x;
            const int z = current.grid_z + candidates[i].grid_z;
            if (in_grid(x, z)
                && result.scratch_grid[grid_index(x, z)] == distance) {
                next = MovementGridPosition{.grid_x = x, .grid_z = z};
                break;
            }
        }
        if (!next.has_value()) {
            result.status = BattleMovementPathModelStatus::Provisional;
            result.confidence = "path gradient did not contain the requested neighbor distance";
            break;
        }

        previous_delta = {
            .grid_x = current.grid_x - next->grid_x,
            .grid_z = current.grid_z - next->grid_z,
        };
        current = *next;
        result.path_entries[result.entry_count++] = current;
        auto& active_cell = result.active_grid_after[grid_index(current.grid_x, current.grid_z)];
        const auto marker = static_cast<std::uint8_t>(0x60 + distance);
        result.route_marker_mutations.push_back({
            .grid = current,
            .before = active_cell,
            .after = marker,
        });
        active_cell = marker;
    }

    result.terminator_written = result.entry_count == result.distance
        && result.entry_count <= kBattleMovementPathCapacity;
    if (result.entry_count == 0) {
        if (result.status != BattleMovementPathModelStatus::Provisional) {
            result.status = BattleMovementPathModelStatus::Unreachable;
        }
        return result;
    }
    if (input.selection_policy
        == BattleMovementPathSelectionPolicy::NextPathingGridSquare) {
        result.selected_path_index = 0;
        result.confidence =
            "validated next-square selection from the current pathing-grid gradient";
    } else {
        result.selected_path_index = select_battle_movement_straight_run_index(
            input.actor.current_grid,
            result.path_entries,
            result.entry_count,
            input.initial_path_index);
    }
    if (result.selected_path_index < result.entry_count) {
        result.selected_path_node = result.path_entries[result.selected_path_index];
        result.destination_source = input.selection_policy
                == BattleMovementPathSelectionPolicy::NextPathingGridSquare
            ? MovementCommitDestinationSource::GeneratedSingleSquare
            : MovementCommitDestinationSource::SelectedPathNode;
    } else {
        result.status = BattleMovementPathModelStatus::Unsupported;
        result.confidence = "selected path index points past the path terminator";
        return result;
    }
    if (result.status != BattleMovementPathModelStatus::Provisional) {
        result.status = input.actor.width == 1 && input.actor.depth == 1
            ? BattleMovementPathModelStatus::Exact
            : BattleMovementPathModelStatus::Provisional;
    }
    return result;
}

const char* battle_movement_path_model_status_name(BattleMovementPathModelStatus status) {
    switch (status) {
    case BattleMovementPathModelStatus::Exact:
        return "Exact";
    case BattleMovementPathModelStatus::Provisional:
        return "Provisional";
    case BattleMovementPathModelStatus::MissingInput:
        return "MissingInput";
    case BattleMovementPathModelStatus::Unsupported:
        return "Unsupported";
    case BattleMovementPathModelStatus::Unreachable:
        return "Unreachable";
    }
    return "Unknown";
}

const char* movement_commit_destination_source_name(MovementCommitDestinationSource source) {
    switch (source) {
    case MovementCommitDestinationSource::SelectedPathNode:
        return "SelectedPathNode";
    case MovementCommitDestinationSource::GeneratedSingleSquare:
        return "GeneratedSingleSquare";
    case MovementCommitDestinationSource::ExplicitGrid:
        return "ExplicitGrid";
    case MovementCommitDestinationSource::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

} // namespace savor::predict
