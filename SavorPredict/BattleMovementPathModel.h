#pragma once

#include "MovementModel.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

constexpr std::size_t kBattleMovementGridWidth = 11;
constexpr std::size_t kBattleMovementGridCellCount =
    kBattleMovementGridWidth * kBattleMovementGridWidth;
constexpr std::size_t kBattleMovementTerrainWidth = 9;
constexpr std::size_t kBattleMovementTerrainCellCount =
    kBattleMovementTerrainWidth * kBattleMovementTerrainWidth;
constexpr std::size_t kBattleMovementPathCapacity = 11;

enum class BattleMovementPathModelStatus {
    Exact,
    Provisional,
    MissingInput,
    Unsupported,
    Unreachable,
};

enum class MovementCommitDestinationSource {
    SelectedPathNode,
    GeneratedSingleSquare,
    ExplicitGrid,
    Unknown,
};

enum class BattleMovementPathSelectionPolicy {
    StraightRunPathIndex,
    NextPathingGridSquare,
};

struct BattleMovementPathCombatantInput {
    int slot = -1;
    bool present = false;
    bool alive = false;
    std::uint16_t movement_flags = 0;
    int width = 1;
    int depth = 1;
    MovementGridPosition current_grid{};
    MovementGridPosition previous_grid{};
    int queued_controller_state = 0;
};

struct BattleMovementPathInput {
    std::array<std::uint8_t, kBattleMovementGridCellCount> base_grid{};
    std::array<std::uint8_t, kBattleMovementGridCellCount> active_grid{};
    BattleMovementPathCombatantInput actor{};
    BattleMovementPathCombatantInput target{};
    std::uint8_t initial_path_index = 0;
    BattleMovementPathSelectionPolicy selection_policy =
        BattleMovementPathSelectionPolicy::StraightRunPathIndex;
};

struct BattleMovementRouteMarkerMutation {
    MovementGridPosition grid{};
    std::uint8_t before = 0;
    std::uint8_t after = 0;
};

struct BattleMovementPathResult {
    BattleMovementPathModelStatus status = BattleMovementPathModelStatus::MissingInput;
    MovementReachabilityStatus reachability = MovementReachabilityStatus::Unknown;
    std::uint8_t reachability_status_0x16 = 0;
    std::array<std::uint8_t, kBattleMovementGridCellCount> scratch_grid{};
    std::array<std::uint8_t, kBattleMovementGridCellCount> active_grid_after{};
    std::uint8_t distance = 0;
    std::array<MovementGridPosition, kBattleMovementPathCapacity> path_entries{};
    std::size_t entry_count = 0;
    bool terminator_written = false;
    std::vector<BattleMovementRouteMarkerMutation> route_marker_mutations;
    std::uint8_t selected_path_index = 0;
    std::optional<MovementGridPosition> selected_path_node;
    MovementCommitDestinationSource destination_source =
        MovementCommitDestinationSource::Unknown;
    std::string confidence;
    std::string provenance;
};

std::array<std::uint8_t, kBattleMovementGridCellCount>
map_battle_terrain_9x9_to_grid_11x11(
    const std::array<std::uint8_t, kBattleMovementTerrainCellCount>& terrain);

BattleMovementPathResult model_battle_movement_path(
    const BattleMovementPathInput& input);

std::uint8_t select_battle_movement_straight_run_index(
    const MovementGridPosition& current_grid,
    const std::array<MovementGridPosition, kBattleMovementPathCapacity>& entries,
    std::size_t entry_count,
    std::uint8_t initial_path_index);

const char* battle_movement_path_model_status_name(BattleMovementPathModelStatus status);
const char* movement_commit_destination_source_name(MovementCommitDestinationSource source);

} // namespace savor::predict
