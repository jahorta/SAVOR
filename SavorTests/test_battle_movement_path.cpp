#include "ActionMotionTargetModel.h"
#include "BattleFrameSchedulerModel.h"
#include "BattleMovementPathModel.h"
#include "BattlePredictionScenario.h"
#include "BattleSourceModel.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace savor::predict {
namespace {

std::array<std::uint8_t, kBattleMovementGridCellCount> flat_grid()
{
    std::array<std::uint8_t, kBattleMovementGridCellCount> grid{};
    for (int z = 0; z < 11; ++z) {
        for (int x = 0; x < 11; ++x) {
            if (x == 0 || z == 0 || x == 10 || z == 10) {
                grid[static_cast<std::size_t>(z * 11 + x)] = 0x7f;
            }
        }
    }
    return grid;
}

void place_slot(
    std::array<std::uint8_t, kBattleMovementGridCellCount>& grid,
    int slot,
    int x,
    int z)
{
    grid[static_cast<std::size_t>(z * 11 + x)] =
        static_cast<std::uint8_t>(slot + 0x50);
}

BattleMovementPathInput simple_path_input(
    MovementGridPosition actor,
    MovementGridPosition target)
{
    BattleMovementPathInput input;
    input.base_grid = flat_grid();
    input.active_grid = input.base_grid;
    place_slot(input.active_grid, 0, actor.grid_x, actor.grid_z);
    place_slot(input.active_grid, 4, target.grid_x, target.grid_z);
    input.actor = BattleMovementPathCombatantInput{
        .slot = 0,
        .present = true,
        .alive = true,
        .movement_flags = 0x0fc7,
        .width = 1,
        .depth = 1,
        .current_grid = actor,
        .previous_grid = actor,
    };
    input.target = BattleMovementPathCombatantInput{
        .slot = 4,
        .present = true,
        .alive = true,
        .movement_flags = 0x0fc7,
        .width = 1,
        .depth = 1,
        .current_grid = target,
        .previous_grid = target,
    };
    return input;
}

struct CapturedGridCell {
    int x = 0;
    int z = 0;
    std::uint8_t value = 0;
};

BattleMovementPathInput captured_s001_path_input(
    int actor_slot,
    int target_slot,
    MovementGridPosition actor_current,
    MovementGridPosition actor_previous,
    MovementGridPosition target_current,
    MovementGridPosition target_previous,
    std::uint16_t actor_flags,
    std::uint16_t target_flags,
    const std::vector<CapturedGridCell>& active_cells)
{
    const auto scenario = first_battle_soldiers_prediction_scenario();
    const auto resolved = resolve_battle_source_bundle(
        scenario.source_selection);
    EXPECT_TRUE(resolved.bundle.ok);
    const auto& source = resolved.bundle;

    BattleMovementPathInput input;
    input.base_grid = map_battle_terrain_9x9_to_grid_11x11(
        source.snapshot.terrain_source_9x9);
    input.active_grid = input.base_grid;
    for (const auto& cell : active_cells) {
        input.active_grid[static_cast<std::size_t>(cell.z * 11 + cell.x)] = cell.value;
    }
    input.actor = BattleMovementPathCombatantInput{
        .slot = actor_slot,
        .present = true,
        .alive = true,
        .movement_flags = actor_flags,
        .width = 1,
        .depth = 1,
        .current_grid = actor_current,
        .previous_grid = actor_previous,
    };
    input.target = BattleMovementPathCombatantInput{
        .slot = target_slot,
        .present = true,
        .alive = true,
        .movement_flags = target_flags,
        .width = 1,
        .depth = 1,
        .current_grid = target_current,
        .previous_grid = target_previous,
    };
    return input;
}

TEST(BattleMovementPathModelTest, TerrainConversionPreservesValidatedSentinels)
{
    std::array<std::uint8_t, kBattleMovementTerrainCellCount> terrain{};
    terrain[0] = 0;
    terrain[1] = 1;
    terrain[2] = 2;
    terrain[3] = 3;
    terrain[4] = 4;
    terrain[5] = 5;

    const auto grid = map_battle_terrain_9x9_to_grid_11x11(terrain);

    EXPECT_EQ(grid[0], 0x7f);
    EXPECT_EQ(grid[1 * 11 + 1], 0x00);
    EXPECT_EQ(grid[1 * 11 + 2], 0x7d);
    EXPECT_EQ(grid[1 * 11 + 3], 0x7c);
    EXPECT_EQ(grid[1 * 11 + 4], 0x00);
    EXPECT_EQ(grid[1 * 11 + 5], 0x7c);
    EXPECT_EQ(grid[1 * 11 + 6], 0x00);
}

TEST(BattleMovementPathModelTest, CapturedStraightPathBuildsSelectedNodeAndRouteMarker)
{
    const auto result = model_battle_movement_path(simple_path_input(
        {.grid_x = 5, .grid_z = 4},
        {.grid_x = 5, .grid_z = 2}));

    ASSERT_EQ(result.status, BattleMovementPathModelStatus::Exact);
    EXPECT_EQ(result.reachability_status_0x16, 4);
    EXPECT_EQ(result.distance, 1);
    ASSERT_EQ(result.entry_count, 1u);
    EXPECT_EQ(result.path_entries[0].grid_x, 5);
    EXPECT_EQ(result.path_entries[0].grid_z, 3);
    EXPECT_TRUE(result.terminator_written);
    ASSERT_TRUE(result.selected_path_node.has_value());
    EXPECT_EQ(result.selected_path_index, 0);
    EXPECT_EQ(result.destination_source, MovementCommitDestinationSource::SelectedPathNode);
    ASSERT_EQ(result.route_marker_mutations.size(), 1u);
    EXPECT_EQ(result.route_marker_mutations[0].after, 0x61);
}

TEST(BattleMovementPathModelTest, AdjacentTargetDoesNotInventAPathCommit)
{
    const auto result = model_battle_movement_path(simple_path_input(
        {.grid_x = 5, .grid_z = 4},
        {.grid_x = 5, .grid_z = 3}));

    EXPECT_EQ(result.status, BattleMovementPathModelStatus::Exact);
    EXPECT_EQ(result.reachability_status_0x16, 1);
    EXPECT_EQ(result.entry_count, 0u);
    EXPECT_FALSE(result.selected_path_node.has_value());
    EXPECT_EQ(result.destination_source, MovementCommitDestinationSource::Unknown);
}

TEST(BattleMovementPathModelTest, StraightRunSelectionUsesFarthestCollinearNode)
{
    std::array<MovementGridPosition, kBattleMovementPathCapacity> entries{};
    entries[0] = {.grid_x = 5, .grid_z = 4};
    entries[1] = {.grid_x = 5, .grid_z = 3};
    entries[2] = {.grid_x = 5, .grid_z = 2};
    entries[3] = {.grid_x = 6, .grid_z = 1};

    EXPECT_EQ(
        select_battle_movement_straight_run_index(
            {.grid_x = 5, .grid_z = 5},
            entries,
            4,
            0),
        3);
}

TEST(BattleMovementPathModelTest, ElevenEntrySelectionDoesNotTruncateToLegacyCapacity)
{
    std::array<MovementGridPosition, kBattleMovementPathCapacity> entries{};
    for (std::size_t index = 0; index < entries.size(); ++index) {
        entries[index] = {
            .grid_x = static_cast<int>(index + 1),
            .grid_z = 5,
        };
    }

    EXPECT_EQ(
        select_battle_movement_straight_run_index(
            {.grid_x = 0, .grid_z = 5},
            entries,
            entries.size(),
            0),
        10);
}

TEST(BattleMovementPathModelTest, UnsupportedMovementFlagGateRemainsExplicit)
{
    auto input = simple_path_input(
        {.grid_x = 5, .grid_z = 4},
        {.grid_x = 5, .grid_z = 2});
    input.actor.movement_flags = 0;

    const auto result = model_battle_movement_path(input);

    EXPECT_EQ(result.status, BattleMovementPathModelStatus::Unsupported);
    EXPECT_FALSE(result.selected_path_node.has_value());
}

TEST(BattleMovementPathModelTest, CapturedCorpusPathVectorsAreBitExact)
{
    struct Case {
        const char* label;
        BattleMovementPathInput input;
        std::vector<MovementGridPosition> expected;
    };
    const std::vector<Case> cases = {
        {
            "147896 action 0 slot 0 second pursuit leg",
            captured_s001_path_input(
                0, 5, {4, 5}, {4, 6}, {6, 3}, {6, 2}, 0x0fc7, 0x0fc7,
                {{4, 2, 0x54}, {4, 5, 0x50}, {6, 2, 0x55},
                 {6, 3, 0x55}, {6, 6, 0x51}}),
            {{5, 4}},
        },
        {
            "147896 action 1 slot 5 first pursuit leg",
            captured_s001_path_input(
                5, 1, {6, 3}, {6, 2}, {6, 7}, {6, 6}, 0x0fc7, 0x0ff7,
                {{4, 2, 0x54}, {5, 3, 0x50}, {5, 4, 0x50},
                 {6, 3, 0x55}, {6, 6, 0x51}, {6, 7, 0x51}}),
            {{6, 4}, {5, 5}, {5, 6}},
        },
        {
            "147896 slot 0",
            captured_s001_path_input(
                0, 4, {5, 4}, {4, 5}, {4, 2}, {0, 0}, 0x0fc7, 0x0fc7,
                {{4, 2, 0x54}, {6, 3, 0x55}, {5, 4, 0x50}, {6, 6, 0x51}}),
            {{5, 3}},
        },
        {
            "147896 slot 5",
            captured_s001_path_input(
                5, 1, {5, 5}, {6, 4}, {7, 7}, {6, 7}, 0x0fc7, 0x0ff7,
                {{5, 3, 0x50}, {5, 5, 0x55}, {7, 7, 0x51}}),
            {{6, 6}},
        },
        {
            "158364 slot 4",
            captured_s001_path_input(
                4, 0, {5, 6}, {4, 5}, {5, 4}, {4, 5}, 0x0fc7, 0x0fc7,
                {{6, 3, 0x55}, {5, 4, 0x50}, {5, 6, 0x54}, {6, 7, 0x51}}),
            {{5, 5}},
        },
        {
            "173344 slot 0",
            captured_s001_path_input(
                0, 5, {4, 4}, {4, 5}, {6, 2}, {0, 0}, 0x0fc7, 0x0fc7,
                {{6, 2, 0x55}, {4, 3, 0x54}, {4, 4, 0x50}, {6, 6, 0x51}}),
            {{5, 4}, {5, 3}},
        },
        {
            "148016 slot 1",
            captured_s001_path_input(
                1, 4, {7, 7}, {6, 7}, {4, 2}, {0, 0}, 0x0ff7, 0x0fc7,
                {{4, 2, 0x54}, {4, 3, 0x50}, {5, 5, 0x55}, {7, 7, 0x51}}),
            {{7, 6}, {7, 5}, {6, 4}, {5, 3}},
        },
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.label);
        const auto result = model_battle_movement_path(test_case.input);
        ASSERT_EQ(result.status, BattleMovementPathModelStatus::Exact);
        ASSERT_EQ(result.entry_count, test_case.expected.size());
        EXPECT_EQ(result.distance, test_case.expected.size());
        for (std::size_t index = 0; index < test_case.expected.size(); ++index) {
            EXPECT_EQ(result.path_entries[index].grid_x, test_case.expected[index].grid_x);
            EXPECT_EQ(result.path_entries[index].grid_z, test_case.expected[index].grid_z);
        }
        EXPECT_TRUE(result.terminator_written);
    }
}

TEST(BattleMovementPathModelTest, CapturedGridMarkerAdjacencyReturnsStatusOne)
{
    const auto input = captured_s001_path_input(
        5, 0, {6, 3}, {6, 2}, {5, 4}, {4, 5}, 0x0fc7, 0x0fc7,
        {{4, 2, 0x54}, {4, 5, 0x50}, {5, 4, 0x50},
         {6, 3, 0x55}, {6, 6, 0x51}});

    const auto result = model_battle_movement_path(input);

    EXPECT_EQ(result.status, BattleMovementPathModelStatus::Exact);
    EXPECT_EQ(result.reachability_status_0x16, 1);
    EXPECT_EQ(result.reachability, MovementReachabilityStatus::Adjacent1);
    EXPECT_FALSE(result.selected_path_node.has_value());
}

TEST(BattleMovementPathModelTest, NextPathingGridSquareDoesNotUseStraightRunIndex)
{
    auto input = simple_path_input({5, 6}, {5, 2});
    input.selection_policy =
        BattleMovementPathSelectionPolicy::NextPathingGridSquare;

    const auto result = model_battle_movement_path(input);

    ASSERT_EQ(result.status, BattleMovementPathModelStatus::Exact);
    ASSERT_GT(result.entry_count, 1u);
    ASSERT_TRUE(result.selected_path_node.has_value());
    EXPECT_EQ(result.selected_path_index, 0);
    EXPECT_EQ(result.selected_path_node->grid_x, result.path_entries[0].grid_x);
    EXPECT_EQ(result.selected_path_node->grid_z, result.path_entries[0].grid_z);
    EXPECT_EQ(
        result.destination_source,
        MovementCommitDestinationSource::GeneratedSingleSquare);
}

TEST(BattleMovementPathModelTest, DeadActorWorksheetCanStillEvaluateLiveTarget)
{
    auto input = simple_path_input({5, 4}, {5, 2});
    input.actor.alive = false;

    const auto result = model_battle_movement_path(input);

    EXPECT_EQ(result.status, BattleMovementPathModelStatus::Exact);
    ASSERT_TRUE(result.selected_path_node.has_value());
    EXPECT_EQ(result.selected_path_node->grid_x, 5);
    EXPECT_EQ(result.selected_path_node->grid_z, 3);
}

TEST(BattleMovementPathModelTest, InitialPathIndexPastTerminatorIsUnsupported)
{
    auto input = simple_path_input({5, 4}, {5, 2});
    input.initial_path_index = 3;

    const auto result = model_battle_movement_path(input);

    EXPECT_EQ(result.status, BattleMovementPathModelStatus::Unsupported);
    EXPECT_FALSE(result.selected_path_node.has_value());
    EXPECT_EQ(result.destination_source, MovementCommitDestinationSource::Unknown);
}

TEST(ActionMotionTargetModelTest, ModesSixAndThirteenSelectOwnPosHolder)
{
    const BattleFrameVec3 pos_holder{.x = 15.0f, .y = 0.0f, .z = -30.0f};
    for (const auto mode : {std::int16_t{0x06}, std::int16_t{0x13}}) {
        const auto result = select_action_motion_target(ActionMotionTargetInput{
            .actor_slot = 0,
            .action_mode = mode,
            .own_pos_holder = pos_holder,
        });
        ASSERT_EQ(result.status, ActionMotionTargetStatus::Exact);
        ASSERT_TRUE(result.target.has_value());
        EXPECT_FLOAT_EQ(result.target->x, pos_holder.x);
        EXPECT_FLOAT_EQ(result.target->z, pos_holder.z);
        EXPECT_EQ(result.source, ActionMotionTargetSource::OwnPosHolder);
    }
}

TEST(ActionMotionTargetModelTest, ExactTargetModeDoesNotGuessMissingCombatant)
{
    const auto missing = select_action_motion_target(ActionMotionTargetInput{
        .actor_slot = 0,
        .target_slot = 4,
        .action_mode = 5,
    });
    EXPECT_EQ(missing.status, ActionMotionTargetStatus::MissingInput);
    EXPECT_FALSE(missing.target.has_value());
}

TEST(ActionMotionTargetModelTest, ReactionModesUseSecondaryInstructionTarget)
{
    const BattleFrameVec3 secondary_target{
        .x = -30.0f,
        .y = 0.0f,
        .z = 45.0f,
    };
    for (const auto mode : {
             std::int16_t{0x09},
             std::int16_t{0x0a},
             std::int16_t{0x0b},
             std::int16_t{0x0d},
             std::int16_t{0x20}}) {
        const auto result = select_action_motion_target(
            ActionMotionTargetInput{
                .actor_slot = 4,
                .target_slot = 5,
                .action_mode = mode,
                .target_current_position =
                    BattleFrameVec3{.x = 1.0f, .y = 2.0f, .z = 3.0f},
                .secondary_target_current_position = secondary_target,
            });
        ASSERT_EQ(result.status, ActionMotionTargetStatus::Exact);
        ASSERT_TRUE(result.target.has_value());
        EXPECT_FLOAT_EQ(result.target->x, secondary_target.x);
        EXPECT_FLOAT_EQ(result.target->z, secondary_target.z);
        EXPECT_EQ(
            result.source,
            ActionMotionTargetSource::
                SecondaryTargetCombatantCurrentPosition);
    }
}

TEST(ActionMotionTargetModelTest, UnsupportedModeUsesOnlyExplicitProvisionalFallback)
{
    const auto fallback = select_action_motion_target(ActionMotionTargetInput{
        .actor_slot = 0,
        .action_mode = 3,
        .provisional_fallback = BattleFrameVec3{.x = 3.0f, .y = 0.0f, .z = 4.0f},
    });
    EXPECT_EQ(fallback.status, ActionMotionTargetStatus::Provisional);
    ASSERT_TRUE(fallback.target.has_value());
    EXPECT_FLOAT_EQ(fallback.target->x, 3.0f);
}

TEST(BattleFrameStateModelTest, CommitUpdatesGridAndPosHolderWithoutMovingCurrentPosition)
{
    std::vector<MovementSlotState> slots = {
        {
            .slot = 0,
            .present = true,
            .is_player = true,
            .alive = true,
            .movement_flags = 0x0fc7,
            .start_position = BattleStartPosition{.present = true, .grid_x = 5, .grid_z = 4},
        },
    };
    const std::array<std::uint8_t, 81> terrain_source_9x9{};
    auto state = initialize_first_battle_frame_state(0, slots, terrain_source_9x9);
    ASSERT_TRUE(state.has_value());
    const auto before = find_frame_combatant(*state, 0)->combatant_cur_pos_0x1c;

    ASSERT_TRUE(commit_movement_grid_8008178c(
        *state,
        0,
        {.grid_x = 5, .grid_z = 3}));

    const auto* after = find_frame_combatant(*state, 0);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->grid_position.grid_z, 3);
    EXPECT_FLOAT_EQ(after->pos_holder.z, -30.0f);
    EXPECT_FLOAT_EQ(after->combatant_cur_pos_0x1c.x, before.x);
    EXPECT_FLOAT_EQ(after->combatant_cur_pos_0x1c.z, before.z);
    EXPECT_FALSE(after->pending_frame_start_position_sync);
    EXPECT_EQ(state->active_grid[static_cast<std::size_t>(4 * 11 + 5)], 0x50);
    EXPECT_EQ(state->active_grid[static_cast<std::size_t>(3 * 11 + 5)], 0x50);
}

} // namespace
} // namespace savor::predict
